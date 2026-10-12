# AGENTS.md — termdict

## Repo map

```
termdict.hh            builder, block (bounds-checked decoding), reader (find, scan).
test/test.cc           std::map comparisons, corruption, golden fixture.
fuzz/fuzz_reader.cc    libFuzzer: built tables answer exactly; junk block bytes throw corrupt or answer.
bench/bench.cc         Lookups against a hash map and a full FST on a vocabulary file.
```

## Invariants

- Block format is fixed: [length][count][restarts][restart offsets][entries], LEB128 throughout.
- Every 16th entry restarts (shared length 0); a block holds at most 48 entries.
- Never read a length or offset from a block without checking it against the block's end.

## Verify a change

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```
