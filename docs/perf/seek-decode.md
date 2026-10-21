# Seek and sequential decode

Status: done on branch `perf/seek-decode` (kept: E1, E2, E3 with E4a, E6, E8, and the seek; removed: E4b, E5, E7).

## Ledger

- State: built, measured on the Mac and the gate VM, CI green; not merged (the owner lands it).
- Done: `e4314a5` (seek, E1-E6, bench, tests, fuzz), `01457e2` (E8, docs), and this note.
- Next: a `seek` fast path for forward seeks inside the open block (skips `floor_value`,
  the larger part of a seek); the remaining decode is the table lookup per symbol, which an
  SSSE3/NEON shuffle could do 16 at a time for alphabets of 16 or fewer (strata's numeric
  keys) behind a runtime check.
- Blocked on: nothing.

## Target

1. Positioning a cursor (strata format 14's split layout: seek and 10, YCSB E; prospect's
   expansions re-seeking): `reader::scan(from)` builds every key of `from`'s block from
   its start, up to 63 of them, before reaching `from`.
2. Sequential decode (`reader::cursor::next`, strata's key-ordered scans and merges,
   prospect's term iteration): 32-36 ns per key on the Mac and the gate VM in strata's
   format-14 measurement (`strata/docs/design/format-14-kv/q1-q2.md`), more than RocksDB's
   inline layout spends on a whole record. Target about 15 ns per key (an estimate).

Bytes on disk stay identical; the API change is additive (`reader::cursor::seek`).

## Workloads

`bench/scan.cc` (`termdict_scan_bench`), two key sets:

- kv: strata's KV keys, "Q" and 15 decimal digits of a scattered id (strata's
  `bench/kv/kvgen.hh`), 1,000,000 keys, each value its LEB128 ordinal (11-symbol
  alphabet, 4-bit codes).
- terms: `/usr/share/dict/words` (235,976 terms, 6-byte values, as prospect's term infos;
  53-symbol alphabet, 6-bit codes).

Per set: a full `reader::cursor` walk per key, seek and 10 (half present, half absent
targets), the same through `reader::scan(from)`, and `find` of present and absent keys.

## Procedure

Release builds (`-O3 -DNDEBUG`), one thread, light quiet holds. Each experiment is built
from the same `bench/scan.cc` against two `termdict.hh` (`.scratch/mk.sh`), and the
binaries run interleaved with the order alternating per trial (`.scratch/ab.sh`), five
trials or more, each figure the median of its repetitions. An A/A pair (one header built
twice) runs first on each platform to state the noise. Mac: i9-9980HK, macOS 15.8.1, Apple
clang 17. VM: the Linux gate VM (Ubuntu 24.04 under QEMU/HVF on the same host), clang 18.1.3.

Keep a change if it is faster beyond the A/A spread on both platforms and slower on
neither key set or on `find`; remove it otherwise.

## Profile (before)

`perf record -e cpu-clock` on the VM (the guest has no hardware counters), the walk alone:

| Where | Share | Why |
|---|---:|---|
| `block::suffixes::decode`, out of line | 34% | `57 / width_` per call (a 32-bit `div`, 27% of the function); `mask_`, `width_`, `a_`, `a_->size` reloaded after every symbol's char store (a char store may alias them); `shr %cl` by a variable width (2-3 uops on Intel without BMI2) |
| `block::cursor::next`, out of line | 11% | |
| libc | 13% | mostly the bench's setup sort (a call-graph profile); `key_.resize` zero-filling only on keys that grow, which costs terms, not kv |
| `block::header`, out of line | 8% | five `throw` sites keep it from inlining |
| the bench's loop (`reader::cursor::next` inlined) | 9% | |

Seeks and finds (`perf record`, `find` and seek and 10 alone): `fst::floor_value` 29-34%,
37% of it on the instruction after a node's header load; the block constructor 7-8%, 76% of
it on the block's first byte; the run walk 19-26%. Both are cache misses at 1,000,000 keys
(10 MB of blocks, about 400 KB of index), so a seek's remaining cost is mostly memory.

## Experiments

Mac, quick interleaved A/Bs (4-5 trials of 3-5 repetitions); keyscan medians, ns per key.
Each row is against the row before it; host noise moved the same binary by up to 10%
between sessions, so the A/A spread is restated with the final A/B.

| Step | Change | kv | terms |
|---|---|---:|---:|
| base | `main` | 27.9 | 35.8 |
| E1 | Cold `fail()` for every throw, a one-byte LEB128 fast path, `decode` on locals (no reloads after char stores), symbols per load from a table (no `div`) | 25.7 | 33.4 |
| E2 | `reader::cursor` decodes into its own buffer that never shrinks: no `resize` per key | 24.2 | 23.2 |
| E3 | `decode` dispatches once on the width: shifts become immediates | 23.0 | 25.2 (lost: three out-of-line calls per key) |
| E4a | `header` and `decode` always inlined into the one out-of-line `step` | 20.5 | 21.6 |
| E4b | `step` inlined too | 22.5 | 22.3 (lost; not kept) |
| probe | Fixed-count decode into slack, no code check (not shippable) | 16.6 | 14.9 |
| E5 | Fixed-count decode into a padded buffer, per-symbol code check | 21.8 | 19.4 (lost: the SLP vectorizer turned the check into `pinsrw` chains and spills) |
| E6 | The code check as one SWAR carry test per chunk | 17.9 | 15.9 |

| E7 | Eight symbols assembled in a register, one 8-byte store each (instead of a byte store per symbol) | 1.000× | 1.015× (paired, against E6: lost, inside the noise; not kept) |
| E8 | Byte-coded tables (all 256 bytes): one fixed 16-byte move into the padding instead of a `memcpy` call of the suffix's length | coded: 1.005× | coded: 0.997× (VM, paired against E6: unchanged) |

The probe showed what the trip-count branch cost: the suffix length changes every key, so
the per-symbol loop's exit mispredicted about once per key (3-5 ns). A second probe (x6)
skips the symbol decode entirely: the walk then costs 6.8 ns per key on the Mac and 7.9 on
the VM, so of the final 17.6 ns (kv), about 7 ns is entry bookkeeping (header, bounds
checks, positions) and about 10 ns is the decode, one table lookup and byte store per
symbol.

E8 on byte-coded tables (`TERMDICT_BENCH_BYTES=1`), paired against E6: terms 0.52× on the
Mac and 0.60× on the VM (16.8 to 10.0 ns per key); kv unchanged (its suffixes, about 9
bytes, already took one short `memcpy`).

## Results

Paired per-trial ratios (the branch's time over `main`'s in the same trial, median and
range), seven trials of five repetitions, interleaved with the order alternating; the
A/A pair is `main` built twice. Light holds; other agents' single-thread benchmarks shared
the host, which the pairing absorbs. Seek and 10 on the branch is compared with `main`'s
`reader::scan(from)` and 10 more keys, the way `main` positions.

| | Mac kv | Mac terms | VM kv | VM terms |
|---|---|---|---|---|
| A/A, walk | 0.989 [0.896-1.120] | 1.073 [0.974-1.117] | 1.022 [0.980-1.083] | 1.023 [0.989-1.085] |
| Walk, ns per key (main to branch) | 29.2 to 17.6 | 36.9 to 15.6 | 30.8 to 19.7 | 37.2 to 17.3 |
| Walk, paired | 0.616 [0.547-0.662]: 1.62× faster | 0.419 [0.371-0.462]: 2.39× faster | 0.628 [0.603-0.708]: 1.59× faster | 0.464 [0.433-0.479]: 2.16× faster |
| Seek and 10, ns (scan on main to seek) | 2,019 to 895 | 2,512 to 739 | 2,079 to 1,024 | 2,400 to 855 |
| Seek and 10, paired | 0.451: 2.2× faster | 0.294: 3.4× faster | 0.507: 2.0× faster | 0.374: 2.7× faster |
| `scan(from)` and 10 (now on seek) | 0.480: 2.1× faster | 0.369: 2.7× faster | 0.538: 1.9× faster | 0.410: 2.4× faster |
| `find`, present | 0.969 (noise) | 0.973 (noise) | 1.007 (noise) | 0.946 (noise) |
| `find`, absent | 0.949 (noise) | 0.975 (noise) | 1.029 (noise) | 1.043 (noise) |

The about-15 ns target: terms meet it on the Mac (15.6) and come within 15% on the VM
(17.3); kv is at 17.6 and 19.7.

A seek is mostly memory at 1,000,000 keys: `fst::floor_value` and the block's first line
miss the caches (see the profile), so most of the remaining seek is the index lookup.

## The four axes

- Time: above. Builds and `find` do not change (the builder is untouched; `find` is
  within the A/A on both platforms).
- Memory: `sizeof(reader::cursor)` stays 208 bytes and `block` 64. The cursor's key buffer
  is now always on the heap, the longest key so far plus 16 bytes of padding, where
  libc++'s `std::string` held keys of up to 22 bytes inline: one allocation of about 32
  bytes per open cursor on strata's keys.
- Disk: bytes identical (the golden fixture, and the same bytes per key in every run:
  10.456 kv, 11.043 terms).
- Indexing: the builder is unchanged, so build throughput is unchanged.

## Is a format change worth it?

No. The decode left is a table lookup per symbol; the format already allows byte-coded
suffixes (an empty alphabet), which turn the decode into a copy. Measured with E8: kv
walks in 10.8 ns per key byte-coded against 19.6 coded on the VM, but its tables grow 46%
(15.27 against 10.46 bytes per key); terms grow 8% (11.93 against 11.04) and walk in 10.0
against 17.2 ns. That is a writer's choice today, per dictionary, not a format change.

## What it saves

Per key scanned, 11-12 ns (kv) and 20-21 ns (terms); per seek and 10, 1.1 µs (kv) and
1.6-1.8 µs (terms). A host merging or scanning 10^9 keys a day saves 12-21 CPU-seconds a
day, which is negligible; one serving 10^5 short scans a second (YCSB E's 1-100 records,
50 on average) saves about 50 × 12 ns + 1.1 µs = 1.7 µs per scan, 0.17 of a core per host
(an estimate from these per-unit numbers, not a measured end-to-end run). For strata's
format 14 this was the remaining split-against-inline gap at 100-byte values: termdict's
walk was 32-36 ns per key against inline's 23-26 ns per whole record; it is now 16-20.

## Correctness

The tests (std::map comparisons; cursor seeks from wherever the last walk left off; seeks
to every key and beside it, across every block and restart boundary, in four key shapes,
coded and byte-coded; an out-of-alphabet code at every position and width from 1 to 8
beside the largest valid codes, through `reader::cursor`, `seek`, and `block::cursor`),
the same under ASan and UBSan, and the fuzz target extended to seeks (556,314 runs in 15
minutes, then 193,723 after E8, no findings). Four mutations (the SWAR check off, the check
narrowed, the run walk's skip rule, the next-run index) each fail the tests. strata's suite
(19/19, and its format-14 prototypes, which use `block`'s public pieces, build) and
prospect's (36/36) pass against the branch. CI: Linux x86_64 GCC and Clang, arm64, macOS
arm64 and x86_64, FreeBSD, ASan and UBSan, TSan.

