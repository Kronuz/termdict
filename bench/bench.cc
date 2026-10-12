// termdict lookups on a vocabulary (one key per line): present and absent keys, against
// an open-addressing hash map and a full-vocabulary FST; size per key for each.
//   termdict_bench KEYS_FILE
#include "termdict.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
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
}
