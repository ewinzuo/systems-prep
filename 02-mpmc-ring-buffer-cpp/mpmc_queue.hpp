#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ip {

inline constexpr std::size_t kCacheLine = 64;

// Vyukov-style bounded MPMC queue.
template <class T>
class MpmcQueue {
public:
    explicit MpmcQueue(std::size_t capacity)
        : capacity_(capacity), mask_(capacity - 1) {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0)
            throw std::invalid_argument("capacity must be a power of two and > 0");
        cells_ = new Cell[capacity];
        for (std::size_t i = 0; i < capacity; ++i)
            cells_[i].seq.store(i, std::memory_order_relaxed);
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    ~MpmcQueue() { delete[] cells_; }

    MpmcQueue(const MpmcQueue&) = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    bool try_push(T v) {
        Cell* cell;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            std::size_t seq = cell->seq.load(std::memory_order_acquire);
            std::intptr_t diff = (std::intptr_t)seq - (std::intptr_t)pos;
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false;  // full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = std::move(v);
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        Cell* cell;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            std::size_t seq = cell->seq.load(std::memory_order_acquire);
            std::intptr_t diff = (std::intptr_t)seq - (std::intptr_t)(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false;  // empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        out = std::move(cell->data);
        cell->seq.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t size_approx() const noexcept {
        auto h = enqueue_pos_.load(std::memory_order_relaxed);
        auto t = dequeue_pos_.load(std::memory_order_relaxed);
        return h - t;
    }

private:
    struct Cell {
        std::atomic<std::size_t> seq;
        T                        data;
    };

    alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_;
    alignas(kCacheLine) std::atomic<std::size_t> dequeue_pos_;

    std::size_t capacity_;
    std::size_t mask_;
    Cell*       cells_;
};

}  // namespace ip
