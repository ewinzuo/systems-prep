#pragma once
#include <atomic>
#include <cstddef>
#include <memory>

namespace ip {

template <typename T> struct Cell {
  T data_;
  std::atomic<bool> is_ready{false};
};

template <typename T, typename Alloc = std::allocator<Cell<T>>>
class MpscQueue {
  using allocator_traits = std::allocator_traits<Alloc>;
  using value_type = T;
  using size_type = typename allocator_traits::size_type;

public:
  explicit MpscQueue(size_type capacity, const Alloc &alloc = Alloc{})
      : alloc_(alloc), capacity_(capacity),
        items_(allocator_traits::allocate(alloc_, capacity)), head_(0),
        tail_(0) {
    for (size_type i = 0; i < capacity_; ++i) {
      allocator_traits::construct(alloc_, &items_[i]);
    }
  }

  MpscQueue(const MpscQueue &) = delete;
  MpscQueue &operator=(const MpscQueue &) = delete;
  MpscQueue(MpscQueue &&) = delete;
  MpscQueue &operator=(MpscQueue &&) = delete;

  ~MpscQueue() {
    for (size_type i = 0; i < capacity_; ++i) {
      items_[i].~Cell();
    }
    allocator_traits::deallocate(alloc_, items_, capacity_);
  }

  bool push(T const &value) {
    /* CAS-loop claim: only advance head if there's room. fetch_add would
     * irreversibly inflate head on a full queue. */
    auto head = head_.load(std::memory_order_relaxed);
    for (;;) {
      auto tail = tail_.load(std::memory_order_acquire);
      if (full(head, tail))
        return false;
      if (head_.compare_exchange_weak(head, head + 1, std::memory_order_relaxed,
                                      std::memory_order_relaxed))
        break;
      /* CAS failed: another producer claimed `head`. CAS updated `head` with
       * the new value; loop and retry. */
    }
    auto &cell = element(head);
    cell.data_ = value;
    cell.is_ready.store(true, std::memory_order_release);
    return true;
  }

  bool pop(T &value) {
    /* Single consumer: tail is consumer-owned, no atomic claim needed. */
    auto tail = tail_.load(std::memory_order_relaxed);
    auto head = head_.load(std::memory_order_acquire);
    if (empty(head, tail))
      return false;
    auto &cell = element(tail);
    if (!cell.is_ready.load(std::memory_order_acquire))
      return false; /* slot claimed but producer hasn't finished writing */
    value = std::move(cell.data_);
    cell.is_ready.store(false, std::memory_order_relaxed);
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
  Cell<T> &element(size_type idx) { return items_[idx % capacity_]; }
  bool full(size_type head, size_type tail) const {
    return head - tail >= capacity_;
  }

private:
  [[no_unique_address]] Alloc alloc_;
  size_type capacity_;
  Cell<T> *items_;
  alignas(64) std::atomic<size_type> head_;
  alignas(64) std::atomic<size_type> tail_;
  char padding_[64 - sizeof(size_type)];
};

} // namespace ip
