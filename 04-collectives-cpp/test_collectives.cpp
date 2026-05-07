#include "collective.hpp"
#include "transport_inproc.hpp"

#include <iostream>

int main() {
    // TODO Day 4:
    //   - spawn N threads, each running allreduce_ring on its slice
    //   - verify result matches sequential sum (within eps)
    //   - same for allgather_recdouble
    //   - sweep N = {2, 4, 8} and count = {1024, 65536}
    std::cout << "collectives stub\n";
}
