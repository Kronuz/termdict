# Seek and sequential decode

Status: in progress (branch `perf/seek-decode`).

## Ledger

- State: decode E1-E6 and `cursor::seek` built; tests, seek tests, corrupt-code tests,
  ASan and UBSan green on the Mac; strata (19/19) and prospect (36/36) pass against the
  branch; fuzzing; final A/Bs on the Mac and the VM next.
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

The probe showed what the trip-count branch cost: the suffix length changes every key, so
the per-symbol loop's exit mispredicted about once per key (3-5 ns).
