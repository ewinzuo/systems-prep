/* Day 1 implementation goes here.
 *
 * Sketch:
 *   struct spsc_ring {
 *       // producer line:  _Atomic uint64_t head;  uint64_t cached_tail;
 *       // consumer line:  _Atomic uint64_t tail;  uint64_t cached_head;
 *       // shared ro line: capacity, mask, elem_size, buf
 *       // each block padded / _Alignas(64) so the producer's writes don't
 *       // invalidate the consumer's cache line.
 *   };
 *
 *   create:
 *     - reject !pow2 capacity, zero elem_size, capacity*elem_size overflow
 *     - aligned_alloc with size rounded up to a multiple of CACHELINE
 *       (macOS aligned_alloc rejects sizes that aren't a multiple of alignment)
 *     - atomic_init head and tail to 0
 *
 *   push:
 *     - relaxed load of own head
 *     - check cached_tail; if appears full, refresh tail with acquire load
 *     - memcpy slot at (head & mask)
 *     - release-store head + 1   (publishes the slot write)
 *
 *   pop is symmetric.
 *
 *   batch (push_n / pop_n):
 *     - one or two memcpys (straddle the wrap boundary)
 *     - one acquire load of the other side's index
 *     - one release-store at the end (amortizes — this is the big win)
 *
 *   size / empty / full are racy snapshots, fine for telemetry.
 *
 * Edge cases the tests should cover (from README):
 *   create() rejects bad args; fill-to-capacity then push fails;
 *   pop on empty fails; wrap-around correctness across many laps;
 *   batch over-capacity returns partial; concurrent producer/consumer
 *   sees a strictly monotonic sequence; TSan and ASan are clean.
 *
 * Memory ordering reminders:
 *   - producer is the *only* writer of head -> relaxed self-loads are fine
 *   - tail load must be acquire (sees consumer's prior writes)
 *   - head store must be release (publishes slot write)
 *   - on x86 these compile the same as relaxed; on ARM the orderings matter
 */

#include "spsc_ring.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct spsc_ring {
  /* shared read-only metadata: own cache line (not written after create). */
  size_t capacity;
  size_t mask;          /* capacity - 1; capacity must be power of two */
  size_t element_size;
  char  *buf;

  /* producer writes head, consumer acquire-loads it. */
  _Alignas(64) _Atomic uint64_t head;

  /* cached_tail: producer-only state on its own cache line. Producer writes
   * (refresh) and reads (check) freely; consumer never touches it. Separate
   * line from head so producer's refresh writes don't invalidate the head
   * line in the consumer's cache. */
  _Alignas(64) uint64_t cached_tail;

  /* consumer writes tail, producer acquire-loads it. */
  _Alignas(64) _Atomic uint64_t tail;

  /* cached_head: consumer-only state on its own cache line. */
  _Alignas(64) uint64_t cached_head;
};
spsc_ring_t *spsc_ring_create(size_t capacity, size_t elem_size) {
  /* Require power-of-two capacity so we can replace `idx % capacity` with
   * the much cheaper `idx & (capacity - 1)`. */
  if (capacity == 0 || (capacity & (capacity - 1)) != 0 || elem_size == 0)
    return NULL;

  /* aligned_alloc gives a 64-byte-aligned base so the struct's first and last
   * cache lines aren't shared with adjacent heap objects. macOS requires the
   * size argument to be a multiple of the alignment, so round up — sizeof is
   * already a multiple of 64 (alignof(struct) is 64 from the alignas on tail),
   * but the round-up is defensive. */
  size_t struct_sz = (sizeof(spsc_ring_t) + 63) & ~(size_t)63;
  spsc_ring_t *ring = aligned_alloc(64, struct_sz);
  if (!ring)
    return NULL;
  ring->buf = malloc(capacity * elem_size);
  if (!ring->buf) {
    free(ring);
    return NULL;
  }
  atomic_init(&ring->head, 0);
  atomic_init(&ring->tail, 0);
  ring->cached_tail  = 0;
  ring->cached_head  = 0;
  ring->element_size = elem_size;
  ring->capacity     = capacity;
  ring->mask         = capacity - 1;
  return ring;
}

static char *spsc__address_of_index(const spsc_ring_t *r, uint64_t idx) {
  return r->buf + (idx & r->mask) * r->element_size;
}

void spsc_ring_destroy(spsc_ring_t *r) {
  free(r->buf);
  free(r);
}
int spsc_ring_push(spsc_ring_t *r, const void *e) {
  uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
  /* Try cached tail first — most pushes succeed without an acquire-load. */
  if (head - r->cached_tail >= r->capacity) {
    r->cached_tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (head - r->cached_tail >= r->capacity) return -1;
  }
  memcpy(spsc__address_of_index(r, head), e, r->element_size);
  atomic_store_explicit(&r->head, head + 1, memory_order_release);
  return 0;
}
int spsc_ring_pop(spsc_ring_t *r, void *e) {
  uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
  if (tail == r->cached_head) {
    r->cached_head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (tail == r->cached_head) return -1;
  }
  memcpy(e, spsc__address_of_index(r, tail), r->element_size);
  atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
  return 0;
}
size_t spsc_ring_push_n(spsc_ring_t *r, const void *e, size_t n) {
  uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
  size_t remaining = r->capacity - (size_t)(head - r->cached_tail);
  if (remaining < n) {
    r->cached_tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    remaining = r->capacity - (size_t)(head - r->cached_tail);
    if (remaining < n) n = remaining;
  }
  if (n == 0) return 0;

  size_t idx = (size_t)(head & r->mask);
  size_t first = r->capacity - idx; /* slots from idx to end of buf */
  if (first > n) first = n;
  memcpy(r->buf + idx * r->element_size, e, first * r->element_size);
  if (n > first) {
    memcpy(r->buf, (const char *)e + first * r->element_size,
           (n - first) * r->element_size);
  }
  atomic_store_explicit(&r->head, head + n, memory_order_release);
  return n;
}

size_t spsc_ring_pop_n(spsc_ring_t *r, void *e, size_t n) {
  uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
  size_t avail = (size_t)(r->cached_head - tail);
  if (avail < n) {
    r->cached_head = atomic_load_explicit(&r->head, memory_order_acquire);
    avail = (size_t)(r->cached_head - tail);
    if (avail < n) n = avail;
  }
  if (n == 0) return 0;

  size_t idx = (size_t)(tail & r->mask);
  size_t first = r->capacity - idx;
  if (first > n) first = n;
  memcpy(e, r->buf + idx * r->element_size, first * r->element_size);
  if (n > first) {
    memcpy((char *)e + first * r->element_size, r->buf,
           (n - first) * r->element_size);
  }
  atomic_store_explicit(&r->tail, tail + n, memory_order_release);
  return n;
}

size_t spsc_ring_size(const spsc_ring_t *r) {
  /* Approximate snapshot — fine for telemetry, not for control flow.
   * (push/pop do their own atomic loads inline for correctness.) */
  uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
  uint64_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
  return (size_t)(h - t);
}
size_t spsc_ring_capacity(const spsc_ring_t *r) { return r->capacity; }
bool spsc_ring_empty(const spsc_ring_t *r) { return spsc_ring_size(r) == 0; }
bool spsc_ring_full(const spsc_ring_t *r) {
  return spsc_ring_size(r) == r->capacity;
}
