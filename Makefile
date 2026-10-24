CC      ?= cc
CFLAGS  = -ggdb -std=gnu99 -Wall
SRCS    = $(wildcard gc*.c)
BINS    = $(SRCS:.c=)

all: $(BINS)

markandsweep: main.c
	$(CC) $(CFLAGS) main.c -o $@

gc%: gc%.c
	$(CC) $(CFLAGS) $< -o $@

test: all gc2_bo
	@ok=0; fail=0; \
	for b in $(BINS); do \
	  if ./$$b > /dev/null 2>&1; then echo "PASS $$b"; ok=$$((ok+1)); \
	  else echo "FAIL $$b"; fail=$$((fail+1)); fi; \
	done; \
	for b in gc2_bo; do \
	  if ./$$b > /dev/null 2>&1; then echo "UNEXPECTED-PASS $$b"; fail=$$((fail+1)); \
	  else echo "EXPECTED-FAIL $$b"; ok=$$((ok+1)); fi; \
	done; \
	echo "$$ok passed, $$fail failed"; \
	test $$fail -eq 0

gc2_bo: gc2_incremental.c
	$(CC) $(CFLAGS) -DBARRIER_OFF gc2_incremental.c -o $@

run: markandsweep
	valgrind --leak-check=yes ./markandsweep

.PHONY: all test run clean
clean:
	rm -f $(BINS) markandsweep gc2_bo *~
