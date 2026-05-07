#include "transport_inproc.hpp"

#include <stdexcept>

namespace ip::collective {

InProcGrid::InProcGrid(int n_) : n(n_), mu(n_*n_), cv(n_*n_), q(n_*n_) {}

InProcTransport::InProcTransport(int world_size, int my_rank,
                                 std::shared_ptr<InProcGrid> grid)
    : world_size_(world_size), my_rank_(my_rank), grid_(std::move(grid)) {}

void InProcTransport::send(int dst_rank, const void* buf, std::size_t len) {
    if (dst_rank < 0 || dst_rank >= world_size_)
        throw std::out_of_range("send dst");
    int idx = my_rank_ * world_size_ + dst_rank;
    std::vector<std::byte> bytes(len);
    std::memcpy(bytes.data(), buf, len);
    {
        std::lock_guard<std::mutex> lk(grid_->mu[idx]);
        grid_->q[idx].push(std::move(bytes));
    }
    grid_->cv[idx].notify_one();
}

void InProcTransport::recv(int src_rank, void* buf, std::size_t len) {
    if (src_rank < 0 || src_rank >= world_size_)
        throw std::out_of_range("recv src");
    int idx = src_rank * world_size_ + my_rank_;
    std::unique_lock<std::mutex> lk(grid_->mu[idx]);
    grid_->cv[idx].wait(lk, [&] { return !grid_->q[idx].empty(); });
    auto bytes = std::move(grid_->q[idx].front());
    grid_->q[idx].pop();
    lk.unlock();
    if (bytes.size() != len) throw std::runtime_error("recv size mismatch");
    std::memcpy(buf, bytes.data(), len);
}

}  // namespace ip::collective
