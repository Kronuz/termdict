# termdict architecture

`termdict.hh` is the whole implementation. A `builder` turns sorted key-value pairs into
two byte strings, the blocks and the index; a `reader` opens them in place and answers
lookups, scans, and seeks. [AGENTS.md](AGENTS.md) has the exact byte layout of both.

## Building

- `termdict::builder(alphabet)`: `add(key, value)` in strictly increasing key order;
  `finish()` returns two byte strings, the blocks and the index. `alphabet` names every
  byte the keys use (empty: all 256).
- Key suffixes are coded over the alphabet: each byte becomes its rank,
  `bit_width(size - 1)` bits wide, so codes sort as the bytes do. Lowercase terms take
  5 bits a byte. Each restart run stores its suffixes as one bit stream.
- Blocks hold 32–64 entries (cut at 64, or at 32 or more once 1 KiB is reached). Entries
  are prefix-compressed; every 32nd entry is a restart point stored whole.

## Reading

- `termdict::reader(blocks, index)`: `find(key)` returns the value bytes or nothing;
  `scan(from, f)` visits entries with keys `>= from` in order; `reader::cursor` pulls
  every entry in order, one block decoded at a time, for k-way merges of dictionaries,
  and `cursor::seek(from)` moves it to the first key `>= from` from any position.
- A lookup is one `fst::floor_value` over the block index, a binary search over at most
  two restart points, and at most 32 entry decodes that compare the target in place
  without rebuilding keys.
- A seek starts at its target: one `fst::floor_value`, a binary search over the block's
  restart points, and one restart run walked without building keys; only the landing
  key is rebuilt, as the target's shared prefix plus its own suffix.

## Validation

- Every length and offset read from a block is bounds-checked; a violation throws
  `termdict::corrupt`. The reader's constructor validates the alphabet and the whole FST
  index, one linear pass over its nodes (no hook needed), and throws `corrupt` on malformed
  bytes. The FST's values are block offsets; each is checked against the blocks when a
  lookup or seek opens that block.
- `reader(blocks, index, verify, context)` calls `verify(context, offset, length)`
  on each block's bytes before reading them (its length prefix, then the whole
  block), for checksum verification on first use; a false return throws `corrupt`.
  Without it, a lookup, seek or scan validates only the entries it decodes, so damage
  elsewhere in a block goes unnoticed until something reads it.

## Tests

The tests compare `find`, `scan`, and cursor seeks with `std::map` over random key sets
(including long values that force small blocks) and the system dictionary, seek to every
key and beside it across block boundaries, reject unsorted input and out-of-alphabet codes
at every width, query every truncation and 20,000 random corruptions of the blocks, reject each malformed-FST
case and every truncation and single-byte change of the index, and compare a golden fixture.
`fuzz/fuzz_reader.cc` and `fuzz/fuzz_open.cc` (arbitrary blocks and index bytes) are libFuzzer
targets. `build/termdict_scan_bench` times
sequential decode, seeks, and lookups on strata's KV keys and a vocabulary
([docs/perf/seek-decode.md](docs/perf/seek-decode.md)).
