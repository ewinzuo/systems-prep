// Day 4 implementation goes here.
//
// Plan: for log2(N) steps, exchange with rank XOR (1<<step) and concatenate
// the received chunk into the right position. World size must be pow2.

#include "collective.hpp"

namespace ip::collective {

void allgather_recdouble(const RankInfo& info,
                         float* data,
                         std::size_t chunk) {
    (void)info; (void)data; (void)chunk;
    // TODO
}

}  // namespace ip::collective
