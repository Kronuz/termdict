# Forward seek inside the open block

Status: explored on `explore/forward-seek-b`; a win on forward seeks, random seeks unchanged within noise. Not merged.

## Change

`reader::cursor::seek(from)` first tries `forward(from)` when a block is open and a key is
current: `from >= key` and enough of `from` matches the current key (common prefix at least a
third of the key's length, a cheap gate against far targets). It then binary-searches the open
block's later restart runs, finishes the current run by stepping, or walks the landing run
(`walk_to` and `land`, as before), skipping `fst::floor_value`. When `from` may lie past the
block's last key (the landing run is the block's last and has no entry >= from) it returns false
and the old path runs. Bytes on disk are unchanged.

## Experiments

1. No gate: forward rows 10-15% faster, random seek-and-10 about 9-25% slower (the failed
   attempts touch a cold block). Run 1, 7 trials, noisy host (A/A spread about 20%). Loser as is.
2. Common-prefix gate (kept, below).

## Result (experiment 2)

`bench/scan.cc` gained `fwd_ns` (each target 1..256 entries past the previous, half present, half
just above a key) and `near_ns` (1..16). 7 interleaved trials, order alternating, each figure the
median of 7 repetitions, `quiet run` light, Mac i9-9980HK, Release `-O3`, clang. Main is
`380f6b3`'s header built into the same bench. Median ns per seek, min-max over trials in brackets.

| Row | A/A (main, main) | main | forward-seek | change |
|---|---|---|---|---|
| kv near (1..16) | 428 / 421 | 679 [641-811] | 331 [323-361] | 51% less |
| terms near | 396 / 396 | 600 [574-740] | 315 [299-328] | 48% less |
| kv fwd (1..256) | 511 / 520 | 760 [697-887] | 553 [525-594] | 27% less |
| terms fwd | 478 / 486 | 693 [638-768] | 548 [503-568] | 21% less |
| kv random seek10 | 982 / 977 | 1396 [1260-1545] | 1466 [1241-1514] | 5% slower, inside spread |
| terms random seek10 | 856 / 860 | 1284 [1217-1478] | 1294 [1220-1329] | 1% slower, inside spread |

A/A medians agree within 2%; the host load differs between the A/A and A/B phases (other
agents share it), so compare only within a row's pair. The 1..256 row gains less because blocks
hold at most 64 entries, so hops over 64 leave the block and fall back.

## Identity

`bench/scan.cc` checks every forward target (100,000 each for 1..256 and 1..16) against
`std::lower_bound` before timing, and the tests gained forward-seek sweeps (steps 1 to 700, present
and just-above targets, one cursor) beside the existing random-order seek checks; `termdict_test`
passes.

## Next

Prefetching the next block, and a gate-free exact test, would need the block's last key (not stored).
