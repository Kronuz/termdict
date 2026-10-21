// termdict: find and scan against std::map on random and dictionary key sets, block
// boundaries and restarts, corrupt input (truncation and byte-flip sweeps in exact-size heap
// blocks), the suffix decoder against a bit-unpacking oracle, and a golden fixture.
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "fst.hh"
#include "varint.hh"
// The decode sweep drives block::suffixes directly.
#define private public
#include "termdict.hh"
#undef private

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using reference = std::map<std::string, std::string>;

// The keys' alphabet half the time (coded suffixes), all 256 bytes otherwise.
static termdict::tables build(const reference& ref, bool coded = true) {
	std::string alphabet;
	if (coded) {
		for (const auto& [k, v] : ref) { alphabet += k; }
		std::sort(alphabet.begin(), alphabet.end());
		alphabet.erase(std::unique(alphabet.begin(), alphabet.end()), alphabet.end());
	}
	termdict::builder b(alphabet);
	for (const auto& [k, v] : ref) { b.add(k, v); }
	return b.finish();
}

static void check(const reference& ref, std::mt19937_64& rng, const std::string& alphabet, int probes) {
	auto t = build(ref, rng() % 2);
	termdict::reader r(t.blocks, t.index);
	for (const auto& [k, v] : ref) {
		auto got = r.find(k);
		CHECK(got && *got == v);
	}
	{
		termdict::reader::cursor c(r);
		auto w = ref.begin();
		for (; c.next(); ++w) {
			CHECK(w != ref.end());
			if (w == ref.end()) { break; }
			CHECK(c.key() == w->first && c.value() == w->second);
		}
		CHECK(w == ref.end() && !c.next());
	}
	auto random_key = [&] {
		std::string k;
		std::size_t n = rng() % 9;
		for (std::size_t i = 0; i < n; ++i) { k.push_back(alphabet[rng() % alphabet.size()]); }
		return k;
	};
	for (int i = 0; i < probes; ++i) {
		std::string probe = random_key();
		auto it = ref.find(probe);
		auto got = r.find(probe);
		CHECK(bool(got) == (it != ref.end()));
		if (got && it != ref.end()) { CHECK(*got == it->second); }
		reference want(ref.lower_bound(probe), ref.end()), seen;
		std::size_t limit = rng() % 200;
		r.scan(probe, [&](std::string_view k, std::string_view v) {
			seen.emplace(std::string(k), std::string(v));
			return seen.size() < limit;
		});
		reference prefix;
		for (auto w = want.begin(); w != want.end() && prefix.size() < limit; ++w) { prefix.insert(*w); }
		CHECK(seen == (limit ? prefix : reference{}) || (limit == 0 && seen.size() <= 1));
	}
	// One cursor seeking from wherever the last seek and walk left it: each lands on
	// lower_bound, and next() goes on in order from there.
	termdict::reader::cursor c(r);
	for (int i = 0; i < probes; ++i) {
		std::string probe = random_key();
		if (i % 3 == 0 && !ref.empty()) {  // a present key
			auto it = ref.begin();
			std::advance(it, std::ptrdiff_t(rng() % ref.size()));
			probe = it->first;
		}
		auto w = ref.lower_bound(probe);
		CHECK(c.seek(probe) == (w != ref.end()));
		for (std::size_t steps = rng() % 70; w != ref.end(); ++w) {
			CHECK(c.key() == w->first && c.value() == w->second);
			if (steps-- == 0) { break; }
			const bool more = c.next();
			CHECK(more == (std::next(w) != ref.end()));
		}
		if (w == ref.end()) { CHECK(!c.next()); }
	}
}

// Seeks to every key, and to probes just below, just above, and inside each, against
// lower_bound: every block's first and last key, every restart run's first, and past the end.
static void check_seek(const std::vector<std::string>& keys, bool coded) {
	reference ref;
	for (std::size_t i = 0; i < keys.size(); ++i) { ref[keys[i]] = std::to_string(i); }
	auto t = build(ref, coded);
	termdict::reader r(t.blocks, t.index);
	termdict::reader::cursor c(r);
	auto expect = [&](const std::string& probe) {
		auto w = ref.lower_bound(probe);
		const bool found = c.seek(probe);
		CHECK(found == (w != ref.end()));
		if (found && w != ref.end()) {
			CHECK(c.key() == w->first && c.value() == w->second);
			if (std::next(w) != ref.end()) {
				CHECK(c.next() && c.key() == std::next(w)->first);
			} else {
				CHECK(!c.next());
			}
		} else {
			CHECK(!c.next());
		}
	};
	// Forward seeks that stay on one cursor: short hops, run and block crossings.
	for (std::size_t step : {1, 2, 5, 31, 32, 33, 100, 700}) {
		c.seek("");
		for (std::size_t i = 0; i < keys.size(); i += step) {
			expect(i % 2 ? keys[i] : keys[i] + '\0');
		}
	}
	expect("");
	expect(std::string(1, '\xff') + keys.back());
	expect(keys.back() + "\xff");
	for (const auto& k : keys) {
		expect(k);
		expect(k + '\0');
		expect(k + "\xff");
		expect(k.substr(0, k.size() - 1));
		std::string below = k;
		below.back() = char(std::uint8_t(below.back()) - 1);
		expect(below);
		expect(below + "\xff\xff");
	}
	// Backward and forward from the end.
	CHECK(!c.seek(keys.back() + "\xff") && !c.next());
	CHECK(c.seek(keys.front()) && c.key() == ref.begin()->first);
}


// Forward seeks stay in the open block exactly when the target sorts between the current key
// and the block's last key: a counting verifier sees every block the cursor opens. Targets
// are each block's keys, restart keys, last key, one past it, and the next block's first key,
// from every position in the block, checked against std::lower_bound; backward targets too.
static void check_forward(const std::vector<std::string>& keys, bool coded) {
	reference ref;
	for (std::size_t i = 0; i < keys.size(); ++i) { ref[keys[i]] = std::to_string(i); }
	auto t = build(ref, coded);
	std::size_t verifies = 0;
	termdict::reader r(t.blocks, t.index, [](void* n, std::uint64_t, std::uint64_t) { ++*static_cast<std::size_t*>(n); return true; }, &verifies);
	std::vector<std::size_t> first;  // each block's first key's position
	{
		termdict::reader::cursor c(r);
		std::size_t seen = verifies, i = 0;
		while (c.next()) {
			if (verifies != seen) { first.push_back(i); seen = verifies; }
			CHECK(c.key() == keys[i]);
			++i;
		}
		CHECK(i == keys.size());
	}
	first.push_back(keys.size());
	termdict::reader::cursor c(r);
	// Seeks to target from the entry at s; true when it landed inside the block ending at last.
	auto probe = [&](std::size_t s, const std::string& target, std::size_t last) {
		CHECK(c.seek(keys[s]) && c.key() == keys[s]);
		const std::size_t before = verifies;
		const auto w = std::lower_bound(keys.begin(), keys.end(), target);
		const bool found = c.seek(target);
		CHECK(found == (w != keys.end()));
		if (found && w != keys.end()) { CHECK(c.key() == *w && c.value() == ref[*w]); }
		const bool inside = target >= keys[s] && target <= keys[last];
		CHECK((verifies == before) == inside);
		if (found) {
			const bool more = c.next();
			CHECK(more == (std::next(w) != keys.end()));
			if (more) { CHECK(c.key() == *std::next(w)); }
		} else {
			CHECK(!c.next());
		}
	};
	for (std::size_t b = 0; b + 1 < first.size(); ++b) {
		const std::size_t f = first[b], last = first[b + 1] - 1;
		for (std::size_t s = f; s <= last; ++s) {
			std::vector<std::size_t> to;  // positions whose key and key + '\0' are targets
			for (std::size_t j = s; j <= last; ++j) { to.push_back(j); }
			for (std::size_t j : to) {
				probe(s, keys[j], last);
				probe(s, keys[j] + '\0', last);
			}
			if (last + 1 < keys.size()) {  // the next block's first key, and a hop past it
				probe(s, keys[last + 1], last);
				probe(s, keys[std::min(keys.size() - 1, last + 40)], last);
			}
			probe(s, keys[last] + "\xff", last);
			if (s > 0) {  // backwards: below the current key, in and out of the block
				probe(s, keys[s - 1], last);
				probe(s, keys[s - 1] + '\0', last);
				probe(s, keys[f], last);
			}
			probe(s, "", last);
			probe(s, keys[s], last);  // equal to the current key
		}
	}
	// A failed seek leaves the cursor past the end, not on a stale entry.
	for (std::size_t j : {std::size_t(1), keys.size() / 2, keys.size() - 1}) {
		if (j >= keys.size()) { continue; }
		CHECK(c.seek(keys[0]));
		CHECK(!c.seek(keys.back() + "\xff") && !c.next());
		CHECK(c.seek(keys[j]) && c.key() == keys[j]);
		CHECK(c.seek(keys.back()) && c.key() == keys.back() && !c.next());
		CHECK(!c.seek(keys.back() + '\0') && !c.next());
		CHECK(c.seek(keys[j]) && c.key() == keys[j]);
	}
	// Random hops over one cursor against lower_bound.
	std::mt19937_64 rng(keys.size() * 31 + coded);
	std::size_t pos = 0;
	for (int i = 0; i < 20000; ++i) {
		pos = rng() % 5 == 0 ? rng() % keys.size() : std::min(keys.size() - 1, pos + rng() % (rng() % 4 ? 20 : 200));
		std::string target = keys[pos];
		switch (rng() % 4) {
		case 1: target += '\0'; break;
		case 2: target.resize(target.size() / 2); break;
		case 3: if (!target.empty()) { target.back() = char(std::uint8_t(target.back()) + 1); } break;
		default: break;
		}
		const auto w = std::lower_bound(keys.begin(), keys.end(), target);
		const bool found = c.seek(target);
		CHECK(found == (w != keys.end()));
		if (found && w != keys.end()) { CHECK(c.key() == *w); pos = std::size_t(w - keys.begin()); }
	}
}


// Runs fn over a copy of bytes in an exact-size heap block, so ASan sees any read past its end.
template <class F>
static void on_exact(std::string_view bytes, F&& fn) {
	char* m = static_cast<char*>(std::malloc(bytes.size() ? bytes.size() : 1));
	std::memcpy(m, bytes.data(), bytes.size());
	try { fn(std::string_view(m, bytes.size())); } catch (const termdict::corrupt&) {}
	std::free(m);
}

// Drives cursor next/seek, scan and find over bytes; only termdict::corrupt may escape.
static void drive(std::string_view bytes, const std::string& index, const std::vector<std::string>& probes) {
	termdict::reader r(bytes, index);
	{ termdict::reader::cursor c(r); try { while (c.next()) { (void)c.key(); (void)c.value(); } } catch (const termdict::corrupt&) {} }
	for (const auto& p : probes) {
		try { termdict::reader::cursor c(r); if (c.seek(p)) { for (int s = 0; s < 40 && c.next(); ++s) { (void)c.key(); } } } catch (const termdict::corrupt&) {}
		try { int n = 0; r.scan(p, [&](std::string_view, std::string_view) { return ++n < 40; }); } catch (const termdict::corrupt&) {}
		try { (void)r.find(p); } catch (const termdict::corrupt&) {}
	}
}

// Every truncation length and random byte flips of several dictionaries, in exact-size heap blocks.
static void sweep_corruption(std::mt19937_64& rng) {
	for (int d = 0; d < 6; ++d) {
		reference ref;
		const std::string alpha = d % 2 ? "abcdefghijklmnopqrstuvwxyz0123456789" : "abc";
		const int n = d < 2 ? 40 : 120;
		for (int i = 0; i < n; ++i) {
			std::string k;
			for (std::size_t j = 1 + rng() % 12; j; --j) { k.push_back(alpha[rng() % alpha.size()]); }
			ref[k] = std::string(rng() % 6, 'v') + std::to_string(i);
		}
		auto t = build(ref, d < 4);
		std::vector<std::string> probes{"", "a", std::string(1, '\xff')};
		auto it = ref.begin();
		for (std::size_t i = 0; i < ref.size(); i += 1 + ref.size() / 6, it = std::next(ref.begin(), std::ptrdiff_t(i % ref.size()))) { probes.push_back(it->first); probes.push_back(it->first + '\0'); }
		for (std::size_t len = 0; len <= t.blocks.size(); ++len) {
			on_exact(std::string_view(t.blocks).substr(0, len), [&](std::string_view b) { drive(b, t.index, probes); });
		}
		for (int m = 0; m < 200 && !t.blocks.empty(); ++m) {
			std::string bad = t.blocks;
			for (int f = 1 + int(rng() % 4); f; --f) { bad[rng() % bad.size()] = char(rng()); }
			if (rng() % 4 == 0) { bad.resize(rng() % bad.size()); }
			on_exact(bad, [&](std::string_view b) { drive(b, t.index, probes); });
		}
	}
}

// decode and decode_padded against a bit-unpacking oracle, every width and every alphabet size
// with that width, exact-size heap source and destination, garbage past the suffix.
static void sweep_decode(std::mt19937_64& rng) {
	for (unsigned W = 1; W <= 8; ++W) {
		std::vector<unsigned> sizes;
		if (W == 1) { sizes = {1, 2}; } else { for (unsigned s = (1u << (W - 1)) + 1; s <= (1u << W); ++s) { sizes.push_back(s); } }
		for (unsigned size : sizes) {
			termdict::alphabet a;
			a.size = size;
			a.width = W;
			for (unsigned i = 0; i < 256; ++i) { a.symbol[i] = char(size == 256 ? i : i < size ? (i * 5 + 1) : 0); }
			CHECK(std::bit_width(size - 1) == int(W) || size == 1);
			for (int it = 0; it < 400; ++it) {
				const unsigned pre = rng() % 11, n = rng() % 50;
				const std::size_t bytes = ((pre + n) * W + 7) / 8;
				const std::size_t total = bytes + (rng() % 3 == 0 ? rng() % 20 : 0);
				char* src = static_cast<char*>(std::malloc(total ? total : 1));
				for (std::size_t i = 0; i < total; ++i) { src[i] = char(rng()); }
				auto put = [&](std::size_t sym, unsigned code) {
					for (unsigned b = 0; b < W; ++b) {
						const std::size_t bit = sym * W + b;
						src[bit / 8] = char((code >> b) & 1 ? src[bit / 8] | (1 << (bit % 8)) : src[bit / 8] & ~(1 << (bit % 8)));
					}
				};
				std::vector<unsigned> codes(n);
				bool invalid = false;
				for (unsigned i = 0; i < n; ++i) {
					unsigned c = rng() % size;
					if (size < (1u << W) && rng() % 32 == 0) { c = size + rng() % ((1u << W) - size); invalid = true; }
					codes[i] = c;
					put(pre + i, c);
				}
				for (unsigned i = 0; i < pre; ++i) { put(i, rng() % (1u << W)); }
				for (int padded = 0; padded < 2; ++padded) {
					termdict::block::suffixes s(a, src, bytes, src + total);
					s.skip(pre);
					s.start(n);
					const std::size_t cap = n + (padded ? 16 : 0);
					char* out = static_cast<char*>(std::malloc(cap ? cap : 1));
					std::memset(out, 0x5a, cap);
					bool threw = false;
					try { if (padded) { s.decode_padded(n, out); } else { s.decode(n, out); } } catch (const termdict::corrupt&) { threw = true; }
					CHECK(threw == invalid);
					if (!threw) {
						for (unsigned i = 0; i < n; ++i) { if (out[i] != a.symbol[codes[i]]) { CHECK(!"decoded symbol"); break; } }
						if (!padded) { for (std::size_t i = n; i < cap; ++i) { CHECK(out[i] == 0x5a); } }
						CHECK(s.at_ == std::uint64_t(pre + n) * W);
					}
					std::free(out);
				}
				std::free(src);
			}
		}
	}
}

int main() {
	std::mt19937_64 rng(42);
	for (int round = 0; round < 200; ++round) {
		static const char wide[] = "\x01\x7f\x80\xff" "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-.";
		const char* alphabets[] = {"ab", "abcdefghijklmnopqrstuvwxyz", "a"};
		std::string alphabet = round % 4 == 3 ? std::string(wide, sizeof wide - 1) : alphabets[round % 3];
		reference ref;
		std::size_t n = rng() % 3000;
		for (std::size_t i = 0; i < n; ++i) {
			std::string k;
			std::size_t len = 1 + rng() % 8;
			for (std::size_t j = 0; j < len; ++j) { k.push_back(alphabet[rng() % alphabet.size()]); }
			std::string v(rng() % (round % 7 == 0 ? 600 : 12), char('A' + rng() % 26));
			ref[k] = v;
		}
		check(ref, rng, alphabet, 200);
	}
	// Empty dictionary.
	{
		auto t = termdict::builder{}.finish();
		termdict::reader r(t.blocks, t.index);
		CHECK(!r.find("x"));
		std::size_t n = 0;
		r.scan("", [&](std::string_view, std::string_view) { return ++n > 0; });
		CHECK(n == 0);
		termdict::reader::cursor c(r);
		CHECK(!c.seek("") && !c.seek("x") && !c.next());
	}
	// Seeks at every key and block boundary: keys sharing long prefixes, keys of one byte,
	// one key, and keys that are prefixes of the next.
	{
		std::vector<std::string> sets[4];
		for (int i = 0; i < 3000; ++i) {
			char k[16];
			std::snprintf(k, sizeof k, "k%06d", i * 7);
			sets[0].push_back(k);
		}
		for (int c = 'b'; c <= 'y'; ++c) { sets[1].push_back(std::string(1, char(c))); }
		sets[2].push_back("only");
		for (int i = 1; i <= 200; ++i) { sets[3].push_back(std::string(std::size_t(i), 'p')); }
		for (const auto& keys : sets) {
			for (bool coded : {false, true}) {
				check_seek(keys, coded);
				check_forward(keys, coded);
			}
		}
	}
	// A suffix code past the alphabet is corrupt, at every symbol position and width, and the
	// largest valid codes beside it are not: one key of n symbols, each the alphabet's
	// largest code (size - 1), then one symbol raised to 2^width - 1.
	for (unsigned width = 1; width <= 8; ++width) {
		const unsigned size = width == 1 ? 1 : (1u << width) - 1;
		std::string alphabet;
		for (unsigned i = 1; i <= size; ++i) { alphabet.push_back(char(i)); }
		for (std::size_t n = 1; n <= 40; ++n) {
			termdict::builder b(alphabet);
			const std::string key(n, alphabet.back());
			b.add(key, "");
			auto t = b.finish();
			CHECK(t.blocks.size() < 128 && t.blocks[4] == char((n * width + 7) / 8));  // one-byte lengths
			{
				termdict::reader r(t.blocks, t.index);
				termdict::reader::cursor c(r);
				CHECK(c.next() && c.key() == key);
				CHECK(c.seek(key) && c.key() == key);
			}
			for (std::size_t j = 0; j < n; ++j) {
				std::string bad = t.blocks;
				for (std::size_t bit = j * width; bit < (j + 1) * width; ++bit) { bad[5 + bit / 8] = char(bad[5 + bit / 8] | (1 << (bit % 8))); }
				termdict::reader r(bad, t.index);
				int threw = 0;
				try {
					termdict::reader::cursor c(r);
					(void)c.next();
				} catch (const termdict::corrupt&) { ++threw; }
				try {
					termdict::reader::cursor c(r);
					(void)c.seek("");
				} catch (const termdict::corrupt&) { ++threw; }
				try {
					termdict::alphabet a;
					(void)a.parse(t.index);
					termdict::block blk(bad, 0, a);
					termdict::block::cursor bc(blk, 0);
					(void)bc.next();
				} catch (const termdict::corrupt&) { ++threw; }
				CHECK(threw == 3);
			}
		}
	}
	// A key byte outside the alphabet is refused.
	{
		termdict::builder b("abc");
		b.add("abc", "1");
		bool threw = false;
		try { b.add("abd", ""); } catch (const std::invalid_argument&) { threw = true; }
		CHECK(threw);
	}
	// A malformed alphabet in the index (unsorted, or longer than the bytes) is corrupt.
	{
		reference ref{{"ab", "1"}, {"ba", "2"}};
		auto t = build(ref);
		for (std::string bad : {std::string("\x02" "ba") + t.index.substr(3), std::string("\x7f")}) {
			bool threw = false;
			try { termdict::reader r(t.blocks, bad); } catch (const termdict::corrupt&) { threw = true; }
			CHECK(threw);
		}
		termdict::reader r(t.blocks, t.index);
		CHECK(r.alphabet_bytes() == "ab" && r.find("ba") && *r.find("ba") == "2");
	}
	// Long keys (past Lucene's and Tantivy's term limits) round-trip, sharing prefixes and
	// not.
	{
		reference ref;
		for (std::size_t len : {std::size_t(14), std::size_t(15), std::size_t(65531), std::size_t(1) << 20}) {
			ref[std::string(len, 'a') + "b"] = "s" + std::to_string(len);
			ref[std::string(len, 'c')] = "u" + std::to_string(len);
		}
		for (bool coded : {false, true}) {
			auto t = build(ref, coded);
			termdict::reader r(t.blocks, t.index);
			for (const auto& [k, v] : ref) { CHECK(r.find(k) && *r.find(k) == v); }
			std::size_t n = 0;
			r.scan("", [&](std::string_view k, std::string_view v) { CHECK(ref.at(std::string(k)) == v); return ++n > 0; });
			CHECK(n == ref.size());
		}
		static_assert(termdict::max_key_bytes == fst::max_key_length);
	}
	// Order is enforced.
	{
		termdict::builder b;
		b.add("b", "");
		bool threw = false;
		try { b.add("a", ""); } catch (const std::invalid_argument&) { threw = true; }
		CHECK(threw);
	}
	// A verifier sees every block byte before it is read; a rejected block throws corrupt.
	{
		termdict::builder b;
		for (int i = 0; i < 5000; ++i) {
			char k[16];
			std::snprintf(k, sizeof k, "k%06d", i);
			b.add(k, std::string(1 + i % 5, 'v'));
		}
		auto t = b.finish();
		struct coverage {
			std::vector<bool> seen;
			std::uint64_t reject = ~0ull;  // reject ranges starting here
		} cov{std::vector<bool>(t.blocks.size()), ~0ull};
		auto verify = [](void* ctx, std::uint64_t offset, std::uint64_t length) {
			auto* c = static_cast<coverage*>(ctx);
			if (offset == c->reject) { return false; }
			for (std::uint64_t i = offset; i < offset + length; ++i) { c->seen[i] = true; }
			return true;
		};
		termdict::reader r(t.blocks, t.index, verify, &cov);
		std::size_t n = 0;
		r.scan("", [&](std::string_view, std::string_view) { return ++n > 0; });
		CHECK(n == 5000);
		CHECK(std::all_of(cov.seen.begin(), cov.seen.end(), [](bool v) { return v; }));
		CHECK(r.find("k002500") && *r.find("k002500") == std::string(1, 'v'));
		cov.reject = 0;  // the first block
		bool threw = false;
		try { (void)r.find("k000001"); } catch (const termdict::corrupt&) { threw = true; }
		CHECK(threw);
		CHECK(r.find("k004999"));  // other blocks still answer
	}
	// Corrupt block bytes throw termdict::corrupt (or answer) without leaving the buffer.
	{
		reference ref;
		for (int i = 0; i < 2000; ++i) { ref["t" + std::to_string(i * 7919 % 100003)] = std::to_string(i); }
		auto t = build(ref);
		for (std::size_t cut = 0; cut < t.blocks.size(); cut += 1 + t.blocks.size() / 200) {
			std::string truncated = t.blocks.substr(0, cut);
			termdict::reader r(truncated, t.index);
			for (int q = 0; q < 20; ++q) {
				try { (void)r.find("t" + std::to_string(rng() % 100003)); } catch (const termdict::corrupt&) {}
			}
		}
		for (int round = 0; round < 20000; ++round) {
			std::string mutated = t.blocks;
			mutated[rng() % mutated.size()] = char(rng());
			termdict::reader r(mutated, t.index);
			try {
				for (int q = 0; q < 10; ++q) { (void)r.find("t" + std::to_string(rng() % 100003)); }
				std::size_t n = 0;
				r.scan("t5", [&](std::string_view, std::string_view) { return ++n < 300; });
			} catch (const termdict::corrupt&) {
			}
		}
	}
	sweep_corruption(rng);
	sweep_decode(rng);
	// Golden fixture.
	{
		termdict::builder b("key0123456789");
		for (int i = 0; i < 120; ++i) { b.add("key" + std::to_string(1000 + i), "v" + std::to_string(i * i)); }
		auto t = b.finish();
		std::string bytes = t.blocks + '\x00' + t.index;
		const char* path = TERMDICT_FIXTURES "/small.td";
		if (std::getenv("TERMDICT_WRITE_FIXTURES")) {
			std::FILE* f = std::fopen(path, "wb");
			std::fwrite(bytes.data(), 1, bytes.size(), f);
			std::fclose(f);
		}
		std::string golden;
		if (std::FILE* f = std::fopen(path, "rb")) {
			for (int c; (c = std::fgetc(f)) != EOF;) { golden.push_back(char(c)); }
			std::fclose(f);
		}
		CHECK(golden == bytes);
	}
	// A real vocabulary.
	if (std::ifstream in("/usr/share/dict/words"); in) {
		reference ref;
		std::size_t i = 0;
		for (std::string w; std::getline(in, w);) { if (!w.empty()) { ref.emplace(w, std::to_string(i++)); } }
		check(ref, rng, "abcdefghijklmnopqrstuvwxyzAB", 300);
	}
	// Restart runs: each value records whether the builder said it starts a run and its
	// index; find_run replays the run from its first entry through the key's, in order.
	{
		termdict::builder b;
		std::vector<std::string> keys;
		for (int i = 0; i < 2000; ++i) {
			char k[16];
			std::snprintf(k, sizeof k, "key%06d", i * 3);
			keys.push_back(k);
		}
		for (std::size_t i = 0; i < keys.size(); ++i) { b.add(keys[i], (b.next_is_restart() ? "R" : "-") + std::to_string(i)); }
		auto t = b.finish();
		termdict::reader r(t.blocks, t.index);
		for (std::size_t i = 0; i < keys.size(); ++i) {
			std::vector<std::string> run;
			auto v = r.find_run(keys[i], [&](std::string_view value) { run.emplace_back(value); });
			CHECK(v && *v == run.back());
			CHECK(!run.empty() && run.front()[0] == 'R');
			for (std::size_t j = 1; j < run.size(); ++j) { CHECK(run[j][0] == '-'); }
			CHECK(std::stoul(run.back().substr(1)) == i && std::stoul(run.front().substr(1)) == i + 1 - run.size());
		}
		std::size_t visited = 0;
		CHECK(!r.find_run("key000001", [&](std::string_view) { ++visited; }) && visited >= 1);
	}
	std::printf("termdict: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
