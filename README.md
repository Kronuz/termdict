# termdict

[![CI](https://github.com/Kronuz/termdict/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/termdict/actions/workflows/ci.yml)

A block-tree sorted string table for C++20: sorted byte-string keys mapped to opaque
value bytes, stored as prefix-compressed blocks with a small FST over each block's
first key. It is the term dictionary of a search engine: keys are terms, values are
term statistics and postings pointers. Header-only; depends on
[fst](https://github.com/Kronuz/fst) and [varint](https://github.com/Kronuz/varint).

## What it is

- `termdict::builder(alphabet)`: `add(key, value)` in strictly increasing key order;
  `finish()` returns two byte strings, the blocks and the index. `alphabet` names every
  byte the keys use (empty: all 256).
- Key suffixes are coded over the alphabet: each byte becomes its rank,
  `bit_width(size - 1)` bits wide, so codes sort as the bytes do. Lowercase terms take
  5 bits a byte. Each restart run stores its suffixes as one bit stream.
- `termdict::reader(blocks, index)`: `find(key)` returns the value bytes or nothing;
  `scan(from, f)` visits entries with keys `>= from` in order.
- Blocks hold 32–64 entries (cut at 64, or at 32 or more once 1 KiB is reached). Entries
  are prefix-compressed; every 32nd entry is a restart point stored whole.
- A lookup is one `fst::floor_value` over the block index, a binary search over at most
  two restart points, and at most 32 entry decodes that compare the target in place
  without rebuilding keys.
- Every length and offset read from a block is bounds-checked; a violation throws
  `termdict::corrupt`. The FST index is validated when the reader is constructed.
- `reader(blocks, index, verify, context)` calls `verify(context, offset, length)`
  on each block's bytes before reading them (its length prefix, then the whole
  block), for checksum verification on first use; a false return throws `corrupt`.

## Usage

```cpp
#include "termdict.hh"

termdict::builder b("aelpy");           // the keys' bytes
b.add("apple", "\x01\x02");
b.add("apply", "\x03");
auto tables = b.finish();               // tables.blocks, tables.index

termdict::reader r(tables.blocks, tables.index);
auto v = r.find("apply");               // "\x03"
r.scan("app", [](std::string_view key, std::string_view value) { return true; });
```

## Performance

English Wikipedia vocabulary (3,964,752 terms), 6-byte values, Linux VM on an Intel
i9-9980HK, Clang 18 `-O3`, against Lucene 10.4's `TermsEnum.seekExact` on the same terms
in the same VM (`strata/bench/phase2/lucene_seek`). Absent keys are present keys with a
byte appended or the last byte changed.

| Lookup | termdict | Lucene `seekExact` | Full-vocabulary `fst::get` | Hash map |
|---|---:|---:|---:|---:|
| Present | 645 ns | 1,190 ns | 368 ns | 126 ns |
| Absent | 686 ns | 1,250 ns | 389 ns | 94 ns |

The tables take 12.35 bytes per key including the 6-byte values (blocks 48.0 MB, index
0.9 MB).

## Build and test

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
build/termdict_bench KEYS_FILE
```

The tests compare `find` and `scan` with `std::map` over random key sets (including
long values that force small blocks) and the system dictionary, reject unsorted input,
query every truncation and 20,000 random corruptions, and compare a golden fixture.
`fuzz/fuzz_reader.cc` is a libFuzzer target.

## License

MIT. See [LICENSE](LICENSE).
