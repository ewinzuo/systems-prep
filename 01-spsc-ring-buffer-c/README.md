# Day 1 — SPSC Lock-Free Ring Buffer (C)

A single-producer / single-consumer bounded ring buffer. No locks, no CAS — just
careful memory ordering.

## Why SPSC first
- Simplest concurrent data structure that still has real teeth.
- Forces you to think about memory ordering without the distraction of CAS / ABA.
- The transport pattern shows up everywhere: NIC RX/TX rings, NPU work queues,
  audio buffers, log shippers, kernel/user IPC.

## API
```c
spsc_ring_t *spsc_ring_create(size_t capacity, size_t elem_size);
void         spsc_ring_destroy(spsc_ring_t *r);

int    spsc_ring_push(spsc_ring_t *r, const void *elem);   // 0 ok, -1 full
int    spsc_ring_pop (spsc_ring_t *r,       void *elem);   // 0 ok, -1 empty
size_t spsc_ring_push_n(spsc_ring_t *r, const void *elems, size_t n);
size_t spsc_ring_pop_n (spsc_ring_t *r,       void *elems, size_t n);

size_t spsc_ring_size    (const spsc_ring_t *r);  // approximate, racy
size_t spsc_ring_capacity(const spsc_ring_t *r);
bool   spsc_ring_empty   (const spsc_ring_t *r);
bool   spsc_ring_full    (const spsc_ring_t *r);
```

## Edge cases — what we explicitly handle

### 1. Over-capacity / push-into-full
- `push` returns `-1` and the buffer is unchanged. Caller decides: drop, retry,
  back-pressure, etc. No silent overwrite (the design is "never lose data";
  if you want a "drop-oldest" overwriting variant, that's a separate type).
- A common bug is testing `head == tail` for both empty and full. We use
  monotonic `uint64_t` counters where:
  - empty ⟺ `head == tail`
  - full  ⟺ `head - tail == capacity`
  This trades one bit of state for not having to "waste a slot" or use a
  separate flag.

### 2. Empty / pop-from-empty
- `pop` returns `-1` and the out-buffer is untouched.

### 3. Capacity must be power-of-2
- We use `head & mask` instead of `head % capacity`. `create()` rejects
  non-power-of-2 capacities with `NULL`. Cheaper, removes a div, and the
  monotonic-counter scheme requires it.

### 4. Counter wraparound (`uint64_t`)
- `head` and `tail` are 64-bit monotonic. At 1 ns per push, wrapping
  takes ~584 years. We don't worry about it. The arithmetic
  `head - tail` works under unsigned wraparound anyway.
- A 32-bit version would wrap in ~4 seconds at 1 GHz pushes — would need
  more care.

### 5. False sharing
- `head` and `tail` live on separate 64-byte cache lines (`alignas(64)`).
  Without this, every producer write invalidates the consumer's cache line and
  vice versa — typically 5–10× slowdown on contended workloads.
- The shared-read-only metadata (capacity, mask, buf pointer, elem_size)
  goes on its own line so neither side's writes touch it.

### 6. Memory ordering
- Producer:
  - `relaxed` load of own `head` (only writer, no synchronization needed).
  - `acquire` load of `tail` (so that writes the consumer signaled with
    its `release` store on `tail` are visible).
  - `memcpy` element into slot.
  - `release` store of new `head` (publishes the slot write to the consumer).
- Consumer is symmetric.
- On x86 (TSO) all loads are acquire and all stores are release, so this
  generates the same code as `relaxed` loads + plain stores. On ARM /
  weak-memory targets the explicit ordering matters and prevents real bugs.

### 7. ABA
- N/A for SPSC — only one writer per index, so no CAS, so no ABA.
- Day 2's MPMC variant has to deal with it.

### 8. Element size & alignment
- `elem_size == 0` rejected.
- We `memcpy` rather than typed assignment. Caller is responsible for
  alignment if they care — typical use is `sizeof(T)` for trivially-copyable T.

### 9. Non-trivial / pointer types
- For C, only trivially-copyable types make sense. If you store pointers
  to heap objects, ownership is the caller's problem (this ring just moves bytes).

### 10. `size()` is approximate
- `head` and `tail` are loaded non-atomically together, so the returned
  size is from "some moment in the recent past". Fine for telemetry,
  not for control flow. Use `push`/`pop` return values for the real answer.

### 11. Shutdown / EOF
- Out of scope for this exercise. In production you'd add a sentinel value,
  a separate atomic flag, or a "poison pill" message.

### 12. Cache-line write amplification
- A naive push reloads `tail` every iteration. The `cached_tail` /
  `cached_head` optimization (each side caches the *other* side's index
  and only reloads on full/empty) cuts contended traffic on the shared
  cache lines by ~10×. Implemented in `spsc_ring.c`.

## Tests covered (`test_spsc.c`)
- Create/destroy: NULL on bad capacity, NULL on bad elem_size.
- Push/pop single element.
- Fill exactly to capacity, next push fails.
- Empty pop fails.
- Wrap-around: push N, pop N, push N — verify all values.
- Batch push/pop: exact, partial-fill, partial-drain.
- Concurrent producer + consumer, 10M items, verify monotonic sequence.
- TSan run: should report no races.

## Bench (`bench_spsc.c`)
- Pin producer and consumer to different cores.
- Measure single-element push/pop throughput (Mops/s).
- Measure batch (n=64) push_n/pop_n throughput.
- Print false-sharing-disabled vs. enabled comparison (compile-time).

## Build
```sh
make           # builds libspsc.a, test_spsc, bench_spsc
make test      # runs test_spsc
make tsan      # rebuilds with -fsanitize=thread and runs test_spsc
make asan      # rebuilds with -fsanitize=address,undefined and runs test_spsc
make bench     # runs bench_spsc
make clean
```

## Likely interview questions
1. Why power-of-2 capacity?
2. How do you tell empty from full?
3. Why monotonic counters instead of masked indices in storage?
4. Walk through the memory orders. What breaks on ARM if I make them all relaxed?
5. Why does the cached_tail optimization help?
6. How would you change this for MPSC? MPMC?
7. What happens if the producer dies mid-push? (Half-written slot — we never
   advance `head`, so the slot is invisible. Good.)
8. How would you add blocking semantics (push waits when full)? Futex / condvar
   on the head/tail values.
