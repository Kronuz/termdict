# termdict

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/termdict/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/termdict/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)

Block-tree sorted string table for C++20: prefix-compressed blocks indexed by an FST,
with in-place, bounds-checked lookups. It maps sorted byte-string keys to opaque value
bytes, and a small FST over the blocks' first keys finds the block a key lives in. It
is the term dictionary of a search engine: keys are terms, values are term statistics
and postings pointers.

![termdict: an FST over block first keys points into prefix-compressed blocks](docs/termdict.svg)

## ✨ Highlights

- **Two flat byte strings.** `builder::finish()` returns the blocks and the index, and a
  `reader` opens them in place as `std::string_view`s.
- **Compact.** Blocks of 32 to 64 prefix-compressed entries, a restart every 32, and key
  suffixes coded in `bit_width(alphabet size - 1)` bits a byte (5 for lowercase terms):
  12.35 bytes per key on the English Wikipedia vocabulary, 6-byte values included.
- **Lookups without rebuilding keys.** One `fst::floor_value` over the block index, a
  binary search over at most two restart points, and at most 32 entry decodes that
  compare the target in place.
- **Ordered scans and seeks.** `scan(from, f)` and `reader::cursor` visit entries in key
  order from any key, one block decoded at a time, for k-way merges of dictionaries.
- **Bounds-checked.** Every length and offset read from a block is checked, the
  constructor validates the whole FST index, and malformed bytes throw
  `termdict::corrupt`.
- **Checksums on first use.** An optional `verify(context, offset, length)` hook sees
  each block's bytes before the reader decodes them.

## 🚀 Quick start

```cpp
#include "termdict.hh"

#include <cstdio>
#include <string_view>

int main() {
	termdict::builder b("aelpy");          // every byte the keys use
	b.add("apple", "\x01\x02");            // keys in strictly increasing order
	b.add("apply", "\x03");
	auto tables = b.finish();              // tables.blocks, tables.index

	termdict::reader r(tables.blocks, tables.index);
	if (auto v = r.find("apply")) {
		std::printf("apply: %zu value byte\n", v->size());   // apply: 1 value byte
	}
	r.scan("app", [](std::string_view key, std::string_view) {
		std::printf("scan: %.*s\n", int(key.size()), key.data());   // scan: apple, scan: apply
		return true;
	});

	termdict::reader::cursor c(r);
	if (c.seek("applf")) {
		std::printf("seek: %.*s\n", int(c.key().size()), c.key().data());   // seek: apply
	}
}
```

## 📦 Installation

Requires a C++20 compiler and CMake 3.14 or newer. It brings
[fst](https://github.com/Kronuz/fst) and [varint](https://github.com/Kronuz/varint)
through FetchContent. CI builds and tests it with GCC and Clang on Linux (x86_64,
arm64), macOS (arm64, x86_64), and FreeBSD, plus ASan, UBSan, and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(termdict
  GIT_REPOSITORY https://github.com/Kronuz/termdict.git
  GIT_TAG        dc42a4f8f45bfea1570d8a7d3f88d2b1a3156f55)  # or main, which tracks the tip
FetchContent_MakeAvailable(termdict)

target_link_libraries(my_app PRIVATE termdict::termdict)
```

Then `#include "termdict.hh"`.

## 🎯 When to use it

A sorted, immutable key-to-bytes table, built once and read many times, that needs
ordered scans or seeks from any key: term dictionaries, sorted string tables in
immutable segments.

Not for:

- **Mutable maps.** Build once, then read; keys go in strictly increasing order.
- **Point lookups alone.** A hash map answers a present key in 126 ns against 645 ns
  here (see [Performance](#-performance)); termdict pays for order and size.
- **Trusting unverified blocks.** Without the `verify` hook, a lookup, seek, or scan
  checks only the entries it decodes, so damage elsewhere in a block goes unnoticed
  until something reads it.

## ⚡ Performance

English Wikipedia vocabulary (3,964,752 terms), 6-byte values, against Lucene 10.4's
`TermsEnum.seekExact` on the same terms. Absent keys are present keys with a byte
appended or the last byte changed.

| Lookup | termdict | Lucene `seekExact` | Full-vocabulary `fst::get` | Hash map |
| --- | ---: | ---: | ---: | ---: |
| Present | 645 ns | 1,190 ns | 368 ns | 126 ns |
| Absent | 686 ns | 1,250 ns | 389 ns | 94 ns |

The tables take 12.35 bytes per key including the 6-byte values (blocks 48.0 MB, index
0.9 MB). Linux VM on an Intel Core i9-9980HK, Clang 18, `-O3`; the termdict, FST, and
hash-map columns come from:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target termdict_bench
./build/termdict_bench KEYS_FILE   # one key per line
```

[docs/perf/](docs/perf/) has the seek and decode measurements.

## 📚 Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md): building, reading, seeking, and validation.
- [docs/perf/](docs/perf/): the seek and decode measurements.
- [AGENTS.md](AGENTS.md): repository map, the byte layout, and invariants.
- [Catalog entry](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#termdict).

## 🛠️ Building and testing

```sh
cmake -B build
cmake --build build
ctest --test-dir build
```

The tests compare `find`, `scan`, and cursor seeks with `std::map`, inject truncations
and random corruptions into the blocks and the index, and check a golden fixture.
`-DTERMDICT_FUZZ=ON` with Clang builds the libFuzzer targets; `termdict_bench`,
`termdict_scan_bench`, and `termdict_open_bench` build with the tests.

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) first, and the
[catalog's AGENTS.md](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md) for
the family's rules. CI must pass on every platform.

## 📄 License

MIT; see [LICENSE](LICENSE).
