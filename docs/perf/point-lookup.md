# Point lookup (issue 1)

Status: explored on branch `explore/point-lookup`; three experiments, none kept. Issue 1 stays open.

## Ledger

- State: loss reproduced; stage costs measured; E1-E3 measured and dropped.
- Next: a structural change inside the run (see "What would settle it"); the performance engineer.
- Blocked on: nothing.

## Reproduction

`termdict_bench /usr/share/dict/words`, i9-9980HK, macOS 15.8.1, Apple clang 17, Release, `quiet strict`.
The first A/A (the same binary twice, 3 interleaved trials) ran while other agents loaded the host and
all columns read 3-4x the issue's figures (termdict 1,559-1,704 ns, `fst::get` 392-467, hash map
141-197 ns); the A/A spread of termdict was 3% and the ratio termdict : `fst::get` was 4.0 (3.35 in
the issue). The loss exists on head. The stage numbers below come from `.scratch/stage.cc` (the bench's
key set and shuffled present queries, medians of 1,000 batches of 1,024) on a quiet host.

## Where the time goes (ns per present lookup, cumulative)

| Stage | ns | Share |
|---|---:|---:|
| `fst::map::floor_value` | 126 | 34% |
| + `reader::open` (block header, restarts) | 146 | 5% |
| + restart search (one `compare_restart`) | 182 | 10% |
| + `walk_to` (up to 32 entries) | 364 | 50% |

The same key repeated (everything cached, branches learned): 257 ns, of which floor 55-75, walk about
160. So the walk is compute-bound at about 10 ns per entry, not cache or misprediction bound
(random keys add about 25 ns to the walk). Cache misses show up in the floor (126 random against 55-75 hot).

## Experiments

Stage figures, three rounds per binary, interleaved base/variant.

| # | Change | Base ns | Variant ns | Verdict |
|---|---|---:|---:|---|
| E1 | `restart_interval` 16 (walk halves, two compares) | 372 | 335 | -10%, but the format changes (+2.3% bytes), `cursor::forward` assumes at most two runs; dropped |
| E2 | `walk_to` compares a suffix by `decode_padded` chunks and a byte loop, not `next()` per symbol | 360 | 395-405 | 10% slower; dropped |
| E3 | `walk_to` header fast path (one-byte lengths and shared), landing state rebuilt lazily | 361 | 357 | within the spread (about 15); dropped |

## What would settle it

Floor is a third and not termdict's to change. The walk is half and its per-entry cost is serial
(header parse, then the next entry's address). Candidates, none tried: a per-run skip table (every
8th entry's offset and bit position, a format change and 3-4 bytes per 32 entries) so the walk starts
within 8 entries of the key; or interval 16 with `max_block_entries` 32 (twice the index entries).
The absent-key `fusefilter` idea in the issue is untried.
