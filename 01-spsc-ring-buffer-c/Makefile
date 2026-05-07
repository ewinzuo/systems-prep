CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror -pthread
LDFLAGS ?= -pthread

OBJS = spsc_ring.o
HDRS = spsc_ring.h

.PHONY: all test bench tsan asan clean

all: test_spsc bench_spsc

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

libspsc.a: $(OBJS)
	$(AR) rcs $@ $^

test_spsc: test_spsc.o libspsc.a
	$(CC) $(CFLAGS) -o $@ test_spsc.o libspsc.a $(LDFLAGS)

bench_spsc: bench_spsc.o libspsc.a
	$(CC) $(CFLAGS) -o $@ bench_spsc.o libspsc.a $(LDFLAGS)

test: test_spsc
	./test_spsc

bench: bench_spsc
	./bench_spsc

tsan:
	$(MAKE) clean
	$(MAKE) test_spsc CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=thread -pthread" \
	                  LDFLAGS="-fsanitize=thread -pthread"
	./test_spsc

asan:
	$(MAKE) clean
	$(MAKE) test_spsc CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -pthread" \
	                  LDFLAGS="-fsanitize=address,undefined -pthread"
	./test_spsc

clean:
	rm -f *.o *.a test_spsc bench_spsc
