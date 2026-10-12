// termdict lookups on a vocabulary (one key per line): present and absent keys, against
// an open-addressing hash map and a full-vocabulary FST; size per key for each. Medians
// of batches of 1,024 lookups, then p50 and p99 of single lookups.
//   termdict_bench KEYS_FILE
#include "termdict.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <utility>
#include <vector>

using clock_type = std::chrono::steady_clock;

class hash_map {
public:
	explicit hash_map(const std::vector<std::string>& keys) {
		std::size_t cap = 1;
		while (cap < keys.size() * 2) { cap <<= 1; }
		slots_.assign(cap, slot{});
		mask_ = cap - 1;
		for (std::size_t i = 0; i < keys.size(); ++i) {
			std::uint32_t off = std::uint32_t(arena_.size());
			arena_ += keys[i];
			std::size_t h = hash(keys[i]);
			std::size_t at = h & mask_;
			while (slots_[at].len) { at = (at + 1) & mask_; }
			slots_[at] = slot{off, std::uint32_t(keys[i].size()), std::uint32_t(h >> 32), i};
		}
	}
	bool contains(std::string_view key) const noexcept {
		std::size_t h = hash(key);
		for (std::size_t at = h & mask_;; at = (at + 1) & mask_) {
			const slot& s = slots_[at];
			if (!s.len) { return false; }
			if (s.tag == std::uint32_t(h >> 32) && s.len == key.size() && std::memcmp(arena_.data() + s.off, key.data(), key.size()) == 0) { return true; }
		}
	}
	std::size_t bytes() const noexcept { return slots_.size() * sizeof(slot) + arena_.size(); }

private:
	struct slot { std::uint32_t off = 0, len = 0, tag = 0; std::uint64_t value = 0; };
	static std::size_t hash(std::string_view k) noexcept {
		std::uint64_t h = 1469598103934665603ull;
		for (unsigned char c : k) { h = (h ^ c) * 1099511628211ull; }
		return std::size_t(h);
	}
	std::vector<slot> slots_;
	std::string arena_;
	std::size_t mask_ = 0;
};

template <typename F>
static double median_ns(const std::vector<std::string>& queries, F&& f) {
	std::vector<double> batches;
	volatile std::size_t sink = 0;
	for (std::size_t start = 0; start + 1024 <= queries.size() && batches.size() < 1500; start += 1024) {
		auto t = clock_type::now();
		for (std::size_t i = start; i < start + 1024; ++i) { sink = sink + f(queries[i]); }
		batches.push_back(std::chrono::duration<double, std::nano>(clock_type::now() - t).count() / 1024);
	}
	std::nth_element(batches.begin(), batches.begin() + batches.size() / 2, batches.end());
	return batches[batches.size() / 2];
}

// p50 and p99 of single lookups, each timed alone less the timer's own median cost.
template <typename F>
static std::pair<double, double> percentiles_ns(const std::vector<std::string>& queries, F&& f) {
	volatile std::size_t sink = 0;
	std::vector<double> empty, t;
	for (int i = 0; i < 100000; ++i) {
		auto a = clock_type::now();
		empty.push_back(std::chrono::duration<double, std::nano>(clock_type::now() - a).count());
	}
	std::nth_element(empty.begin(), empty.begin() + empty.size() / 2, empty.end());
	const double overhead = empty[empty.size() / 2];
	t.reserve(queries.size());
	for (const auto& q : queries) {
		auto a = clock_type::now();
		sink = sink + f(q);
		t.push_back(std::chrono::duration<double, std::nano>(clock_type::now() - a).count() - overhead);
	}
	auto at = [&](double p) {
		auto k = t.begin() + std::ptrdiff_t(p * double(t.size() - 1));
		std::nth_element(t.begin(), k, t.end());
		return *k;
	};
	return {at(0.50), at(0.99)};
}

int main(int argc, char** argv) {
	std::ifstream in(argc > 1 ? argv[1] : "/usr/share/dict/words");
	std::vector<std::string> keys;
	for (std::string line; std::getline(in, line);) { if (!line.empty()) { keys.push_back(line); } }
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

	termdict::builder b;
	fst::builder fb;
	for (std::size_t i = 0; i < keys.size(); ++i) {
		char value[6];
		std::memcpy(value, &i, 6);  // a term-info-sized value
		b.add(keys[i], std::string_view(value, 6));
		fb.insert(keys[i], i);
	}
	auto t = b.finish();
	termdict::reader r(t.blocks, t.index);
	std::string fst_bytes = fb.finish();
	fst::map full(fst_bytes);
	hash_map h(keys);

	std::mt19937_64 rng(5);
	std::vector<std::string> present(keys), absent;
	std::shuffle(present.begin(), present.end(), rng);
	if (present.size() > 1500 * 1024) { present.resize(1500 * 1024); }
	for (const auto& k : present) {
		std::string a = k;
		if (rng() & 1) { a.push_back('q'); } else { a.back() = char(a.back() ^ 1); }
		if (!std::binary_search(keys.begin(), keys.end(), a)) { absent.push_back(a); }
	}

	std::printf("%zu keys; termdict %.2f B/key (blocks %zu + index %zu), fst %.2f B/key, hash map %.2f B/key\n", keys.size(),
		double(t.blocks.size() + t.index.size()) / double(keys.size()), t.blocks.size(), t.index.size(),
		double(fst_bytes.size()) / double(keys.size()), double(h.bytes()) / double(keys.size()));
	std::printf("%-10s %14s %14s %14s\n", "", "termdict ns", "fst get ns", "hash map ns");
	for (int round = 0; round < 2; ++round) {
		double tp = median_ns(present, [&](const std::string& k) { return r.find(k).has_value(); });
		double fp = median_ns(present, [&](const std::string& k) { return full.get(k).has_value(); });
		double hp = median_ns(present, [&](const std::string& k) { return h.contains(k); });
		double ta = median_ns(absent, [&](const std::string& k) { return r.find(k).has_value(); });
		double fa = median_ns(absent, [&](const std::string& k) { return full.get(k).has_value(); });
		double ha = median_ns(absent, [&](const std::string& k) { return h.contains(k); });
		std::printf("%-10s %14.1f %14.1f %14.1f\n%-10s %14.1f %14.1f %14.1f\n", "present", tp, fp, hp, "absent", ta, fa, ha);
	}
	std::printf("%-10s %14s %14s %14s\n", "single", "termdict p50/p99", "fst p50/p99", "hash map p50/p99");
	for (const auto* set : {&present, &absent}) {
		auto tq = percentiles_ns(*set, [&](const std::string& k) { return r.find(k).has_value(); });
		auto fq = percentiles_ns(*set, [&](const std::string& k) { return full.get(k).has_value(); });
		auto hq = percentiles_ns(*set, [&](const std::string& k) { return h.contains(k); });
		std::printf("%-10s %7.0f/%-6.0f %7.0f/%-6.0f %7.0f/%-6.0f\n", set == &present ? "present" : "absent", tq.first, tq.second, fq.first, fq.second, hq.first, hq.second);
	}
}
