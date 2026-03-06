CC      = cc
CFLAGS  = -O2 -march=native -Wall -Wextra -std=c11
CORE    = core/residc.c
INCLUDE = -Icore

.PHONY: all clean examples bench test

all: examples

examples: examples/custom/quote_example

bench: bench/bench_quote
	./bench/bench_quote

examples/custom/quote_example: examples/custom/quote_example.c $(CORE) core/residc.h
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ examples/custom/quote_example.c $(CORE)

bench/bench_quote: bench/bench_quote.c $(CORE) core/residc.h
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ bench/bench_quote.c $(CORE)

test:
	@bash test/run_tests.sh

clean:
	rm -f examples/custom/quote_example bench/bench_quote test/test_bitio test/test_zigzag test/test_tiered test/test_mfu test/test_adaptive test/test_roundtrip test/test_edge test/test_errors
