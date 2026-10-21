# Forward seek inside the open block

Status: finished on `perf/forward-seek` from main a91e9b6, from explorer B's `explore/forward-seek-b`
(1deaa82). Seek is a decoder path in a shared library, so it gets one review before landing.
Bytes on disk and `seek`'s contract are unchanged.

## Change

`reader::cursor::seek(from)` first tries `forward(from)` when the cursor is on an entry (`on_entry_`,
set by a successful `next()`/`seek()`, cleared by a failed or throwing one) and `from` is at or above
the current key. `from` equal to the current key returns at once. Otherwise `from` is compared with
the open block's last key (`block::last_key`, cached per open block): above it, `forward` returns
false at the cost of one comparison and the old path runs; at or below it, `from` is certainly in the
block, so the later restart runs are binary-searched and the landing run is stepped or walked
(`walk_to`, `land`), skipping `fst::floor_value`. The explorer's common-prefix gate is gone.

`block::last_key` scans the last run's entry headers, then decodes from the last entry back only the
suffix symbols the key keeps, instead of decoding every entry of the run.

A block's first forward attempt only marks it (`tried_`) and falls back; the second decodes the last
key. The marks survive re-opening the same block offset. Without this, the decode lands on every
random seek that opens a block it never returns to (see Experiments 2). So the fast path is taken
exactly when the target is in the open block, except for the first attempt after a block opens.

## Experiments

Same bench and method throughout: `bench/scan.cc` (kv: 1,000,000 keys; terms: 235,976 words), 7
trials, order alternating (a b a2, then a2 b a), each figure the median of 5 repetitions, A/A =
the main binary run twice (a, a2), strict `quiet run`, Apple clang 17 `-O3 -DNDEBUG`, i9-9980HK.
Median ns per seek; min-max over trials in brackets for the branch.

1. Last key decoded through `block::cursor` (all entries of the run) on the first attempt: fwd 21-50%
   slower, random seek+10 43-59% slower (kv fwd 457 to 552; kv seek10 909 to 1304). Rejected.
2. Last key from the headers (`block::last_key`), decoded on the first attempt: near 48-55% less,
   fwd 8-13% less, but random seek+10 15-18% slower (kv 872 to 1004, terms 702 to 826; A/A within 5%).
   Half of random seeks try a forward seek from the block the previous random seek opened, and pay
   the decode for nothing. Rejected as is.
3. Same, decoded on the second attempt (kept).

## Result (experiment 3)

| Row | A/A (main, main) | main | forward-seek | change |
|---|---|---|---|---|
| kv near (1..16) | 367 / 375 | 367 | 190 [178-231] | 48% less |
| terms near | 348 / 336 | 348 | 193 [183-203] | 45% less |
| kv fwd (1..256) | 434 / 428 | 434 | 427 [380-526] | inside the spread |
| terms fwd | 391 / 401 | 391 | 388 [367-430] | inside the spread |
| kv random seek+10 | 883 / 891 | 883 | 879 [850-933] | inside the spread |
| terms random seek+10 | 726 / 751 | 726 | 772 [684-806] | 6% slower, inside the spread (A/A differ 3%) |

keyscan and find rows move by at most 6% either way with no pattern. The explorer's heuristic
measured fwd 21-27% less; the exact test gives that up (only 7% of 1..300-step seeks stay in a block
of at most 64 entries, so most fwd attempts miss, and a miss still costs the first-attempt mark and
the last-key decode). Near seeks keep the gain.

## Proof

- `check_forward` (test/test.cc): a counting verifier sees every block the cursor opens. From every
  position of every block, over keys and keys + `\0` of the rest of the block, the block's last key,
  one past it, the next block's first key, a hop past it, backwards targets (below the current key, the
  block's first key, ""), and equal to the current key, a seek opens no block exactly when the target
  lies between the current key and the block's last key, and always lands on `std::lower_bound` with
  `next()` following. Restart keys are among the targets. Also: `block::last_key` equals each block's
  true last key; a failed seek leaves no stale base; the first attempt falls back and the second hits;
  20,000 random hops on one cursor against `std::lower_bound`.
- Mutants, each caught by `check_forward`: forward disabled; last-key gate `>` to `>=`; the
  `on_entry_` guard replaced by `block_ && at_.index > 0`; marks not reset on a new block.
- `termdict_test` passes under Homebrew LLVM ASan+UBSan (`-fno-sanitize-recover=all`); the corruption
  sweep drives one cursor with forward seeks over truncated and flipped blocks. The libFuzzer target
  gained ascending seeks without `next()`, on good tables and on junk blocks: 222,783 runs in 61 s,
  clean.

## Next

Prefetching the next block; a block's last key stored in the block (a format change, so an architect
question) would remove the first-attempt mark and make the test exact for every attempt.
