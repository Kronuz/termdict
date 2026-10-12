// termdict: find and scan against std::map on random and dictionary key sets, block
// boundaries and restarts, corrupt input, and a golden fixture.
#include "termdict.hh"

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

static termdict::tables build(const reference& ref) {
	termdict::builder b;
	for (const auto& [k, v] : ref) { b.add(k, v); }
	return b.finish();
}

static void check(const reference& ref, std::mt19937_64& rng, const std::string& alphabet, int probes) {
	auto t = build(ref);
	termdict::reader r(t.blocks, t.index);
	for (const auto& [k, v] : ref) {
		auto got = r.find(k);
		CHECK(got && *got == v);
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
		std::string alphabet = round % 2 ? "ab" : "abcdefghijklmnopqrstuvwxyz";
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
	// Order is enforced.
	{
		termdict::builder b;
		b.add("b", "");
		bool threw = false;
		try { b.add("a", ""); } catch (const std::invalid_argument&) { threw = true; }
		CHECK(threw);
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
		termdict::builder b;
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
	std::printf("termdict: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
