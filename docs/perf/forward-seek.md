# Forward seek inside the open block

Status: explored on branch `explore/forward-seek-a`; not for landing as it stands.

## Hypothesis

`reader::cursor::seek(from)` can skip `fst::floor_value` when `from` is at or past the
cursor's current key and still inside the open block: a binary search of the block's runs
from the current run, then the same run walk. When the walk runs out in the block's last
run, the answer depends on the next block, so the seek falls back to the index.

## Change (branch only)

- `termdict.hh`: `cursor` tracks `positioned_` (key_ is an entry of the open block); `seek`
  tries `seek_within` first; `land_in` is the shared landing (regular path and fast path).
- `bench/scan.cc`: `fseek_ns`, seeks that each move 1 to 300 entries past the last one
  (8 sweeps of the set, every seek checked against `std::lower_bound`).

## Results

Release, one thread, `quiet run explore-seek-a 20 "termdict forward seek"`, QUIET_LEVEL=light.
kv set, 1,000,000 keys, ns per seek, median of 7 repetitions per invocation. Seven
interleaved trials per binary, order rotated per trial. The three binaries are direct
`c++ -std=c++20 -O3 -DNDEBUG` builds of the same `bench/scan.cc`: `main-a` and `main-b` (the
same `main` header, the A/A pair), `fwd` (the branch header).

Experiment 1, the specified workload (steps 1 to 300):

| Binary | fseek median | range | seek10 median | range |
|---|---:|---|---:|---|
| main-a | 950 | 875 to 1024 | 1396 | 1316 to 1475 |
| main-b (A/A) | 969 | 879 to 1087 | 1424 | 1245 to 1618 |
| fwd | 1250 | 1108 to 1472 | 1638 | 1569 to 1904 |

fwd/main-a per trial: fseek 1.23 to 1.44 (median 1.32), seek10 1.06 to 1.30 (median 1.17).
A/A per trial: fseek 0.97 to 1.06 (median 1.02), seek10 0.95 to 1.10 (median 1.02).
Verdict: loser; the fast path makes forward seeks slower at this step size.

Mechanism, from a counting build (fast-path hits over 1 million keys):

| Step range | Seeks | Landed in the open block | Fell back to the index |
|---|---:|---:|---:|
| 1 to 2 | 666,532 | 96.6% | 3.4% |
| 1 to 32 | 60,649 | 62.5% | 37.5% |
| 1 to 300 | 6,615 | 7.2% | 92.8% |

Blocks hold up to 64 entries (2 runs of 32), so a 150-entry step usually leaves the block.
Each miss pays a run search and a run walk before the index lookup it was meant to skip.

Experiment 2, short steps (steps 1 to 16, the same harness with `% 16`), 6 interleaved trials:

| Binary | fseek median | range |
|---|---:|---|
| main-a | 849 | 694 to 1101 |
| main-b (A/A) | 871 | 728 to 1176 |
| fwd | 687 | 591 to 735 |

fwd/main-a per trial: 0.67 to 0.85 (median 0.81, 1.24x faster), outside the A/A band
(main-b/main-a per trial: 0.93 to 1.11) on every trial.
Verdict: win at short steps; loss at long steps. Only worth it with a long-step fallback
that costs nothing extra.

## Identity

- `termdict_test` (both trees): ok.
- Bench correctness: every seek in the random and forward sets checked against
  `std::lower_bound`; both trees exit 0.
- Cross-binary: a seeded mix of 400,000 operations on a 200,000-key set (forward, backward,
  absent, edge, next() runs), printed per landing: main and fwd outputs are identical
  (540,052 lines, 342,626 successful seeks, 24,271 failed ones).

## Next

1. Find a fallback that does not pay a run search and walk: the block would need its last
   key or a bound, which is a format change (bytes on disk change; format work is gated).
2. Or gate by the caller: a scan that knows its step is short can use a cursor method for
   the in-block case, so long-step callers keep the index path.
3. Recommendation: do not land the change as written; the short-step win is real, the
   long-step loss is larger in absolute terms at the specified workload.
