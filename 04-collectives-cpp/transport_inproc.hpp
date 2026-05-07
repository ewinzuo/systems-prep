#pragma once

#include "collective.hpp"

#include <cstring>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>

namespace ip::collective {

// In-process transport: an N×N grid of byte queues. send(dst) appends to
// queues[my_rank][dst]; recv(src) blocks on queues[src][my_rank].
//
// TODO (Day 4):
//   replace the std::queue<std::vector<std::byte>> with the MPMC queue from
//   Day 2 once it's a fixed-element type. For now, std::mutex + condvar is
//   simple and fine for correctness tests.
class InProcTransport : public Transport {
public:
    InProcTransport(int world_size, int my_rank,
                    std::shared_ptr<struct InProcGrid> grid);
    void send(int dst_rank, const void* buf, std::size_t len) override;
    void recv(int src_rank,       void* buf, std::size_t len) override;

private:
    int                              world_size_;
    int                              my_rank_;
    std::shared_ptr<struct InProcGrid> grid_;
};

struct InProcGrid {
    explicit InProcGrid(int n);
    int n;
    std::vector<std::mutex>              mu;
    std::vector<std::condition_variable> cv;
    std::vector<std::queue<std::vector<std::byte>>> q;  // q[src*n + dst]
};

}  // namespace ip::collective
