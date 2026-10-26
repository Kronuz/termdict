# validate-at-open ledger

Branch `fix/validate-at-open` (worktree `~/code/Kronuz/termdict-wt/validate-at-open`), from main 840d7b3.

## State

In progress.

## Finding that shapes the work

The premise "open still trusts the FST" does not hold at the pinned fst (24da59a): `fst::map(bytes)`
validates every node in one linear pass (header, node bounds, label order, widths, target = an
earlier node start, root = last node, key count), and `termdict::reader` uses that constructor. What
was missing:

1. A malformed FST threw `std::invalid_argument`, not `termdict::corrupt`.
2. No test or fuzz target mutated the index bytes (the fuzz target only junked block bytes).

## Done

(filled as criteria close)

## Next

## Blocked on

Nothing.
