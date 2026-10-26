# validate-at-open ledger

Branch `fix/validate-at-open` (worktree `~/code/Kronuz/termdict-wt/validate-at-open`), from main 840d7b3.

## State

In review (K1-K4 met; K5 is the lead's CI watch). Security review wanted: decoder safety.

## What was wrong (and what was not)

The premise "open still trusts the FST" does not hold at the pinned fst (24da59a). `fst::map(bytes)`
validates every node in one linear pass (magic and size, root in range and the last node, header
bits, label order, widths, each target an earlier node start, varints bounded, key count = the
trailer's), and `termdict::reader` used that constructor. Two gaps:

1. A malformed FST threw `std::invalid_argument`, not `termdict::corrupt`.
2. No test or fuzz target mutated the index bytes (fuzz_reader junks block bytes only).

Change: `reader::open_index` parses the alphabet, validates the FST, and translates
`invalid_argument` to `corrupt`. That is the whole library change (termdict.hh, +11 lines).

Decode paths after open, and why lazy checks are safe:
- Index: after validation every node, transition target and output width is in bounds, the graph is
  acyclic, and `floor_value` reads only validated nodes (the 16-byte trailer covers its 8-byte loads).
- Block offsets (the FST's values) are unvalidated numbers until used: `block`'s constructor throws
  `corrupt` for an offset at or past the blocks' end, and bounds-checks every length inside the block.
  Blocks stay lazily validated; a verifier hook covers them, not the index.
- The index is not covered by the verifier hook, so a reader with a hook still needs this pass.

## Done

- K1 met. `open_rejects` (test/test.cc): hand-built FSTs, one per rejected case (short, magic, root
  below/at/past the nodes, delta zero/below/mid-node (two layouts), final output on a non-final node
  (two layouts), reserved chain bits, label order/repeat, >256 transitions, every output/target width
  pair 0-15, declared sizes past the end, truncated nodes, unterminated varints, key count, root not
  last, trailing node), plus a lookup past the blocks. `sweep_index`: every truncation, every bit flip
  and 24 random bytes at every position of four dictionaries' indexes, in exact-size heap buffers.
  Reverted fix (`td-no-translate`): fails at `opens`, "open threw something but termdict::corrupt".
  ASan+UBSan (Homebrew LLVM 23.1.2, `-fsanitize=address,undefined`): `termdict: ok`, 65 s. Plain: ok, 22 s.
- K2 met. `fuzz/fuzz_open.cc` (input = [u32 blocks size][blocks][index]; open, then scan, cursor walk,
  find, seek, forward seeks, scan from probes; only `corrupt` may escape). Seeds from
  `fuzz/make_seeds.cc`. Run: `quiet light ... termdict_fuzz_open .scratch/corpus-open fuzz/corpus/open
  -max_total_time=2100 -rss_limit_mb=2048 -timeout=20` (RelWithDebInfo, `-fsanitize=fuzzer,address,undefined
  -fno-sanitize-recover=all`, Homebrew LLVM clang++): 13,615,556 runs in 2,101 s, no report, peak 610 MB.
  Corpus merged (`-merge=1`) into `fuzz/corpus/open`: 675 files, 390 KB. The fuzz target catches the
  reverted fix (uncaught `invalid_argument` within seconds, 120 s run, crash written).
- K3 met. Mutants (`.scratch/mut/run.py`: a copy of fst.hh or termdict.hh per mutant, test.cc built
  -O1, ASan+UBSan where noted). Killed:
  - td-trusted (`fst::trusted`): open_rejects, root-below-magic case
  - td-no-translate: open_rejects `opens` (above)
  - fst magic: "FST2" case; fst short (ASan): truncation sweep, OOB read of an empty FST
  - final output without final bit: the otherwise well-formed chain with header 0x06
  - chain reserved bits: header 0x14 case
  - label order, label repeat: "ba", "aa" cases
  - output width, target width (>8): the width-pair loop
  - fields room (ASan): 200 labels, widths 0x88, no fields
  - varint length (UBSan, shift past 63): 11-byte varint case
  - target is a node start: the mid-node case with a later node start (CHECK); without it the
    sweeps also loop forever, so this check is what keeps a lookup finite
  - root is the last node: root-at-4 case (CHECK); the sweeps hang without it too
  - key count: count 2 case
  Survived, each subsumed by another check (equivalent): root below magic (root must equal the last
  node's start, which is >= 4), root at/past end (same), delta 0 and delta past the first node (the
  target must be an earlier node start), chain label `at >= end` (the next varint checks it), target
  width 0 (delta 0 is rejected), count > 256 (labels strictly ascend, 256 max), count room (the
  ascending-label loop stops within the 16 trailer bytes and `at != end` follows), varint `at >= end`
  (reads stay in the trailer, then `at != end`), `at != end` (the loop ends with at == end). Depth
  over 2^32 - 1 needs a 4 GiB-deep FST; not tested.
- K4 met. English Wikipedia vocabulary, 3,964,752 terms (strata/.scratch/vocab.txt), blocks 39.1 MB,
  index 708,531 bytes. `quiet strict`, Apple clang -O3, 3 alternating pairs of 31 opens: before (main)
  5.79, 5.80, 5.84 ms median; after 6.18, 6.12, 5.84 ms (noise; no change is claimed: the validation was
  already in main and the new code is a `try` that costs nothing when it does not throw). Each open:
  54 allocations, 5.24 MB total, 3.15 MB peak (the validator's three per-node vectors, ~7.4 bytes per
  index byte), both before and after. The FST mapped without validation: 0.00004 ms. So open costs
  ~5.8 ms and 3 MB transient for a 40 MB dictionary, and validation is all of it. Cheaper options
  (reserve the vectors, drop the key-count vector) belong to the fst repo; not done here.
  Bench: `bench/open.cc` (`termdict_open_bench build VOCAB TABLES`, then `time TABLES 31`).
- K5: branch pushed; CI watched by the lead.

Commits: 175693a (translation, tests, fuzz target), 11e15a0 (bench, docs, test hardening), deceb4d
(width, final-output and declared-size tests; corpus), ea9d9d5 (mid-node target test; this ledger).

## Next

Security review, then land. Optional, in fst: cheaper validation (see K4).

## Blocked on

Nothing.
