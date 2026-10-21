# Code review: perf/forward-seek (termdict)

Reviewed `perf/forward-seek` at 381e1d0 (`origin/perf/forward-seek` is the same commit; base main
a91e9b6), five commits touching `termdict.hh`, `test/test.cc`, `fuzz/fuzz_reader.cc`, `bench/scan.cc`
and docs. Read first: the write-up (`docs/perf/forward-seek.md`), the ledger, the previous review
(`docs/reviews/seek-decode.md`). Light holds only: everything ran at `nice -n 19`, at most 4 jobs,
under `QUIET_LEVEL=light quiet run review-fwdseek`. No timing was taken.

CI on 381e1d0, read with `gh api .../check-runs`: all 8 green (linux x86_64 clang and gcc, linux arm64
gcc, linux asan-ubsan, linux tsan, macos x86_64, macos arm64, freebsd x86_64).

## Verdict: APPROVE WITH CONDITIONS

Seek is correct: no wrong answer from any probe, on any cursor state I could construct, valid tables or
transient `open()` failures (about 23 million differential seeks, 0 mismatches). The decoder additions
are memory safe on truncated and flipped blocks in exact-size heap buffers (ASan+UBSan, no report).
One latent state bug (CR1: a cache field is assigned before the open that can fail) does not change any
answer but breaks the stated invariant "the cache never outlives its block"; it is a one-line fix with
a probe that proves it, so I make it a condition rather than a blocker. The rest are non-blocking.

Blocking items: none.

Conditions (one fix round, then land):

1. CR1: assign `offset_` (and reset the marks) only after `block_.emplace(r_->open(offset))` has
   succeeded, and add a regression test (a verifier that rejects one open; `flaky.cc` is the model).
2. CR3: a test that a seek ahead after `next()` takes the forward path (the mutant that drops
   `on_entry_ = true` from `next()` survives the whole suite; `afternext.cc` is the model).

## Findings

### CR1 (condition): after a failed `open()` the cache describes a different block than the open one

`termdict.hh:757-762` (`cursor::open`): `last_known_ = tried_ = false` and `offset_ = offset` run before
`block_.emplace(r_->open(offset))`. When `open` throws `corrupt` (a verifier that fails, a transient
read error), `block_`, `at_` and `key_` stay on the old block while `offset_` already names the new one
and the marks are reset. The old block then relearns `last_` under the new block's offset; when
`next()` later crosses into that block, `open(next_)` sees `offset == offset_`, keeps the old block's
`last_`, and the new block starts with another block's last key.

Probe `.scratch/review/flaky.cc` (clean tables, a verifier rejecting 5-45% of calls at random, one
cursor continuing after every throw, ~250,000 seeks): `offset_` names a block other than the open one in
about 16% of the 120,000 per-op observations of a run, and `last_` differs from the open block's true last
key in about 2.5% of the observations where it is known and `offset_` is right (`offset_names_other_block=19269
last_checked=30608 last_stale=756`, seed 1; seeds 2 and 3 alike).

It never changes an answer: `last_` only decides whether `forward` is tried past the cheap compare, and
every `true` it returns is decoded against `from` (the restart binary search and the run walk do not
use `last_`); a too-small `last_` makes every forward seek in that block fall back (slow, correct), a
too-large one runs the run walk, which returns false and falls back. `flaky.cc` checks every returned
seek against `std::lower_bound` through all of it: `fails=0` on every seed. So this is a perf and
invariant bug on a transient-failure path, not a correctness bug.

Fix, proved with the probe: move `offset_ = offset;` after the `emplace` (mutant `F7` in `.scratch/
review/mut/`, built into `bin/flaky_F7`): `offset_names_other_block=0 last_stale=0`, `fails=0`, seeds 1
and 2. The reset line before the `emplace` can stay.

### CR2 (non-blocking): on corrupt bytes a seek can now throw where main served it

`block::last_key` decodes the block's last run on the second forward attempt. When that run is corrupt
and the target is in an earlier run, the seek throws `corrupt` where main's seek (which touches only the
target's run) returned. Probe `corrupt.cc` (6 dictionaries, 4,199 truncated/flipped tables, 16,796
cursors continuing after throws, 1.06M seeks, exact-size heap buffers, ASan+UBSan; same source built
against main's header, per-op transcripts diffed with `cmp.py`):

- 7,693 seeks throw on a used cursor that a fresh cursor serves; main has 0 (its seek is history
  independent). An instrumented header (`.scratch/review/inst/`) counted 13,012 `last_key` throws,
  which accounts for them (the forward binary search also reads different restarts than the full one).
- On the 1,331 tables whose full scan is strictly ascending and whose index agrees with the blocks
  (549 of them with keys different from the originals), a used cursor and a fresh one never disagree:
  `clean_bad=0`. On the remaining tables 508 seeks return different keys on a used cursor and a fresh
  one (unsorted or index-inconsistent data, where "first key >= from" is not defined).
- No sanitizer report anywhere; only `termdict::corrupt` escapes.

Readers with a verifier (strata's checksum) reject such blocks at open, so this only shows on
unverified readers. Worth one sentence in the write-up ("a corrupt last run can fail a seek that does
not reach it"); not a reason to hold.

### CR3 (condition): nothing pins that `next()` sets `on_entry_`

`termdict.hh:671`. Mutant `F3` (delete `on_entry_ = true` from `next()`) survives the full suite
(`mut/results-O2.txt`), and `afternext.cc` shows the effect: after `next(); next(); seek(ahead)` the
branch opens 0 blocks, the mutant opens one block per seek. `next()` then `seek()` ahead is the pattern
a merge cursor uses, and the write-up states it ("set by a successful `next()`/`seek()`"); every
`check_forward` probe starts with a `seek`. Add a probe that calls `next()` instead of `seek()` to reach
the starting entry.

### CR4 (non-blocking): the `on_entry_` clear in `next()` is a safety invariant nothing pins

`termdict.hh:665`. `forward` computes `(at_.index - 1) / restart_interval`, which underflows if
`on_entry_` is true while `at_.index == 0` (a block just opened, `step` throwing on its first entry).
The branch is safe: `next()` and `seek()` clear the flag first and set it only after a successful
decode. Mutant `F4` (drop the clear in `next()`) survives the suite and the ASan test too, and
`nextthrow.cc` (38,156 truncated and flipped tables, 22,015 where `next()` threw, 132,090 seeks after the
throw, ASan+UBSan) did not make it misbehave, because `open()` resets `tried_`, so the first forward
attempt in a freshly opened block always returns before the index math. Safe today by two independent
coincidences. A one-line comment on `on_entry_` ("implies `at_.index >= 1` and `block_`") or an
`assert` would keep the next edit honest.

### CR5 (non-blocking): `forward`'s `walk_to` miss is "impossible" only because blocks have at most two runs

`termdict.hh:750`: `return false;  // only unsorted data`. For a target above every key of run `lo` but
below run `lo+1`'s restart key, `walk_to` returns false and `forward` falls back to the full path (a
re-open: correct, not cheap). With `max_block_entries = 64` and `restart_interval = 32` there are at
most two runs, so `lo > run` implies `lo` is the last run, and `from <= last_` makes the miss
impossible. If `max_block_entries` grows, `check_forward`'s "exactly when" assertion starts failing and
the comment is wrong. A `static_assert(max_block_entries <= 2 * restart_interval)` next to the comment
states the dependency.

### CR6 (non-blocking): `last_key` mutants

The author could not make a mutant of `last_key`; I made 14 (`.scratch/review/mut/gen.py`, `mut/run.sh`,
results in `mut/results-O2.txt` and `mut/results-asan.txt`, built against `test/test.cc` unmodified).

| Mutant | Result |
|---|---|
| L3 unclamped decode length (`suffix[i]`, overwrites later entries' bytes) | killed (`test.cc:186`) |
| L4 `need = shared[i]` dropped | killed |
| L5 decode from bit 0 | killed |
| L9 `resize(length + 1)` | killed |
| L11 `skip` dropped | killed |
| L12 `decode_padded` instead of `decode` | killed |
| L6 length = suffix only, L7 previous run, L8 `n` = full run, L10 bit offset taken after skip, L15 value bytes not skipped | killed (uncaught `corrupt` on valid data) |
| L14 `s.start(suffix[i])` removed | survives the plain build, killed by ASan (byte-coded `memcpy` reads past the block; `start()` is memory-safety load-bearing there) |
| L1 `shared[i] > length` check removed | survives, also under ASan: `last_key` then returns a garbage key on a corrupt run instead of throwing; no overflow (the clamp keeps writes inside `length`), and `last_` only gates the fast path |
| L2 `need < shared` (for `<=`), L13 early stop removed | survive; equivalent (decode of 0 symbols; no early exit costs only time) |

So `last_key` is well covered by `check_forward`'s "equals each block's true last key" check; the two
real survivors (L1, L14-without-ASan) are detection-only and the CI asan-ubsan job covers L14.

### CR7 (non-blocking, for the architect): the last key is a format question

The write-up already says it: storing a block's last key (or its length) would remove the first-attempt
mark and make "exactly when the target is in the block" true on every attempt, and removes `last_key`'s
decode. It is a format change; formats are open until release. Not for this branch.

## Criteria

1. Correctness. `fwd_diff.cc`: random dictionaries (alphabets of 1-256 symbols with 0x00/0xff, coded and
   byte-coded, random keys, long shared prefixes, prefix chains, keys to 400 bytes, the empty key, 0-2,500
   keys), cursors driven with random `next()`/`seek()` interleavings of 300 ops, targets from the
   cursor's position, block last keys and first keys, restart keys (positions 31/32), their `+\0`,
   `+\xff`, prefix, decrement, increment and random-suffix variants, backward targets and empty `from`;
   every result against `std::lower_bound`. Includes seeks after `next()` returned false, after seeks
   that failed, and a `next()` right after. 40 seeds x 200 dictionaries (23.1M seeks): `fails=0`; the
   forward path was taken (no block opened, moved ahead) in about 34,000 seeks per seed, so it was not
   silently falling back. Throwing seeks: `flaky.cc` (above). The branch's own `check_forward` also passes.
2. Decoder safety. `corrupt.cc`, `nextthrow.cc` (above) plus the branch's own `termdict_test` (truncation
   and flip sweep driving a cursor with forward seeks): no ASan or UBSan report. The branch's own test
   reaches `last_key` (an instrumented header, `inst2/`, `bin/test_inst`: 70,195 calls, 693 of them throwing
   on its corrupt blocks); `corrupt.cc` threw from it 13,012 times. The reads in `last_key` are bounded by the same `header()`
   and `suffixes::start()` checks as `step()` (L14 shows `start()` is load-bearing and ASan-visible). The
   cache is per cursor, keyed by offset, and CR1 is the one place it goes wrong.
3. Tests catch what they claim. `check_forward`'s counting verifier is sound: it killed F1 (equal case
   removed), F5 (`size_` not set), F6 (`<` to `<=`), F8 (marks not reset on a new block), F9 (`seek`
   not clearing `on_entry_`), F10, F11 (`>` to `>=`), F12 in my run, as it did the author's four. Misses:
   CR3 (F3), CR4 (F4); F2 (`run` off by one when the current entry ends a run) survives and is equivalent
   (stepping the next run from its first entry is also correct).
4. Additivity. Main's unmodified `test/test.cc` compiled against the branch header passes
   (`main_test.sh`, `termdict: ok`). Public surface: only private members of `cursor` and `block` are added (`git diff
   main..HEAD -- termdict.hh`); `cursor` is still non-copyable and `sizeof` grows (a `std::string`, an
   offset, three flags). strata main and prospect main (`git archive` copies of `main`, 22173ba and
   59f36fe, in `.scratch/review/`) configure and build (`ASan+UBSan`, `FETCHCONTENT_SOURCE_DIR_TERMDICT` =
   this worktree; `-I` confirmed in `build.ninja`): strata `scan_test` (plain
   and with `STRATA_FAULTFS_MAP=1`), `core_kv_test`, `keys_fuse_test`, `merge_policy_test`, `index_test`
   and prospect `concepts_test` pass (`consumers-tests.log`). prospect `segment_test` did not finish in
   a 15-minute budget under Debug ASan+UBSan (no output; exit 124 from `timeout`), strata `lifecycle_test`
   and prospect `search_test` were stopped after 10+ minutes without finishing, so those three are
   unverified here, not failed; no baseline run on main's termdict was made to tell slow from stuck.
   Neither consumer takes the forward path: strata's `keys::reader::range` seeks once and then calls
   `next()` (`keys.hh:272`), prospect's `term_cursor` only calls `next()`; so they prove additivity, not
   the forward path.

## Probes and artifacts (all in `.scratch/review/`)

`cfg.sh`, `cc.sh` (build scripts), `fwd_diff.cc` (differential), `flaky.cc` (open failures, cache
invariants), `corrupt.cc` + `cmp.py` (corruption, history independence, main-vs-branch transcripts),
`nextthrow.cc`, `afternext.cc`, `mut/gen.py` + `mut/run.sh` (27 mutants), `inst/`, `inst2/`
(instrumented headers), `main/termdict.hh` (main's header), `cfg-consumers.sh`, `main_test.sh`,
`strata-main/`, `prospect-main/`, logs and `bin/`.

## Not run

- Benchmarks and every perf number in `docs/perf/forward-seek.md` (the review rules forbid timing; the
  1.9x claim is unverified here, and the doc's own table shows terms random seek+10 6% slower inside a
  3% A/A).
- Fuzzing: the branch's libFuzzer target was not rebuilt or run here (the 61 s run in the write-up is the
  author's); `corrupt.cc`/`nextthrow.cc` cover the same bytes under ASan instead.
- TSan locally (CI linux-tsan is green; a cursor is single-threaded and the reader is const).
- Big-endian, 32-bit, MSVC; Linux and FreeBSD locally (CI covers them).
- A multi-run block (three or more runs): the constants forbid it (CR5).
- prospect `segment_test`, `search_test` and strata `lifecycle_test`: built, but too slow under Debug
  ASan+UBSan to finish in the budget (above); the rest of both suites was not run.
