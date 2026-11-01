# Lookup against an FST, a hash map and the standard containers

`bench/bench.cc` (`termdict_bench`) times `reader::find` and four other structures on one
vocabulary: the 235,976 words of `/usr/share/dict/words`, each with a 6-byte value (the key's
index). It is built from the sorted words; 235,280 absent keys come from the present ones
(a `q` appended, or the last byte's low bit flipped, when that is not a key).

## Method

- Contenders: termdict; `fst::get` on one FST over the whole vocabulary ([fst](https://github.com/Kronuz/fst/tree/39245fa)
  at commit 39245fa, the one CMake pins); an open-addressing hash map written in the bench
  (FNV-1a, linear probing, load factor at most one half, the key bytes in one arena);
  `std::unordered_map<std::string_view, std::size_t>`; `std::binary_search` over the sorted
  `std::vector<std::string>`. The standard containers are libc++ of Apple clang 17.
- Agreement: before any timing every contender is asked for every present key and every absent
  key. The bench exits on a difference: the membership must be the same everywhere, and termdict
  must return the 6 value bytes and the FST the index.
- Timing: the keys are shuffled and queried in batches of 1,024; a round's figure is the median
  batch time, per lookup, over up to 1,500 batches. A run prints two rounds; three runs give six,
  and the figure in the README is the median of the six.
- p50 and p99: the per-lookup time over batches of 16, each batch timed alone less the timer's
  own median cost (a coarse clock cannot time one lookup); the median of the three runs.
- Machines: Apple M4 Pro (macOS 26) and Intel Core i9-9980HK (macOS 15), Apple clang 17,
  CMake Release (`-O3 -DNDEBUG`), each held exclusively while the bench ran, at `termdict:f3be2d6`.

## Results

Nanoseconds per lookup, median of six rounds (the range is the fastest and the slowest round):

| Machine | Lookup | termdict | `fst::get` | Hash map | `std::unordered_map` | `std::binary_search` |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Apple M4 Pro | present | 238.8 (230 to 250) | 145.8 (144 to 158) | 30.4 (28 to 37) | 46.0 (37 to 50) | 190.4 (187 to 200) |
| Apple M4 Pro | absent | 247.6 (245 to 267) | 153.6 (151 to 162) | 34.4 (29 to 36) | 32.6 (29 to 36) | 193.0 (185 to 202) |
| Intel Core i9-9980HK | present | 421.5 (361 to 441) | 126.1 (124 to 146) | 56.5 (49 to 67) | 193.7 (179 to 244) | 320.9 (316 to 338) |
| Intel Core i9-9980HK | absent | 435.7 (378 to 465) | 131.6 (124 to 149) | 42.2 (38 to 55) | 115.8 (97 to 160) | 323.5 (316 to 342) |

p50 and p99 over batches of 16, in nanoseconds:

| Machine | Lookup | termdict | `fst::get` | Hash map | `std::unordered_map` | `std::binary_search` |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Apple M4 Pro | present | 240 / 378 | 148 / 253 | 34 / 55 | 55 / 99 | 195 / 281 |
| Apple M4 Pro | absent | 255 / 367 | 154 / 245 | 39 / 60 | 42 / 73 | 190 / 307 |
| Intel Core i9-9980HK | present | 387 / 1,444 | 124 / 204 | 52 / 102 | 197 / 294 | 326 / 480 |
| Intel Core i9-9980HK | absent | 432 / 1,418 | 130 / 242 | 41 / 75 | 109 / 204 | 311 / 497 |

The slowest round over the fastest is 1.1x on the M4 Pro for termdict, the FST and the sorted
vector, and 1.3x to 1.4x for the two hash tables. On the i9 it is 1.2x for termdict and the FST,
1.1x for the sorted vector and 1.4x to 1.7x for the hash tables, so a gap under that is a tie there.

## Size

| Structure | Bytes per key |
| --- | ---: |
| termdict (blocks 2,553,391 + index 52,386, values included) | 11.04 |
| FST (64-bit values) | 6.20 |
| Hash map in the bench | 62.89 |

The standard containers were not sized.

## Reading it

termdict is the slowest column on both machines: a lookup is one `floor_value` over the FST
of block first keys, a binary search over at most two restart points and at most 32 in-place
entry decodes. What it buys is a table 5.7 times smaller than the hash map that scans and seeks in key order
and keeps one block decoded at a time (see [forward-seek.md](forward-seek.md) and
[seek-decode.md](seek-decode.md)). For point lookups alone, a hash map or the FST is faster.
