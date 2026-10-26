# Security review (decoder safety): fix/validate-at-open (termdict)

Reviewed `fix/validate-at-open` at 51b1163 (base `origin/main` 840d7b3) in the fresh worktree
`termdict-wt/review-vao` (detached at 51b1163). Read: the ledger `docs/status/validate-at-open.md`, the
diff (`termdict.hh` +16/-5, `test/test.cc`, `fuzz/fuzz_open.cc`, `fuzz/make_seeds.cc`, `bench/open.cc`;
the 675-file `fuzz/corpus/open` is data), `docs/reviews/seek-decode.md` and `forward-seek.md`, all of
`termdict.hh`, and the pinned fst (24da59a, `fst.hh` 677 lines) and varint (21af82a) sources, extracted
with `git archive` into `.scratch/deps/` so the build used exactly the CMake pins.

Threat model (from the ask): trusted cluster; the input is a dictionary file that may be corrupt (disk,
truncation, bit flips), read by a reader with or without a verifier hook. Attacker named per finding:
"corrupt file" = bit flips or truncation, with no intent. Nothing here needs a network attacker.

## Verdict: APPROVE

No blocking findings. Every probe came back clean under ASan+UBSan (Homebrew LLVM 23.1.2, x86_64
macOS): no out-of-bounds read, no unbounded allocation beyond a linear factor of the input, no
non-terminating lookup, and nothing but `termdict::corrupt` escaped `open`, `find`, `seek`, `scan` or the
cursor, except `std::bad_alloc` from allocation failure (SR1, not a format error). The four claims hold:

| Claim | Result | Evidence |
|---|---|---|
| Main already ran the full structural pass at open | Holds. `index_(alphabet_.parse(index))` builds `fst::map` from a `string_view`, the validating constructor (`fst.hh:151-152`) | `.scratch/main/termdict.hh`; `diff_main_main` below |
| The only library change is invalid_argument to corrupt | Holds, and it changes nothing else: of 2,082,552 mutated indexes the branch and main accept exactly the same set (44,452) | `idx2` vs `idx2_main`, digest `539b9a87cfd029a5` on both |
| Block offsets (FST values) are checked lazily and completely | Holds. The only consumers are `reader::open` (from `find_run`, `cursor::seek`) and `cursor::next` (offset derived from a block's own end); both end in `block`'s constructor | `offsets`, `blockhand`, mutants below |
| Open cost unchanged | Not measured (no timing allowed). By reading: the diff adds a `try` around a call main already made; the accept set is identical | `termdict.hh:790-799` |

## What was exercised

All probes are in `.scratch/probes/` (sources), `.scratch/bin/` (binaries), `.scratch/` (outputs,
mutants, logs). `build.sh NAME` builds a probe with ASan+UBSan against the worktree's `termdict.hh` and
the pinned deps (`INC_TD=` swaps in another header). Builds and runs went through `quiet light`, at most
4 jobs.

| Probe | What it does | Result |
|---|---|---|
| `diff_main.cc` -> `bin/diff_main` (branch), `bin/diff_main_main` (`-DMAIN`, main's header) | Every truncation and single bit flip of a real index (252 inputs); the exception type of `open` | Branch: 15 accepted, 237 `termdict::corrupt`. Main: 15 accepted, 211 `std::invalid_argument`, 26 `corrupt`. Same accept set; the branch converts the 211 |
| `idx2.cc` -> `bin/idx2`, `bin/idx2_main` | Six small indexes (alphabets of 1, 2, 3, 256, 8 symbols), every pair of positions set to every value of 24 dangerous bytes (headers, widths, varint continuation bytes, 0x7f, 0x80, 0xff...): 2,082,552 mutants, each opened and, if accepted, driven (find, seek, forward seek, scan, cursor walk x 3 key variants, hook on every other case) in exact-size heap buffers | 44,452 accepted and driven, 2,038,100 rejected, 0 non-corrupt exceptions, 0 sanitizer reports. `open-only` mode on the branch and on main prints the same digest |
| `offsets.cc` -> `bin/offsets` (`offsets 1 CFG`, CFG 0-4) | A structurally valid FST whose values are arbitrary: first keys "" and every pool byte, value x (and a second variant x + i*golden), x = every byte offset 0..blocks size + 20 and 2^31, 2^32, 2^63-1, 2^63, 2^64-2, 2^64-1; five dictionaries (256-byte alphabet, 3, 1, 26, 2 symbols; 3 to 300 keys); with and without a verifier hook that asserts every range it is handed lies inside the blocks | 37,164 cases (7,804 + 6,148 + 196 + 18,236 + 4,788), 0 unexpected exceptions, 0 sanitizer reports, 0 hook range violations. Roughly 26 million operations threw `corrupt` (the offset was not a block start), the rest answered |
| `blockhand.cc` -> `bin/blockhand` | Hand-built two-restart blocks (count 33, 40, 64): the second restart offset takes every value 0..entries+8 and 2^31..2^64-1; FST at offset 0 and 1 | 740 cases, 0 unexpected, 0 reports |
| Mutants of `termdict.hh` (`.scratch/mut1`, `mut2`, `mut3`; probes `offsets_mut1-3`, `blockhand_mut2`; contributor's test as `test_mut1-3`) | Remove the three lazy checks: `offset >= bytes.size()` (mut1), the restart offset upper bound `restarts_[i] >= end_ - entries_` (mut2), `length > end - p` (mut3) | All three die under ASan in my probes (mut1: SEGV in `block::block`; mut2: heap overflow in `block::run`, found by `blockhand`, not by `offsets`; mut3: heap overflow in `block::block`). The contributor's `termdict_test` also dies on all three. So the lazy checks are live and covered |
| `build-asan/termdict_test` (cmake, RelWithDebInfo, `-fsanitize=address,undefined`, FETCHCONTENT sources = the pins) | The branch's own test | `termdict: ok` |
| `build-asan/termdict_fuzz_open`, `fuzz-open2.log`, `corpus-vao/` | libFuzzer + ASan + UBSan, seeded from the committed corpus (new inputs to `.scratch/corpus-vao`, 73 files), `-max_len=8192 -timeout=20 -rss_limit_mb=4096`, 480 s | 2,470,920 runs, cov 898, no report. (A first 600 s run was stopped by a `quiet` strict hold: libFuzzer reported "timeout after 1200 seconds"; the input, saved as `.scratch/artifacts/timeout-*`, runs in 0.16 s, so it was the pause, not the code) |
| `mem.cc` -> `bin/mem` (plain -O1) | An index of N one-byte leaf nodes (header 0x01), root the last, key count 1: accepted by the validator. Peak RSS growth of `reader::open` | 33 to 36 bytes of peak RSS per index byte (N = 1M, 4M, 16M); see SR1 |

I also read the fst validator (`validate()`, `fst.hh:446-514`) line by line against `load`, `get`, and
`floor_value`, the three readers the branch can reach (termdict calls only `floor_value`):

- Every node is parsed once in address order; each transition target must equal an earlier node start
  (`target_index`, binary search), with delta >= 1 and <= address - 4. So the graph is acyclic, every
  address `floor_value` forms is a node start, and `floor_value`'s loops terminate: its first loop runs at
  most `key.size()` times and each step lowers the address; its descent follows last transitions down.
- Varints are bounded to 10 bytes and to the nodes' end; the runtime `get_varint` has no bound but only
  ever reads validated varints. A count of `31 + varint` can wrap to a small value on 64-bit; the
  validator and `load`/`floor_value` use the same wrapping expression on `uint64`/`size_t`, so they agree.
- The 8-byte `get_le` loads and the SSE label loads (not used by termdict) stay inside the 16-byte trailer.
- `ob <= 8`, `1 <= tb <= 8`, fields fit before the end, labels strictly ascend, <= 256 per node.
- Root equals the last node's start, key count equals the trailer's. Depth fits 32 bits.

## Findings

No blockers. Numbers are stable for a re-check.

- SR1 (low, non-blocking). Attacker: a corrupt or hostile index file. `fst::map::validate()`
  (`fst.hh:446-514`, reached from `reader::open_index`, `termdict.hh:792-800`) keeps three vectors, one
  entry per node (`starts` 8 bytes, `depth` 4, `keys` 8), with no reserve, and a node can be a single
  byte (leaf, header 0x01). An index of N leaf bytes with root = the last and key count 1 passes
  validation, so open allocates 20 bytes per index byte plus doubling slack: measured 33 to 36 bytes of
  peak RSS per index byte (`mem` N = 1M/4M/16M), against the ledger's 7.4 for a real index. The cost is
  linear in the input, so it is bounded, but a corrupt 1 GiB index file would need about 33 GiB of
  transient memory, and `std::bad_alloc` is the one exception `open` can throw that is not
  `termdict::corrupt`. Not a blocker for a trusted cluster whose index is a small fraction of the file.
  Suggest, in the fst repo, dropping or shrinking the per-node vectors (the depth and key-count
  bookkeeping), and one line in the `reader` doc saying that open allocates about 7 to 35 times the index
  size transiently and can throw `bad_alloc`.
- SR2 (low, non-blocking, coverage). `fuzz/fuzz_open.cc` constructs the reader without a verifier hook, and
  `test/test.cc` has no hostile-offset case with a hook, so `reader::open`'s hook branch
  (`termdict.hh:803-815`) is covered for hostile offsets only by my `offsets` probe (hook asserts every
  range is inside the blocks: 0 violations). Suggest one input bit in `fuzz_open` selecting a hook that
  returns true and aborts on an out-of-range range.
- SR3 (info, non-blocking, dev tool). `bench/open.cc:89-90` (`time` mode) reads an 8-byte header from a
  slurped file with `memcpy` before checking the file holds 8 bytes, and computes
  `bytes.size() - 8 - n` without checking `n`. It only reads files its own `build` mode wrote, and ships
  in no library target (`termdict_open_bench`); a truncated tables file would read out of bounds. A size
  check closes it.
- SR4 (info). The ledger says the library change is "+11 lines"; the diff is +16/-5 in `termdict.hh`
  (one doc comment, the two constructors, `open_index`). Nothing else in the header changed.
- SR5 (info, residual, unchanged by this branch). A reader without a verifier hook answers
  from corrupt block bytes that pass the structural checks (wrong value or key, no `corrupt`): the
  memory-safety properties hold, correctness does not (seek-decode CR2). Callers that need detection
  must install a hook; the ledger says so. A hook protects blocks only, not the
  index (the index is the structural pass above plus nothing): an index bit flip that passes validation
  (44,452 of 2,082,552 mutants did) sends lookups to the wrong block, with no error.

## Conditions

None. SR1 to SR3 are follow-ups, not gates.

## Not run

- Timing of any kind: the 5.8 ms open claim and the claim that cost is unchanged (reasoned only; the
  accept set is identical and the code added is a `try`).
- Big-endian and 32-bit targets (fst's `get_le` documents little-endian hosts only), MSVC, other
  platforms (left to CI), TSan (the reader is read-only, no concurrency in the diff).
- A 4 GiB-deep FST (depth over 2^32 - 1) and index files larger than a few tens of MB.
- The strata and prospect consumers.
- Re-measuring the 13.6 M-run, 35-minute fuzz in the ledger: my own run was 2.47 M runs (480 s, seeded from
  the committed corpus) plus the 2.08 M-mutant pair sweep and the 37 K-case offset sweep.

## Probe index

`.scratch/probes/{common.hh,build.sh,diff_main.cc,idx2.cc,offsets.cc,blockhand.cc,mem.cc}`;
outputs `.scratch/{offsets.N.out,idx2.out,fuzz-open2.log,test_mut?.out}`; mutants `.scratch/mut{1,2,3}/`;
main's header `.scratch/main/termdict.hh`; pinned deps `.scratch/deps/{fst,varint}`; fuzz output corpus
`.scratch/corpus-vao`; cmake tree `build-asan/`.
