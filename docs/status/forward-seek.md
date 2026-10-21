# forward-seek ledger

Branch `perf/forward-seek` (worktree `~/code/Kronuz/termdict-wt/forward-seek`), from main a91e9b6.

## State

Code and tests written; not yet compiled (host under a strict hold from another agent).
Needs one review before landing: `cursor::seek` is a decoder path in a shared library.

## Done

- Explorer B's change (1deaa82) cherry-picked as a starting point, its common-prefix gate
  replaced by an exact test: the open block's last key, decoded once per open block on the
  first forward attempt and compared with `from`.
- `on_entry_` guards the fast path (a failed seek leaves a stale key behind).
- Tests: `check_forward` (block-count oracle, all positions x boundary targets, backward,
  random hops against lower_bound); fuzz target gained ascending seeks without next().

## Next

Build, test, ASan+UBSan, mutant, measure (near, fwd, random), docs/perf/forward-seek.md,
push, dispatch CI, remove explorer worktrees.

## Blocked on

Nothing.
