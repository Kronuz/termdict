# AGENTS.md — termdict

## Repo map

```
termdict.hh            builder, block (bounds-checked decoding), reader (find, scan, cursor and seek).
test/test.cc           std::map comparisons, corruption, golden fixture.
fuzz/fuzz_reader.cc    libFuzzer: built tables answer exactly; junk block bytes throw corrupt or answer.
fuzz/fuzz_open.cc      libFuzzer: arbitrary blocks and index bytes; open throws corrupt or the walk stays in bounds.
fuzz/make_seeds.cc     writes fuzz_open's seed corpus (fuzz/corpus/open).
bench/open.cc          Open cost (time, allocations) on a saved vocabulary's tables.
bench/bench.cc         Lookups against a hash map and a full FST on a vocabulary file.
bench/scan.cc          Sequential decode, seeks, and lookups on strata's KV keys and a vocabulary.
docs/perf/             Measured experiments, kept and removed.
docs/termdict.d2     README diagram; render: d2 --layout elk --sketch --theme 302 docs/termdict.d2 docs/termdict.svg
```

## Invariants

- Index format: [alphabet size, 0 for all 256][alphabet bytes, increasing][FST].
- Block format: [length][count][restarts][restart offsets], then per restart run
  [suffix bits length][suffix bits][entries], LEB128 throughout; entries hold lengths and
  values, suffixes live in the run's bit stream (codes are alphabet ranks).
- Every 32nd entry restarts (shared length 0); a block holds at most 64 entries.
- Never read a length or offset from a block without checking it against the block's end.
- The reader's constructor validates the FST (fst::map without `trusted`) and throws `corrupt`;
  never open the index with `fst::trusted`, since a verifier hook covers blocks only.
- A suffix code >= the alphabet's size is corrupt; the chunked decode checks it with one
  SWAR carry test per 64-bit load. `reader::cursor` decodes into a buffer padded by
  `suffixes::padding`, which whole-chunk writes may fill past the key.

## Verify a change

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```
