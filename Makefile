CC      = cc
CFLAGS  = -O2 -march=native -Wall -Wextra -std=c11
CORE    = core/residc.c
INCLUDE = -Icore

.PHONY: all clean examples bench

all: examples

examples: examples/custom/quote_example

bench: bench/bench_quote
	./bench/bench_quote

examples/custom/quote_example: examples/custom/quote_example.c $(CORE) core/residc.h
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ examples/custom/quote_example.c $(CORE)

bench/bench_quote: bench/bench_quote.c $(CORE) core/residc.h
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ bench/bench_quote.c $(CORE)

clean:
	rm -f examples/custom/quote_example bench/bench_quote
