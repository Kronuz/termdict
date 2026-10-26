// The cost of opening a dictionary: reader construction (alphabet, then the FST's validation
// pass) against the FST mapped without validation, as time and heap allocations.
//   termdict_open_bench build VOCABULARY TABLES   builds the tables for a vocabulary (one term
//                                                 per line, 6-byte values) and saves them
//   termdict_open_bench time TABLES [REPS]        times opens of saved tables; the median
//                                                 of REPS (default 21), the two in turn
#include "termdict.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <vector>

namespace {

// Heap accounting: every allocation carries a 16-byte header holding its size.
std::size_t calls = 0, live = 0, peak = 0, total = 0;

}  // namespace

// TSan's runtime defines the replaceable operators itself; there the counts stay zero.
#if defined(__SANITIZE_THREAD__)
#define OPEN_BENCH_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define OPEN_BENCH_TSAN 1
#endif
#endif

#ifndef OPEN_BENCH_TSAN
void* operator new(std::size_t n) {
	void* p = std::malloc(n + 16);
	if (!p) { throw std::bad_alloc(); }
	*static_cast<std::size_t*>(p) = n;
	++calls;
	total += n;
	live += n;
	peak = std::max(peak, live);
	return static_cast<char*>(p) + 16;
}
void operator delete(void* p) noexcept {
	if (!p) { return; }
	char* base = static_cast<char*>(p) - 16;
	live -= *reinterpret_cast<std::size_t*>(base);
	std::free(base);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
#endif

namespace {

using clk = std::chrono::steady_clock;
volatile std::uint64_t sink;

std::string slurp(const char* path) {
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), {}};
}

int build(const char* vocabulary, const char* out) {
	std::vector<std::string> keys;
	{
		std::ifstream in(vocabulary);
		for (std::string w; std::getline(in, w);) { if (!w.empty()) { keys.push_back(w); } }
	}
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	std::string alphabet;
	for (const auto& k : keys) { alphabet += k; }
	std::sort(alphabet.begin(), alphabet.end());
	alphabet.erase(std::unique(alphabet.begin(), alphabet.end()), alphabet.end());
	termdict::builder b(alphabet);
	for (std::size_t i = 0; i < keys.size(); ++i) {
		char value[6];
		std::memcpy(value, &i, 6);
		b.add(keys[i], std::string_view(value, 6));
	}
	auto t = b.finish();
	std::ofstream f(out, std::ios::binary);
	const std::uint64_t n = t.blocks.size();
	f.write(reinterpret_cast<const char*>(&n), 8);
	f << t.blocks << t.index;
	std::printf("%zu keys, blocks %zu bytes, index %zu bytes\n", keys.size(), t.blocks.size(), t.index.size());
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc == 4 && std::strcmp(argv[1], "build") == 0) { return build(argv[2], argv[3]); }
	if (argc < 3 || std::strcmp(argv[1], "time") != 0) { return 2; }
	const std::string bytes = slurp(argv[2]);
	const int reps = argc > 3 ? std::atoi(argv[3]) : 21;
	std::uint64_t n;
	std::memcpy(&n, bytes.data(), 8);
	const std::string_view blocks(bytes.data() + 8, n), index(bytes.data() + 8 + n, bytes.size() - 8 - n);

	std::vector<double> validated, trusted;
	std::size_t v_calls = 0, v_total = 0, v_peak = 0;
	for (int rep = 0; rep < reps; ++rep) {
		{
			calls = total = 0;
			peak = live;
			const std::size_t base = live;
			auto t0 = clk::now();
			termdict::reader r(blocks, index);
			auto t1 = clk::now();
			sink = r.alphabet_bytes().size();
			validated.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
			v_calls = calls;
			v_total = total;
			v_peak = peak - base;
		}
		{
			termdict::alphabet a;
			const std::string_view fst_bytes = a.parse(index);
			auto t0 = clk::now();
			fst::map m(fst_bytes, fst::trusted);
			auto t1 = clk::now();
			sink = m.size();
			trusted.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
		}
	}
	std::sort(validated.begin(), validated.end());
	std::sort(trusted.begin(), trusted.end());
	std::printf("index %zu bytes; open (alphabet + FST validation) %.3f ms median (min %.3f, max %.3f); %zu allocations, %zu bytes total, %zu bytes peak; FST mapped without validation %.5f ms\n",
		index.size(), validated[validated.size() / 2], validated.front(), validated.back(), v_calls, v_total, v_peak, trusted[trusted.size() / 2]);
}
