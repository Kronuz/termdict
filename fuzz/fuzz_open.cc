// libFuzzer target: arbitrary dictionary bytes, [u32 LE blocks size][blocks][index]. Opening
// either throws termdict::corrupt or yields a reader whose find, scan, cursor walk and seeks
// stay in bounds and throw nothing but termdict::corrupt, with no verifier hook.
#include "termdict.hh"

#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	if (size < 4) { return 0; }
	std::uint32_t head;
	std::memcpy(&head, data, 4);
	const std::size_t blocks_size = head % (size - 3);  // 0 .. size - 4
	// Exact-size copies, so ASan sees any read past either table.
	std::vector<char> blocks(data + 4, data + 4 + blocks_size), index(data + 4 + blocks_size, data + size);
	const std::string_view b(blocks.data(), blocks.size()), i(index.data(), index.size());
	try {
		termdict::reader r(b, i);
		std::vector<std::string> probes{"", std::string(1, '\xff'), std::string(b.substr(0, 6)), std::string(i.substr(0, 6))};
		std::size_t n = 0;
		r.scan({}, [&](std::string_view key, std::string_view) {
			if (probes.size() < 40) { probes.emplace_back(key); }
			return ++n < 1000;
		});
		termdict::reader::cursor c(r);
		for (std::size_t s = 0; s < 1000 && c.next(); ++s) { (void)c.key(); (void)c.value(); }
		termdict::reader::cursor fwd(r);  // seeks on from where it stands: the in-block forward path
		for (const auto& p : probes) {
			for (const auto& t : {p, p + '\0', p + "\xff"}) {
				(void)r.find(t);
				termdict::reader::cursor one(r);
				if (one.seek(t)) { for (int s = 0; s < 40 && one.next(); ++s) { (void)one.key(); } }
				int m = 0;
				r.scan(t, [&](std::string_view, std::string_view) { return ++m < 40; });
				(void)fwd.seek(t);
			}
		}
	} catch (const termdict::corrupt&) {
	}
	return 0;
}
