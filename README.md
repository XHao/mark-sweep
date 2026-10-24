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
| `gc4_generational.c` | weak generational hypothesis, promotion, remembered set, minor/major split | JVM G1, Go 1.19+, Mimalloc |

Build and test everything (three expected-fail binaries prove the
barriers actually matter — compile them out and the killer tests
catch the loss):

    make test

Suggested reading order: each file's header comment, then the diff
against the previous stage, then the tests at the bottom — the killer
tests (`test7` in gc2, `test5` in gc4) are the ones whose failure the
barriers exist to prevent.
