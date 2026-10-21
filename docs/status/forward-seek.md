# forward-seek ledger

Branch `perf/forward-seek` (worktree `~/code/Kronuz/termdict-wt/forward-seek`), from main a91e9b6.

## State

Implemented, tested, measured; pushed, CI dispatched on the branch. Needs one review before landing:
`cursor::seek` is a decoder path in a shared library.

## Done

- Explorer B's change (1deaa82) with its heuristic replaced by a last-key test; `block::last_key`
  decodes only the symbols the key keeps; decoded on a block's second forward attempt.
- `check_forward`, corruption-sweep forward seeks, fuzz forward seeks; four mutants caught;
  ASan+UBSan clean; 60 s fuzz clean. Numbers in docs/perf/forward-seek.md.

## Next

Review, then land. Optional: store the last key in the block (format change).

## Blocked on

Nothing.
