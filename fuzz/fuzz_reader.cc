// libFuzzer target: tables built from input-derived keys (over their own alphabet or all
// 256 bytes) answer every lookup and seek exactly, and arbitrary bytes used as block data
// either answer or throw termdict::corrupt.
#include "termdict.hh"

#include <cstdlib>
#include <map>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	std::string_view bytes(reinterpret_cast<const char*>(data), size);
	std::map<std::string, std::string> keys;
	std::size_t start = 0;
	for (std::size_t i = 0; i <= size; ++i) {
		if (i == size || data[i] == '\n') {
			auto line = bytes.substr(start, i - start);
			keys.emplace(std::string(line.substr(0, line.size() / 2 + 1)), std::string(line.substr(line.size() / 2)));
			start = i + 1;
		}
	}
	// The keys' own alphabet (coded suffixes) for odd-sized inputs, all 256 bytes otherwise.
	std::string alphabet;
	if (size % 2) {
		for (const auto& [k, v] : keys) { alphabet += k; }
		if (alphabet.empty()) { alphabet = "a"; }
	}
	termdict::builder b(alphabet);
	for (const auto& [k, v] : keys) { b.add(k, v); }
	auto t = b.finish();
	termdict::reader r(t.blocks, t.index);
	for (const auto& [k, v] : keys) {
		auto got = r.find(k);
		if (!got || *got != v) { std::abort(); }
	}
	// Seeks to each key and to probes beside it land on lower_bound, and next() follows.
	termdict::reader::cursor c(r);
	auto seek = [&](const std::string& probe) {
		auto w = keys.lower_bound(probe);
		if (c.seek(probe) != (w != keys.end())) { std::abort(); }
		if (w == keys.end()) {
			if (c.next()) { std::abort(); }
			return;
		}
		if (c.key() != w->first || c.value() != w->second) { std::abort(); }
		++w;
		if (c.next() != (w != keys.end()) || (w != keys.end() && c.key() != w->first)) { std::abort(); }
	};
	for (const auto& [k, v] : keys) {
		seek(k);
		seek(k + '\0');
		seek(k.substr(0, k.size() / 2));
		if (!k.empty()) { seek(k.substr(0, k.size() - 1) + char(std::uint8_t(k.back()) + 1)); }
	}
	seek(std::string(bytes.substr(0, 8)));
	// The same index over the fuzz bytes as block data.
	termdict::reader junk(bytes, t.index);
	try {
		for (const auto& [k, v] : keys) { (void)junk.find(k); }
		std::size_t n = 0;
		junk.scan({}, [&](std::string_view, std::string_view) { return ++n < 1000; });
		termdict::reader::cursor j(junk);
		for (const auto& [k, v] : keys) {
			if (j.seek(k)) { (void)j.next(); }
		}
	} catch (const termdict::corrupt&) {
	}
	return 0;
}
