CXX      ?= c++
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -pthread \
            -I. -I../03-userspace-driver-dma-c -I../04-collectives-cpp
LDFLAGS  ?= -pthread

# Pull in already-built objects from the prior days.
DEP_OBJS = ../03-userspace-driver-dma-c/nic.o \
           ../04-collectives-cpp/transport_inproc.o \
           ../04-collectives-cpp/allreduce_ring.o \
           ../04-collectives-cpp/allgather_recdouble.o

SRCS = transport_dma.cpp broadcast_tree.cpp reduce_scatter.cpp
OBJS = $(SRCS:.cpp=.o)
HDRS = transport_dma.hpp

.PHONY: all test bench clean deps

deps:
	$(MAKE) -C ../03-userspace-driver-dma-c
	$(MAKE) -C ../04-collectives-cpp

all: deps test_integration bench_collectives

%.o: %.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test_integration: test_integration.cpp $(OBJS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ test_integration.cpp $(OBJS) $(DEP_OBJS) $(LDFLAGS)

bench_collectives: bench_collectives.cpp $(OBJS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ bench_collectives.cpp $(OBJS) $(DEP_OBJS) $(LDFLAGS)

test: test_integration
	./test_integration

bench: bench_collectives
	./bench_collectives

clean:
	rm -f *.o test_integration bench_collectives
