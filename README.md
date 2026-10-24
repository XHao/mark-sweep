# mark-sweep — a GC teaching playground

Sample code for [Baby's First Garbage Collector][article], grown into a
five-stage tour of how real collectors work. Each `gcN.c` is one
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

Build and test everything (three expected-fail binaries prove the
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

The killer tests (`test7` in gc2, `test5` in gc4) are the ones whose
failure the barriers exist to prevent.
