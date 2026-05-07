CXX      ?= c++
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -pthread
LDFLAGS  ?= -pthread

.PHONY: all test tsan asan clean

all: test_mpmc

test_mpmc: test_mpmc.cpp mpmc_queue.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_mpmc.cpp $(LDFLAGS)

test: test_mpmc
	./test_mpmc

tsan:
	$(MAKE) clean
	$(MAKE) test_mpmc CXXFLAGS="-std=c++20 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=thread -pthread" \
	                  LDFLAGS="-fsanitize=thread -pthread"
	./test_mpmc

asan:
	$(MAKE) clean
	$(MAKE) test_mpmc CXXFLAGS="-std=c++20 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -pthread" \
	                  LDFLAGS="-fsanitize=address,undefined -pthread"
	./test_mpmc

clean:
	rm -f test_mpmc bench_mpmc *.o
