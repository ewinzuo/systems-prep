# Day 2 — MPMC Bounded Queue (C++)

Multi-producer / multi-consumer bounded queue, Vyukov-style: each slot has a
sequence number that producers and consumers CAS against. No mutexes, no
unbounded backoff, FIFO ordering.

## Design (Vyukov bounded MPMC)
Each slot holds:
```
struct Cell { atomic<size_t> seq; T data; };
```

Invariant: a slot is *ready to enqueue into* when `seq == pos`, and *ready to
dequeue from* when `seq == pos + 1`.

Enqueue at position `pos = enqueue_pos.load()`:
1. Load `cell.seq`.
2. If `seq - pos == 0`: try `enqueue_pos.compare_exchange_weak(pos, pos+1)`.
   If it wins, store data, `cell.seq.store(pos+1, release)`. Done.
3. If `seq - pos < 0`: queue full. Return false.
4. Else: another producer beat us; reload `pos` and retry.

Dequeue is symmetric with `seq == pos + 1` and ends with
`cell.seq.store(pos + capacity, release)`.

## Why not just SPSC + locks?
- Locks serialize *every* producer through one critical section even when
  they're working on disjoint slots. Vyukov lets disjoint producers proceed in
  parallel.
- Real systems have many producers (NIC RX queues, GPU streams). MPMC is the
  shape NCCL/Neuron actually use.

## Edge cases this design handles
1. **ABA** — `seq` numbers monotonically increase by `capacity` each round, so
   a producer that sees `seq == pos` knows the slot is in *this* generation,
   not a stale earlier one.
2. **Spurious CAS failures** — the loop on `compare_exchange_weak` retries.
3. **Lost producer mid-publish** — if a producer wins the CAS but dies before
   storing data, the slot's `seq` never advances and consumers see the slot
   as still-being-written. This is a liveness hazard — fine for in-process,
   bad for cross-process. Mitigation noted in README.
4. **False sharing** — `enqueue_pos` and `dequeue_pos` on separate cache lines.
5. **Queue full / empty** — explicit return values; no overwriting, no blocking.
6. **Power-of-2 capacity** — same reasons as Day 1.

## API sketch
```cpp
template <class T>
class MpmcQueue {
public:
    explicit MpmcQueue(std::size_t capacity);   // throws if !pow2 or 0
    bool try_push(const T& v);
    bool try_push(T&& v);
    bool try_pop (T& out);
    std::size_t capacity() const noexcept;
    std::size_t size_approx() const noexcept;   // racy
};
```

## Tests
- Single-thread enqueue/dequeue / fill / over-capacity rejection.
- 4 producers × 4 consumers × 1M items each: every produced item appears
  exactly once at some consumer. Order is FIFO *within a producer*, not across.
- TSan / ASan clean.

## Likely interview questions
1. Why a per-slot sequence number? What problem does it solve that a single
   atomic head/tail doesn't?
2. Walk through the ABA scenario without sequence numbers.
3. Why does Vyukov use `compare_exchange_weak` not `_strong`?
4. How would you make this wait-free? (Hint: you mostly can't for MPMC bounded;
   Kogan-Petrank gets close at high cost.)
5. How would you change to support multiple-element batched enqueue atomically?
6. What if `T` is large or non-trivially-copyable — what changes?
