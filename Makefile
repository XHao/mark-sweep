CC      ?= cc
CFLAGS  = -ggdb -std=gnu99 -Wall
SRCS    = $(wildcard gc*.c)
BINS    = $(SRCS:.c=)

all: $(BINS)

markandsweep: main.c
	$(CC) $(CFLAGS) main.c -o $@

gc%: gc%.c
	$(CC) $(CFLAGS) $< -o $@

test: all gc2_bo gc3_overflow gc4_bo gc6_dijkstra gc6_yuasa gc7_bo gc7_overflow gc8_bo
	@ok=0; fail=0; \
	for b in $(BINS); do \
	  if ./$$b > /dev/null 2>&1; then echo "PASS $$b"; ok=$$((ok+1)); \
	  else echo "FAIL $$b"; fail=$$((fail+1)); fi; \
	done; \
	for b in gc2_bo gc3_overflow gc4_bo gc6_dijkstra gc6_yuasa gc7_bo gc7_overflow gc8_bo; do \
	  if ./$$b > /dev/null 2>&1; then echo "UNEXPECTED-PASS $$b"; fail=$$((fail+1)); \
	  else echo "EXPECTED-FAIL $$b"; ok=$$((ok+1)); fi; \
	done; \
	echo "$$ok passed, $$fail failed"; \
	test $$fail -eq 0

gc3_overflow: gc3_copying.c
	$(CC) $(CFLAGS) -DOVERFLOW_TEST gc3_copying.c -o $@

gc7_overflow: gc7_regions.c
	$(CC) $(CFLAGS) -DEXHAUST_TEST gc7_regions.c -o $@

gc8_bo: gc8_zgc.c
	$(CC) $(CFLAGS) -DLOAD_BARRIER_OFF gc8_zgc.c -o $@

gc7_bo: gc7_regions.c
	$(CC) $(CFLAGS) -DRSET_OFF gc7_regions.c -o $@

gc6_yuasa: gc6_hybrid.c
	$(CC) $(CFLAGS) -DBARRIER_YUASA gc6_hybrid.c -o $@

gc6_dijkstra: gc6_hybrid.c
	$(CC) $(CFLAGS) -DBARRIER_DIJKSTRA gc6_hybrid.c -o $@

gc4_bo: gc4_generational.c
	$(CC) $(CFLAGS) -DREMSET_OFF gc4_generational.c -o $@

gc2_bo: gc2_incremental.c
	$(CC) $(CFLAGS) -DBARRIER_OFF gc2_incremental.c -o $@

# Institutionalize the sanitizer pass the reviews ran manually:
# build every stage with ASan+UBSan and run it.
asan:
	@ok=0; fail=0; \
	for s in $(SRCS); do \
	  b=$$(basename $$s .c); \
	  if $(CC) -ggdb -std=gnu99 -Wall -fsanitize=address,undefined $$s -o /tmp/$$b.asan && /tmp/$$b.asan > /dev/null 2>&1; then echo "ASAN PASS $$b"; ok=$$((ok+1)); \
	  else echo "ASAN FAIL $$b"; fail=$$((fail+1)); fi; \
	done; \
	echo "$$ok passed, $$fail failed"; \
	test $$fail -eq 0

run: markandsweep
	valgrind --leak-check=yes ./markandsweep

.PHONY: all test run asan clean
clean:
	rm -f $(BINS) markandsweep gc2_bo gc3_overflow gc4_bo gc6_dijkstra gc6_yuasa gc7_bo gc7_overflow gc8_bo *~
