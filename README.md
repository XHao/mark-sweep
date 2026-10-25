# mark-sweep — a GC teaching playground

Sample code for [Baby's First Garbage Collector][article], grown into an
eight-stage tour. For a guided walk through the algorithms themselves —
what problem each stage inherits, how the invariant lands in code, and
which killer test disproves which shortcut — read
[docs/algorithms.md](docs/algorithms.md) of how real collectors work. Each `gcN.c` is one
self-contained, runnable collector; read them in order and diff each
against its predecessor — the diff *is* the lesson.

[article]: http://journal.stuffwithstuff.com/2013/12/08/babys-first-garbage-collector/

| Stage | Concept introduced | Where it shows up in production |
|---|---|---|
| `main.c` / `gc0_baseline.c` | mark-sweep, root set, adaptive threshold | the classic baseline |
| `gc1_iterative.c` | explicit worklist instead of recursion | every production collector |
| `gc2_incremental.c` | tricolor abstraction, incremental marking, Yuasa write barrier, mutator interleaving | Go's GC, JVM concurrent markers |
| `gc3_copying.c` | Cheney semispace copy, bump-pointer allocation, forwarding pointers, root rewriting | JVM young gen, every moving GC |
| `gc4_generational.c` | weak generational hypothesis, promotion, remembered set, minor/major split | JVM G1, .NET's generational GC |
| `gc4_survivor.c` | age thresholds, eden/survivor space swap, promote on AGE_MAX — gc4's promotion bug fixed | HotSpot young gen (survivor spaces + tenuring) |
| `gc5_compact.c` | Lisp2 mark-compact: three passes, sliding compaction, dedicated forward word, 100% heap usable | JVM full-GC compaction, .NET's compacting GC |
| `gc6_hybrid.c` | Go-style hybrid write barrier (insertion + deletion halves), unbarriered root writes, simulated concurrent marking | Go 1.8+ runtime write barrier |

Build and test everything (five expected-fail binaries prove the
barriers actually matter — compile them out and the killer tests
catch the loss):

    make test

Suggested reading order: each file's header comment, then its diff
against the stage it was built from, then the tests at the bottom.
gc3 and gc4 are built from **gc0**, not their numeric predecessor —
copying collection and generational promotion discard mark/sweep
wholesale, so diffing them against gc2 would be noise:

    diff gc0_baseline.c   gc1_iterative.c    # iterative marking
    diff gc1_iterative.c  gc2_incremental.c  # tricolor + write barrier
    diff gc0_baseline.c   gc3_copying.c      # semispace copy (no mark bits)
    diff gc0_baseline.c   gc4_generational.c # gc3's nursery + gc0's old gen
    diff gc4_generational.c gc4_survivor.c  # aging: survivor spaces + AGE_MAX
    diff gc3_copying.c   gc5_compact.c      # mark-compact (single space, in place)
    diff gc2_incremental.c gc6_hybrid.c     # hybrid barrier + pushRoot

The killer tests (`test7` in gc2, `test5` in gc4, `test8` and
`test9` in gc6) are the ones whose failure the barriers exist to
prevent — each barrier half has its own killer test and its own
expected-fail build that removes it.
