// Day 5: stub. Real implementation depends on Day 3 nic_t being functional.

#include "transport_dma.hpp"

namespace ip::collective {

NicGrid::NicGrid(int n_) : n(n_), nics(n_*n_, nullptr) {
    // TODO: nic_open(...) for each pair.
}
NicGrid::~NicGrid() {
    // TODO: nic_close each.
}

DmaTransport::DmaTransport(int world_size, int my_rank,
                           std::shared_ptr<NicGrid> grid)
    : world_size_(world_size), my_rank_(my_rank), nic_grid_(std::move(grid)) {}

void DmaTransport::send(int dst_rank, const void* buf, std::size_t len) {
    (void)dst_rank; (void)buf; (void)len;
    // TODO: nic_submit + nic_doorbell on nic_grid_->nics[my_rank_*n + dst_rank]
}
void DmaTransport::recv(int src_rank, void* buf, std::size_t len) {
    (void)src_rank; (void)buf; (void)len;
    // TODO: nic_drain on nic_grid_->nics[src_rank*n + my_rank_], match cookie
}

}  // namespace ip::collective
