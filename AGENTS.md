# AGENTS.md — termdict

## Repo map

```
termdict.hh            builder, block (bounds-checked decoding), reader (find, scan).
test/test.cc           std::map comparisons, corruption, golden fixture.
fuzz/fuzz_reader.cc    libFuzzer: built tables answer exactly; junk block bytes throw corrupt or answer.
bench/bench.cc         Lookups against a hash map and a full FST on a vocabulary file.
docs/termdict.d2     README diagram; render: d2 --layout elk --sketch --theme 302 docs/termdict.d2 docs/termdict.svg
```

## Invariants

- Index format: [alphabet size, 0 for all 256][alphabet bytes, increasing][FST].
- Block format: [length][count][restarts][restart offsets], then per restart run
  [suffix bits length][suffix bits][entries], LEB128 throughout; entries hold lengths and
  values, suffixes live in the run's bit stream (codes are alphabet ranks).
- Every 32nd entry restarts (shared length 0); a block holds at most 64 entries.
- Never read a length or offset from a block without checking it against the block's end.

## Verify a change

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```
