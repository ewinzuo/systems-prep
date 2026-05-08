#pragma once
#include <atomic>
#include <cstddef>
#include <memory>

namespace ip {

template <typename T, typename Alloc = std::allocator<T>> class SpscQueue {
  using allocator_traits = std::allocator_traits<Alloc>;
  using value_type = T;
  using size_type = typename allocator_traits::size_type;

public:
  explicit SpscQueue(size_type capacity, const Alloc &alloc = Alloc{})
      : alloc_(alloc), capacity_(capacity),
        items_(allocator_traits::allocate(alloc_, capacity)), head_(0),
        tail_(0) {}
  SpscQueue(const SpscQueue &) = delete;
  SpscQueue &operator=(const SpscQueue &) = delete;
  SpscQueue(SpscQueue &&) = delete;
  SpscQueue &operator=(SpscQueue &&) = delete;
  ~SpscQueue() {
    auto tail = tail_.load(std::memory_order_relaxed);
    auto head = head_.load(std::memory_order_relaxed);
    while (tail != head) {
      items_[tail % capacity_].~T();
      ++tail;
    }
    allocator_traits::deallocate(alloc_, items_, capacity_);
  }
  bool push(T const &value) {
    auto head = head_.load(std::memory_order_relaxed);
    auto tail = tail_.load(std::memory_order_acquire);
    if (full(head, tail)) {
      return false;
    }
    new (element(head)) T(value);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }
  bool pop(T &value) {
    auto tail = tail_.load(std::memory_order_relaxed);
    auto head = head_.load(std::memory_order_acquire);
    if (empty(head, tail)) {
      return false;
    }
    value = *element(tail);
    element(tail)->~T();
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }
  size_type size() const noexcept {
    auto h = head_.load(std::memory_order_relaxed);
    auto t = tail_.load(std::memory_order_relaxed);
    return h - t;
  }
  bool empty() const noexcept { return size() == 0; }
  bool full() const noexcept { return size() == capacity_; }
  size_type capacity() const noexcept { return capacity_; }

private:
  bool empty(size_type head, size_type tail) const { return head == tail; }
  T *element(size_type idx) { return &items_[idx % capacity_]; }
  bool full(size_type head, size_type tail) const {
    return head - tail == capacity_;
  }
  [[no_unique_address]] Alloc alloc_;
  size_type capacity_;
  T *items_;
  alignas(64) std::atomic<size_type> head_;
  alignas(64) std::atomic<size_type> tail_;
  char padding_[64 - sizeof(size_type)];
};

} // namespace ip
