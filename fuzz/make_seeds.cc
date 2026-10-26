// Writes fuzz_open's seed inputs ([u32 LE blocks size][blocks][index]) into the directory
// given: built dictionaries over several key sets and alphabets.
//   termdict_make_seeds DIR
#include "termdict.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void write(const std::string& dir, int n, const termdict::tables& t) {
	std::string out;
	const std::uint32_t size = std::uint32_t(t.blocks.size());
	for (int i = 0; i < 4; ++i) { out.push_back(char(size >> (8 * i))); }
	out += t.blocks;
	out += t.index;
	std::FILE* f = std::fopen((dir + "/seed" + std::to_string(n)).c_str(), "wb");
	if (!f) { std::perror("open"); std::exit(1); }
	std::fwrite(out.data(), 1, out.size(), f);
	std::fclose(f);
}

int main(int argc, char** argv) {
	if (argc != 2) { return 2; }
	int n = 0;
	auto build = [&](const std::string& alphabet, const std::vector<std::string>& keys) {
		termdict::builder b(alphabet);
		for (std::size_t i = 0; i < keys.size(); ++i) { b.add(keys[i], std::string(i % 7, 'v') + std::to_string(i)); }
		write(argv[1], n++, b.finish());
	};
	build("", {"a"});
	build("a", {"a"});
	build("ab", {"a", "ab", "b"});
	build("", {std::string(), "\x01", "a", "ab", "b"});
	std::vector<std::string> nums, words;
	for (int i = 0; i < 40; ++i) { nums.push_back("k" + std::to_string(100 + i)); }
	build("", nums);
	build("k0123456789", nums);
	for (char a = 'a'; a <= 'z'; ++a) {
		for (char b = 'a'; b <= 'z'; b += 5) { words.push_back(std::string(1, a) + b + (a % 3 ? "ing" : "ed")); }
	}
	std::sort(words.begin(), words.end());
	words.erase(std::unique(words.begin(), words.end()), words.end());
	build("adegbilnrtuyzfhjkmopqsvwxc", words);
	build("abcdefghijklmnopqrstuvwxyz", words);
	std::vector<std::string> deep;
	for (int i = 1; i <= 80; ++i) { deep.push_back(std::string(std::size_t(i), 'x')); }
	build("x", deep);
	build("", deep);
	std::vector<std::string> wide;
	for (int i = 0; i < 300; ++i) { wide.push_back(std::string(1, char(i / 2)) + char(i % 2 ? 'b' : 'a')); }
	std::sort(wide.begin(), wide.end());
	wide.erase(std::unique(wide.begin(), wide.end()), wide.end());
	build("", wide);
	return 0;
}
