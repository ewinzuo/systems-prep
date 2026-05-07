CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -pthread
LDFLAGS ?= -pthread

OBJS = nic.o
HDRS = nic.h

.PHONY: all test tsan asan clean

all: test_dma

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

test_dma: test_dma.o $(OBJS)
	$(CC) $(CFLAGS) -o $@ test_dma.o $(OBJS) $(LDFLAGS)

test: test_dma
	./test_dma

tsan:
	$(MAKE) clean
	$(MAKE) test_dma CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=thread -pthread" \
	                 LDFLAGS="-fsanitize=thread -pthread"
	./test_dma

asan:
	$(MAKE) clean
	$(MAKE) test_dma CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -pthread" \
	                 LDFLAGS="-fsanitize=address,undefined -pthread"
	./test_dma

clean:
	rm -f *.o test_dma
