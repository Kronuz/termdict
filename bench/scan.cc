// termdict's sequential decode and seeks, on two key sets: strata's KV keys ("Q" and 15
// decimal digits of a scattered id, each value the key's LEB128 ordinal) and a vocabulary
// (one term per line, 6-byte values, as prospect's term infos). Per set, one JSON line:
//   keyscan_ns  a full reader::cursor walk, per key
//   seek10_ns   cursor::seek to a present or absent key, then 10 next(), per seek
//   walk10_ns   the same through reader::scan(from), which builds the floor block's keys
//   fwd_ns      cursor::seek forward 1..256 entries from the last target, per seek
//   near_ns     the same, 1..16 entries
//   find_ns / absent_ns  find() of present and absent keys
// Each figure is the median of REPS repetitions; build twice (two termdict.hh) for an A/B.
// TERMDICT_BENCH_BYTES=1 builds over all 256 bytes (byte-coded suffixes) instead of the
// keys' alphabet.
//   termdict_scan_bench [VOCABULARY [KV_KEYS [REPS [ONLY]]]]  (ONLY: one measurement, for profiling)
#include "termdict.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <concepts>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

using clk = std::chrono::steady_clock;
volatile std::uint64_t sink;

// strata's bench/kv/kvgen.hh: a bijection on [0, 2^49), printed as 15 digits after "Q".
std::uint64_t scatter(std::uint64_t i) {
	constexpr std::uint64_t mask = (std::uint64_t(1) << 49) - 1;
	i &= mask;
	i = (i * 0x9E3779B97F4A7C15ull) & mask;
	i ^= i >> 24;
	i = (i * 0xBF58476D1CE4E5B9ull) & mask;
	i ^= i >> 21;
	return i;
}
std::string kv_key(std::uint64_t id) {
	std::string k(16, 'Q');
	std::uint64_t v = scatter(id);
	for (int i = 15; i >= 1; --i) {
		k[std::size_t(i)] = char('0' + v % 10);
		v /= 10;
	}
	return k;
}

struct dataset {
	const char* name;
	std::vector<std::string> keys, values, absent;
	termdict::tables t;
};

void build(dataset& d) {
	std::string alphabet;
	if (!std::getenv("TERMDICT_BENCH_BYTES")) {  // set: all 256 bytes, suffixes byte-coded
		bool used[256] = {};
		for (const auto& k : d.keys) {
			for (char c : k) { used[std::uint8_t(c)] = true; }
		}
		for (unsigned c = 0; c < 256; ++c) {
			if (used[c]) { alphabet.push_back(char(c)); }
		}
	}
	termdict::builder b(alphabet);
	for (std::size_t i = 0; i < d.keys.size(); ++i) { b.add(d.keys[i], d.values[i]); }
	d.t = b.finish();
}

template <class F>
double median_of(int reps, F&& f) {
	std::vector<double> v;
	for (int i = 0; i < reps; ++i) { v.push_back(f()); }
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

double ns(clk::time_point t0) { return std::chrono::duration<double, std::nano>(clk::now() - t0).count(); }

template <class C>
concept seekable = requires(C& c, std::string_view k) {
	{ c.seek(k) } -> std::same_as<bool>;
};

template <class Cursor>
void run(dataset& d, int reps, std::string_view only) {
	build(d);
	const termdict::reader r(d.t.blocks, d.t.index);
	const std::size_t n = d.keys.size();
	std::mt19937_64 rng(7);
	std::vector<std::string_view> probes, gone;
	for (std::size_t i = 0; i < 100000; ++i) { probes.push_back(d.keys[rng() % n]); }
	for (std::size_t i = 0; i < 100000; ++i) { gone.push_back(d.absent[rng() % d.absent.size()]); }
	std::vector<std::string_view> targets;  // seeks: half present, half absent
	for (std::size_t i = 0; i < 100000; ++i) { targets.push_back(i % 2 ? probes[i] : gone[i]); }

	// Correctness first: the walk returns every key, a seek lands on lower_bound.
	{
		termdict::reader::cursor c(r);
		std::size_t i = 0;
		while (c.next()) {
			if (i >= n || c.key() != d.keys[i] || c.value() != d.values[i]) { std::fprintf(stderr, "walk wrong at %zu\n", i); std::exit(1); }
			++i;
		}
		if (i != n) { std::fprintf(stderr, "walk short\n"); std::exit(1); }
	}
	if constexpr (seekable<Cursor>) {
		Cursor c(r);
		for (std::size_t i = 0; i < 20000; ++i) {
			const auto it = std::lower_bound(d.keys.begin(), d.keys.end(), targets[i]);
			const bool ok = c.seek(targets[i]);
			if (ok != (it != d.keys.end()) || (ok && c.key() != *it)) { std::fprintf(stderr, "seek wrong at %zu\n", i); std::exit(1); }
		}
	}

	// Forward targets: each 1..2^hop entries past the previous, half just above a key.
	auto forward_targets = [&](unsigned hop) {
		std::vector<std::string> ahead;
		for (std::size_t pos = 0; ahead.size() < 100000;) {
			pos += 1 + rng() % (std::size_t(1) << (1 + rng() % hop));
			if (pos >= n) { pos = 0; }
			ahead.push_back(ahead.size() % 2 ? d.keys[pos] : d.keys[pos] + '\0');
		}
		return ahead;
	};
	const auto ahead = forward_targets(8), near = forward_targets(4);
	if constexpr (seekable<Cursor>) {
		for (const auto* set : {&ahead, &near}) {
			Cursor c(r);
			for (const auto& t : *set) {
				const auto it = std::lower_bound(d.keys.begin(), d.keys.end(), t);
				const bool ok = c.seek(t);
				if (ok != (it != d.keys.end()) || (ok && c.key() != *it)) { std::fprintf(stderr, "forward seek wrong\n"); std::exit(1); }
			}
		}
	}

	auto want = [&](std::string_view what) { return only.empty() || only == what; };
	const double keyscan = !want("keyscan") ? 0 : median_of(std::max<int>(reps, int(5000000 / n)), [&] {
		termdict::reader::cursor c(r);
		std::uint64_t s = 0;
		const auto t0 = clk::now();
		while (c.next()) { s += c.key().size() + std::uint8_t(c.value()[0]); }
		const double t = ns(t0);
		sink = sink + s;
		return t / double(n);
	});
	double seek10 = 0;
	if constexpr (seekable<Cursor>) {
		if (want("seek10")) seek10 = median_of(reps, [&] {
			Cursor c(r);
			std::uint64_t s = 0;
			const auto t0 = clk::now();
			for (auto k : targets) {
				if (!c.seek(k)) { continue; }
				s += c.key().size();
				for (int j = 0; j < 10 && c.next(); ++j) { s += c.key().size(); }
			}
			const double t = ns(t0);
			sink = sink + s;
			return t / double(targets.size());
		});
	}
	double fwd = 0, nearns = 0;
	if constexpr (seekable<Cursor>) {
		auto forward = [&](const std::vector<std::string>& set) {
			return median_of(reps, [&] {
				Cursor c(r);
				std::uint64_t s = 0;
				const auto t0 = clk::now();
				for (const auto& k : set) { s += c.seek(k) ? c.key().size() : 0; }
				const double t = ns(t0);
				sink = sink + s;
				return t / double(set.size());
			});
		};
		if (want("fwd")) { fwd = forward(ahead); }
		if (want("near")) { nearns = forward(near); }
	}
	const double walk10 = !want("walk10") ? 0 : median_of(reps, [&] {
		std::uint64_t s = 0;
		const auto t0 = clk::now();
		for (auto k : targets) {
			int left = 11;
			r.scan(k, [&](std::string_view key, std::string_view) {
				s += key.size();
				return --left > 0;
			});
		}
		const double t = ns(t0);
		sink = sink + s;
		return t / double(targets.size());
	});
	const double find = !want("find") ? 0 : median_of(reps, [&] {
		std::uint64_t s = 0;
		const auto t0 = clk::now();
		for (auto k : probes) { s += r.find(k)->size(); }
		const double t = ns(t0);
		sink = sink + s;
		return t / double(probes.size());
	});
	const double absent = !want("absent") ? 0 : median_of(reps, [&] {
		std::uint64_t s = 0;
		const auto t0 = clk::now();
		for (auto k : gone) { s += r.find(k).has_value(); }
		const double t = ns(t0);
		sink = sink + s;
		return t / double(gone.size());
	});
	std::printf("{\"set\":\"%s\",\"keys\":%zu,\"bytes_per_key\":%.3f,\"keyscan_ns\":%.2f,\"seek10_ns\":%.1f,\"fwd_ns\":%.1f,\"near_ns\":%.1f,\"walk10_ns\":%.1f,\"find_ns\":%.1f,\"absent_ns\":%.1f}\n", d.name, n,
		double(d.t.blocks.size() + d.t.index.size()) / double(n), keyscan, seek10, fwd, nearns, walk10, find, absent);
	std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
	const char* vocabulary = argc > 1 ? argv[1] : "/usr/share/dict/words";
	const std::size_t kv_n = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1000000;
	const int reps = argc > 3 ? std::atoi(argv[3]) : 5;
	const std::string_view only = argc > 4 ? argv[4] : "";

	dataset kv{"kv"};
	{
		for (std::uint64_t i = 0; i < kv_n; ++i) { kv.keys.push_back(kv_key(i)); }
		std::sort(kv.keys.begin(), kv.keys.end());
		for (std::size_t i = 0; i < kv.keys.size(); ++i) {
			char buf[varint::leb128::max_bytes];
			kv.values.emplace_back(buf, std::size_t(varint::leb128::encode(buf, i) - buf));
		}
		for (std::uint64_t i = 0; i < 200000; ++i) { kv.absent.push_back(kv_key((std::uint64_t(1) << 48) + i)); }
	}
	dataset terms{"terms"};
	{
		std::ifstream in(vocabulary);
		for (std::string line; std::getline(in, line);) {
			if (!line.empty()) { terms.keys.push_back(line); }
		}
		std::sort(terms.keys.begin(), terms.keys.end());
		terms.keys.erase(std::unique(terms.keys.begin(), terms.keys.end()), terms.keys.end());
		if (terms.keys.empty()) { std::fprintf(stderr, "no vocabulary at %s\n", vocabulary); return 2; }
		std::mt19937_64 rng(5);
		for (std::size_t i = 0; i < terms.keys.size(); ++i) {
			char v[6];
			const std::uint64_t x = rng();
			std::memcpy(v, &x, 6);
			terms.values.emplace_back(v, 6);
			std::string a = terms.keys[i];
			if (rng() & 1) { a.push_back(terms.keys[i].back()); } else { a.back() = char(a.back() ^ 1); }
			if (!std::binary_search(terms.keys.begin(), terms.keys.end(), a)) { terms.absent.push_back(a); }
		}
	}
	run<termdict::reader::cursor>(kv, reps, only);
	run<termdict::reader::cursor>(terms, reps, only);
}
