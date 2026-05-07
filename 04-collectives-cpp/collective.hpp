#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace ip::collective {

// Transport: send a byte buffer to one rank, recv a byte buffer from another.
// Implementations: in-process MPMC queue grid (Day 4), DMA-ring (Day 5).
class Transport {
public:
    virtual ~Transport() = default;
    // Both calls block until the data has been transferred; len is exact.
    virtual void send(int dst_rank, const void* buf, std::size_t len) = 0;
    virtual void recv(int src_rank,       void* buf, std::size_t len) = 0;
};

struct RankInfo {
    int        rank;
    int        world_size;
    Transport* xport;
};

// Reduction op for primitive types.
enum class Op { kSum, kMax, kMin };

// In-place all-reduce on a float buffer. Splits into chunks of count/world_size
// and runs a ring algorithm. count must currently be divisible by world_size;
// remainder handling is a TODO.
void allreduce_ring(const RankInfo& info,
                    float* data,
                    std::size_t count,
                    Op op);

// Recursive-doubling all-gather: each rank starts with `chunk` floats at
// offset rank*chunk, ends with the full count = chunk * world_size buffer.
// world_size must be a power of two.
void allgather_recdouble(const RankInfo& info,
                         float* data,
                         std::size_t chunk);

}  // namespace ip::collective
