// termdict: find and scan against std::map on random and dictionary key sets, block
// boundaries and restarts, corrupt input, and a golden fixture.
#include "termdict.hh"

#include <algorithm>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <random>

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
