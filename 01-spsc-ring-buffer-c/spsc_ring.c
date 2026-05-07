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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct spsc_ring {
  uint64_t head;
  uint64_t tail;
  size_t capacity;
  size_t element_size;
  char *buf;
};
spsc_ring_t *spsc_ring_create(size_t capacity, size_t elem_size) {
  spsc_ring_t *ring = malloc(sizeof *ring);
  char *buf = malloc(capacity * elem_size);
  ring->buf = buf;
  ring->head = 0;
  ring->tail = 0;
  ring->element_size = elem_size;
  ring->capacity = capacity;
  return ring;
}

static char *spsc__address_of_index(spsc_ring_t *r, uint64_t idx) {
  return r->buf + (idx % r->capacity) * r->element_size;
}

void spsc_ring_destroy(spsc_ring_t *r) {
  free(r->buf);
  free(r);
}
int spsc_ring_push(spsc_ring_t *r, const void *e) {
  if (spsc_ring_full(r))
    return -1;
  memcpy(spsc__address_of_index(r, r->head), e, r->element_size);
  r->head += 1;
  return 0;
}
int spsc_ring_pop(spsc_ring_t *r, void *e) {
  if (spsc_ring_empty(r))
    return -1;
  memcpy(e, spsc__address_of_index(r, r->tail), r->element_size);
  r->tail += 1;
  return 0;
}
size_t spsc_ring_push_n(spsc_ring_t *r, const void *e, size_t n) {
  size_t remaining = spsc_ring_capacity(r) - spsc_ring_size(r);
  if (remaining < n) {
    n = remaining;
  }

  memcpy(spsc__address_of_index(r, r->head), e, n * r->element_size);
  r->head += n;
  return n;
}

size_t spsc_ring_pop_n(spsc_ring_t *r, void *e, size_t n) {
  if (spsc_ring_size(r) < n)
    n = spsc_ring_size(r);

  memcpy(e, spsc__address_of_index(r, r->tail), n * r->element_size);
  r->tail += n;
  return n;
}

size_t spsc_ring_size(const spsc_ring_t *r) { return r->head - r->tail; }
size_t spsc_ring_capacity(const spsc_ring_t *r) { return r->capacity; }
bool spsc_ring_empty(const spsc_ring_t *r) { return r->head == r->tail; }
bool spsc_ring_full(const spsc_ring_t *r) {
  return spsc_ring_size(r) == spsc_ring_capacity(r);
}
