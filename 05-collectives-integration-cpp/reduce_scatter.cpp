// Day 5: standalone reduce-scatter (the first half of ring all-reduce). Stub.

#include "collective.hpp"

namespace ip::collective {

void reduce_scatter_ring(const RankInfo& info, float* data,
                         std::size_t count, Op op) {
    (void)info; (void)data; (void)count; (void)op;
    // TODO
}

}  // namespace ip::collective
