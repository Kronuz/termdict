// libFuzzer target: tables built from input-derived keys answer every lookup exactly,
// and arbitrary bytes used as block data either answer or throw termdict::corrupt.
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
	termdict::builder b;
	for (const auto& [k, v] : keys) { b.add(k, v); }
	auto t = b.finish();
	termdict::reader r(t.blocks, t.index);
	for (const auto& [k, v] : keys) {
		auto got = r.find(k);
		if (!got || *got != v) { std::abort(); }
	}
	// The same index over the fuzz bytes as block data.
	termdict::reader junk(bytes, t.index);
	try {
		for (const auto& [k, v] : keys) { (void)junk.find(k); }
		std::size_t n = 0;
		junk.scan({}, [&](std::string_view, std::string_view) { return ++n < 1000; });
	} catch (const termdict::corrupt&) {
	}
	return 0;
}
