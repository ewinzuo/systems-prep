#pragma once

#include "collective.hpp"

#include <cstddef>
#include <memory>
#include <vector>

extern "C" {
#include "nic.h"
}

namespace ip::collective {

// Day 5: a Transport implementation that funnels each send through the
// Day 3 NIC simulator. Each rank pair shares a NIC instance; the SQE's
// cookie carries the (src, dst, seq) tuple so the receiver can match.
//
// The point isn't to be fast — it's to drive the same algorithms (Day 4) over
// a transport that looks like a real device queue, with completions and
// doorbells. Day 4's in-process transport remains the correctness baseline.
class DmaTransport : public Transport {
public:
    DmaTransport(int world_size, int my_rank,
                 std::shared_ptr<struct NicGrid> nic_grid);
    void send(int dst_rank, const void* buf, std::size_t len) override;
    void recv(int src_rank,       void* buf, std::size_t len) override;

private:
    int                          world_size_;
    int                          my_rank_;
    std::shared_ptr<NicGrid>     nic_grid_;
};

struct NicGrid {
    explicit NicGrid(int n);
    ~NicGrid();
    int n;
    std::vector<nic_t*> nics;        // n*n NICs; nics[src*n + dst] handles src->dst
};

}  // namespace ip::collective
