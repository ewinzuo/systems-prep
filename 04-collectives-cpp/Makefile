CXX      ?= c++
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -pthread -I.
LDFLAGS  ?= -pthread

SRCS = transport_inproc.cpp allreduce_ring.cpp allgather_recdouble.cpp
OBJS = $(SRCS:.cpp=.o)
HDRS = collective.hpp transport_inproc.hpp

.PHONY: all test tsan asan clean

all: test_collectives

%.o: %.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test_collectives: test_collectives.cpp $(OBJS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ test_collectives.cpp $(OBJS) $(LDFLAGS)

test: test_collectives
	./test_collectives

tsan:
	$(MAKE) clean
	$(MAKE) test_collectives CXXFLAGS="-std=c++20 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=thread -pthread -I." \
	                         LDFLAGS="-fsanitize=thread -pthread"
	./test_collectives

asan:
	$(MAKE) clean
	$(MAKE) test_collectives CXXFLAGS="-std=c++20 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -pthread -I." \
	                         LDFLAGS="-fsanitize=address,undefined -pthread"
	./test_collectives

clean:
	rm -f *.o test_collectives
