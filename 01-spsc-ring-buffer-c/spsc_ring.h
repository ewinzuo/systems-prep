#ifndef SPSC_RING_H
#define SPSC_RING_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spsc_ring spsc_ring_t;

/* capacity must be a power of two and > 0; elem_size must be > 0.
 * Returns NULL on invalid args or allocation failure. */
spsc_ring_t *spsc_ring_create(size_t capacity, size_t elem_size);
void         spsc_ring_destroy(spsc_ring_t *r);

/* Returns 0 on success, -1 if full / empty respectively. */
int spsc_ring_push(spsc_ring_t *r, const void *elem);
int spsc_ring_pop (spsc_ring_t *r,       void *elem);

/* Batch variants. Return number of elements actually transferred (may be < n). */
size_t spsc_ring_push_n(spsc_ring_t *r, const void *elems, size_t n);
size_t spsc_ring_pop_n (spsc_ring_t *r,       void *elems, size_t n);

/* Approximate size (racy under concurrent access — use for telemetry only). */
size_t spsc_ring_size    (const spsc_ring_t *r);
size_t spsc_ring_capacity(const spsc_ring_t *r);
bool   spsc_ring_empty   (const spsc_ring_t *r);
bool   spsc_ring_full    (const spsc_ring_t *r);

#ifdef __cplusplus
}
#endif

#endif /* SPSC_RING_H */
