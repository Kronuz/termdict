# Code review: perf/seek-decode (termdict)

Reviewed `perf/seek-decode` at 9714c6a (`origin/perf/seek-decode` is the same commit; base
`origin/main` eaf83b3), three commits touching `termdict.hh`, tests, fuzz target, bench, docs.
Read first: strata's `docs/reviews/calibration.md` (applied: HEAD equals origin; CI checked per
platform; every claim below names its probe). Light/no holds: a strict hold covered 17:08-17:32,
so nothing was compiled until it cleared; everything ran at `nice -n 19`, at most 4 jobs.

## Verdict: APPROVE TO LAND

No blocking findings. The over-read, carry-test, seek, differential and consumer questions all
came back clean under ASan+UBSan (Homebrew LLVM 23, x86_64 macOS). Non-blocking notes below.

CI on 9714c6a, read with `gh api .../check-runs`: all 8 green (linux x86_64 clang and gcc, linux
arm64 gcc, linux asan-ubsan, linux tsan, macos x86_64, macos arm64, freebsd x86_64).

## What was exercised

| Probe (all in `.scratch/review/`) | What it does | Result |
|---|---|---|
| `swar.cc` (`build.sh`, `cc.sh`) | Opens `suffixes` (`#define private public`), runs `decode` and `decode_padded` against a bit-unpacking oracle: every width 1-8, every alphabet size that has that width (1, 2, then 2^(W-1)+1..2^W), 9.6M trials, n 0-49, 0-10 skipped symbols (all bit offsets), random garbage in the skipped symbols and in the run's bits past the suffix (including codes >= size), 0-19 bytes after the run or none, source and destination in exact-size heap allocations | 0 mismatches, 0 ASan/UBSan reports; throws exactly when an in-suffix code >= size (3.8M throws) and never for garbage outside the suffix; unpadded decode writes exactly n bytes; `at_` advances n*W |
| `mut/*.d`, `bin/t_*` | Six mutants of `termdict.hh` run through `test/test.cc`: carry lane shifted, check removed, padding mask removed, `lift` off by one, seek restart compare `<=` to `<`, landing prefix one short | Five die (`m1`, `m2`, `m6` on CHECKs; `m3`, `m5` abort on a spurious corrupt). `m4` survives and is an equivalent mutant (a restart key equal to `from` then lands via the previous run's end and `next()`, same key, one extra run walk) |
| `seek.cc` | 24,000 seeks from any cursor position against `std::map::lower_bound`: alphabets of 1, 2, 3, 5, 11, 53, 129, 256 symbols (and with `\0`, `\xff` added), coded and byte-coded, empty key, empty `from`, `from` = key, key+`\0`, key+`\xff`, key minus last byte, predecessor+`\xff\xff`, past the last key, before the first, then walks of 0-149 `next()`s, to the end and a second `next()` after it; `scan` compared too | 0 failures |
| `seek.cc` (same binary) | For each of 60 dictionaries: every truncation (or 150 random ones above 3 KB) plus 150 random byte/truncation corruptions, each block buffer an exact-size `malloc` so the block end is the allocation end; each walked with `next()`, `seek` x12, `find`, `scan` | 0 sanitizer reports; only `termdict::corrupt` escapes |
| `diff.cc` -> `bin/diff_main` / `bin/diff_branch`, `cmp.py` | Same seeded transcript against `origin/main`'s header and the branch's: 60 dictionaries (alphabets 1-256, coded and byte-coded, keys to 80 bytes, values 0-50 bytes), 60 `find` and 60 `scan` probes each, a full cursor walk, then 25 corruptions each (bit flips, truncations) | 182,710 lines. Clean tables: `find`, `scan`, `walk` identical on every line. All 7,212 differences are `scan` on corrupted bytes (see CR2) |
| `build-asan/termdict_test` (cmake, `-fsanitize=address,undefined`) | The branch's own test, Debug | `termdict: ok`, 45 s |
| `main_test.cc` -> `bin/main_test_on_branch` | `origin/main`'s unmodified `test/test.cc` compiled and run against the branch header | ok (additivity) |
| `build-fuzz/termdict_fuzz`, `fuzz-run.log`, `corpus/` | libFuzzer + ASan + UBSan, the branch's `fuzz/fuzz_reader.cc` (now checking seek), 4 minutes, `-max_len=4096`, under `quiet run` (light) | 691,870 runs, no crash, no leak |
| `maxkey.cc` | 70,000-byte and 70,001-byte keys (multi-byte LEB continued lengths), values to 300 bytes, 40 keys, coded and byte-coded, seek + next against `std::map` | 0 failures |
| `reuse.cc`, `reuse2.cc` | A cursor whose `seek` throws `corrupt` from `open()`, then `next()` | Safe: `block_.emplace(r_->open(..))` evaluates `open` before touching `block_`, so `block_`, `at_` and `count_` still agree and `next()` continues in the old block. Not a finding |
| `build-strata/` (Ninja, ASan+UBSan, `FETCHCONTENT_SOURCE_DIR_TERMDICT` = the worktree) | strata `core_kv_test`, `index_test`, `lifecycle_test` | all ok; `-I` confirmed to point at the worktree |
| `build-prospect/` (same flags, strata from `~/code/Kronuz/strata`, termdict = the worktree) | prospect `concepts_test`, `segment_test` | both ok |
| syntax check, strata `bench/kv/proto/{cached,blocks}.cc` | These use `block::header`, `block::suffixes`, `run`, `compare_restart` directly | compile clean against the branch |

## Answers to the four asks

1. Over-reads. Every read in the new decode is bounded by the block's `end_`, and `end_`
   is already checked against the mapped bytes by `block`'s constructor (`termdict.hh:237-247`):
   - The chunk decode loads through `load(p, end)` (`termdict.hh:315`), which takes 8 bytes
     only when `end - p >= 8` and otherwise reads the remaining bytes one by one (zero when
     `p >= end`). `p` is `bits_ + (at >> 3)` with `at` never beyond the suffix's last bit
     (`start()` checks `n * W <= size_ - at_` first), so `p <= end_`.
   - The 16-byte copy (`termdict.hh:361`) is guarded by `end_ - from >= 16`; otherwise a
     length-n `memcpy` that `start()` already bounded.
   - The writes past the suffix: a padded chunk writes `per` bytes (7 to 16) for its final
     chunk of `k < per` symbols, so at most 15 bytes past the key; the 16-byte copy writes
     exactly 16 past `shared`. The buffer is sized `size + 16` before every padded decode
     (`step` and `land`), and `shared <= prev` is checked before the resize, so the retained
     prefix is never clobbered. `reader::cursor` is the only padded user (`decode_padded` is
     private to `block`).
   - Evidence: `swar.cc` (exact-size heap source and destination, 9.6M trials, with 0-byte
     slack after the run), `seek.cc` (every truncation of 60 dictionaries, exact-size heap),
     the 4-minute fuzz, the CI asan-ubsan job.
2. The carry test. `swar.cc` agrees with the per-symbol check on 9.6M trials covering all
   widths and sizes, including size = 2^W (lift 0) and size 1 at width 1, garbage in the
   skipped symbols, garbage after the suffix and in the unmasked lanes. Reasoning matches
   (`termdict.hh:353-391`): each lane receives `2^W - size`, so a lane overflows iff its code
   is >= size; a valid code never carries and so never feeds a carry into the next lane; the
   lowest bad lane's carry is therefore genuine, and `(sum ^ v ^ lift) & (ones << W)` reads
   the carry-in bit of every lane above (including the one above the top lane), so any bad
   lane sets a bit. `per * W <= 57` keeps every shift and the `carries` mask under 64 bits.
   Lanes past `k` are zeroed by the mask (zero is a valid code for any size >= 1).
3. `seek`. Clean on every position listed in the ask (`seek.cc`; the branch's `check_seek`
   adds block-boundary and run-boundary cases). The no-floor branch resets state and calls
   `next()`; the all-below branch moves to the next run's first entry or the next block;
   `next()` after a false seek stays false (`at_.index == count_` and `next_` unchanged).
4. `find_from` and `scan`: identical to main on every uncorrupted case (`diff.cc`). The
   branch's `walk_to` is a straight extraction of main's loop; the three-way branch order
   (`shared > m` below, `shared == m` compare, `shared < m` past) is preserved, and `visit`
   still sees the landing entry.
5. Additivity: removed or changed names in the diff are all private members (`cursor::p_`,
   `s_`, `index_`, `reader::cursor::offset_`, `block_`, `entry_`); `header`, `suffixes`
   (`start`, `decode`, `skip`, `next`, constructor), `block::cursor`, `run`, `compare_restart`,
   `find_from`, `scan`, `reader::cursor` keep their signatures. New public names: `detail::fail`,
   `detail::leb`, `reader::cursor::seek`. main's unmodified test passes on the branch; strata and
   prospect build and pass against it.

## Findings

No blockers. Each note is non-blocking and becomes a follow-up, not a second review.

- CR1 (non-blocking, test gap). The suite's truncation sweep (`test/test.cc:301-313`) hands the
  reader a `std::string::substr`, whose allocation can extend past `size()`, and calls only
  `find`; it does not drive `cursor`, `seek` or `scan` on truncated blocks, and has no case
  with garbage in lanes past a suffix. The new decode's safety argument rests on the block end
  being the allocation end, so the strongest guard is a truncation/corruption sweep over an
  exact-size `malloc` through `cursor::next`/`seek`/`scan` under ASan, which is what
  `.scratch/review/seek.cc` does (the CI asan-ubsan job would catch an over-read then). Also
  worth porting `swar.cc`'s size/width sweep (it kills mutants `m3` and `m5` today only because
  the width test happens to reach them). Probes: `seek.cc`, `swar.cc`, `mut/`.
- CR2 (non-blocking, behaviour note). `reader::scan(from)` on corrupted block bytes behaves
  differently from main: it no longer decodes the entries before `from` in the block, so some
  corruption that main reported as `corrupt` is not noticed (857 of 7,212 differing lines in
  the `diff.cc` run: main threw, branch answered), some that main did not notice now throws
  (5,493, from the restart-key binary search and the walk over the run), and a few answers
  differ (315). `find` already behaved this way. This only matters for a reader without a
  `verify_` checksum hook, and a corrupted block that passes the structural checks can answer
  wrongly on either version; callers that rely on checksums (strata, prospect) are unaffected.
  Suggest one sentence in the README or `docs/perf/seek-decode.md` saying `scan` and `seek`
  validate only the entries they touch. Probe: `diff.cc` + `cmp.py`, `out_main.txt`,
  `out_branch.txt`.
- CR3 (non-blocking, note). The mutant `m4` (`compare_restart(mid, from) < 0` in the seek
  binary search) passes every test. It is equivalent in results (one extra run walk), so it is
  not a coverage hole; mentioned so a later mutation run is not surprised.

Design remark for the architect (not a code finding): `max_key_bytes` is `fst::max_key_length`
(2^32 - 1), so a single key can need a 4 GiB padded cursor buffer that never shrinks. That is the
same ceiling the builder enforces, and the padded buffer only retains the longest key seen, but a
long-lived merge cursor over a dictionary with one huge key keeps that memory until it is
destroyed. I did not run a 4 GiB case; `maxkey.cc` used 70 KB keys.

## Conditions

None. (Land when the owner is ready; CR1 and CR2 as follow-ups.)

## Not run

- Benchmarks, `termdict_scan_bench` and the perf numbers in `docs/perf/seek-decode.md`
  (review rules forbid it; the speedup claims are unverified here).
- Big-endian and 32-bit targets (the `bswap64` path in `load` is untested here), MSVC/Windows,
  Linux arm64 and FreeBSD locally (CI covers them green).
- TSan locally (CI `linux-tsan` is green; the reader and cursor share no mutable state).
- Fuzzing beyond 4 minutes; 4 GiB keys; blocks mapped from a real `mmap` with a guard page
  (exact-size heap allocations under ASan used instead).
- strata's and prospect's full ctest suites (ran their termdict-using tests: `core_kv`, `index`,
  `lifecycle`; `concepts`, `segment`).
- `strata` and `prospect` were built from their local checkouts (`6e13416`, `0c895cb`) with deps
  from strata's existing build directory plus fetched `crc32c` and `allocators`, not the exact pins.
