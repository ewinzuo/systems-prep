/* Day 1 unit tests for spsc_ring.
 *
 * Coverage:
 *   - create() argument validation
 *   - single push/pop round-trip; empty pop fails
 *   - fill-to-capacity, over-capacity push fails
 *   - many push/pop laps exercise masked-index wrap
 *   - batch push_n / pop_n with partial fill, partial drain, and a batch that
 *     straddles the wrap boundary (regression for the heap-OOB bug)
 *   - 1 producer + 1 consumer concurrent test, ~4M items, consumer sees
 *     strict monotonic sequence
 *
 * Note on the concurrent test:
 *   head/tail are _Atomic uint64_t, accessed via atomic_load/store_explicit
 *   with relaxed self-loads, acquire on the peer's counter, and release on
 *   the self-store. `make tsan` should be clean. */

#include "spsc_ring.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_EQ(a, b) do {                                      \
  long long _a = (long long)(a), _b = (long long)(b);             \
  if (_a != _b) {                                                 \
    fprintf(stderr, "%s:%d: ASSERT_EQ %s=%lld vs %s=%lld\n",      \
            __FILE__, __LINE__, #a, _a, #b, _b);                  \
    exit(1);                                                      \
  }                                                               \
} while (0)

#define ASSERT_TRUE(c) do {                                       \
  if (!(c)) {                                                     \
    fprintf(stderr, "%s:%d: %s is false\n", __FILE__, __LINE__, #c); \
    exit(1);                                                      \
  }                                                               \
} while (0)

/* ---- single-threaded -------------------------------------------------- */

static void test_create_validation(void) {
  ASSERT_TRUE(spsc_ring_create(0, 4) == NULL);
  ASSERT_TRUE(spsc_ring_create(8, 0) == NULL);
  spsc_ring_t *r = spsc_ring_create(8, sizeof(int));
  ASSERT_TRUE(r != NULL);
  ASSERT_EQ(spsc_ring_capacity(r), 8);
  ASSERT_TRUE(spsc_ring_empty(r));
  ASSERT_TRUE(!spsc_ring_full(r));
  ASSERT_EQ(spsc_ring_size(r), 0);
  spsc_ring_destroy(r);
  puts("  test_create_validation OK");
}

static void test_single_push_pop(void) {
  spsc_ring_t *r = spsc_ring_create(8, sizeof(int));
  int v = 0;
  ASSERT_EQ(spsc_ring_pop(r, &v), -1);     /* empty */
  int x = 42;
  ASSERT_EQ(spsc_ring_push(r, &x), 0);
  ASSERT_EQ(spsc_ring_size(r), 1);
  ASSERT_EQ(spsc_ring_pop(r, &v), 0);
  ASSERT_EQ(v, 42);
  ASSERT_TRUE(spsc_ring_empty(r));
  spsc_ring_destroy(r);
  puts("  test_single_push_pop OK");
}

static void test_fill_to_capacity(void) {
  spsc_ring_t *r = spsc_ring_create(4, sizeof(int));
  for (int i = 0; i < 4; i++) ASSERT_EQ(spsc_ring_push(r, &i), 0);
  ASSERT_TRUE(spsc_ring_full(r));
  int rej = 99;
  ASSERT_EQ(spsc_ring_push(r, &rej), -1);  /* over-capacity */
  ASSERT_EQ(spsc_ring_size(r), 4);
  int got;
  for (int i = 0; i < 4; i++) {
    ASSERT_EQ(spsc_ring_pop(r, &got), 0);
    ASSERT_EQ(got, i);
  }
  ASSERT_EQ(spsc_ring_pop(r, &got), -1);   /* empty */
  spsc_ring_destroy(r);
  puts("  test_fill_to_capacity OK");
}

static void test_wrap_around(void) {
  /* 1000 push/pop pairs through a 4-slot ring exercises masked-index wrap. */
  spsc_ring_t *r = spsc_ring_create(4, sizeof(int));
  int got;
  for (int i = 0; i < 1000; i++) {
    ASSERT_EQ(spsc_ring_push(r, &i), 0);
    ASSERT_EQ(spsc_ring_pop(r, &got), 0);
    ASSERT_EQ(got, i);
  }
  spsc_ring_destroy(r);
  puts("  test_wrap_around OK");
}

static void test_batch_basic(void) {
  spsc_ring_t *r = spsc_ring_create(8, sizeof(int));
  int in[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  ASSERT_EQ(spsc_ring_push_n(r, in, 10), 8);  /* only 8 fit */
  ASSERT_TRUE(spsc_ring_full(r));
  int out[10] = {0};
  ASSERT_EQ(spsc_ring_pop_n(r, out, 10), 8);
  for (int i = 0; i < 8; i++) ASSERT_EQ(out[i], i);
  ASSERT_TRUE(spsc_ring_empty(r));
  spsc_ring_destroy(r);
  puts("  test_batch_basic OK");
}

static void test_batch_partial(void) {
  spsc_ring_t *r = spsc_ring_create(4, sizeof(int));
  int two[2] = {1, 2};
  ASSERT_EQ(spsc_ring_push_n(r, two, 2), 2);
  int four[4] = {3, 4, 5, 6};
  ASSERT_EQ(spsc_ring_push_n(r, four, 4), 2);   /* only 2 fit */
  ASSERT_TRUE(spsc_ring_full(r));
  int got[5];
  ASSERT_EQ(spsc_ring_pop_n(r, got, 5), 4);     /* asking for more than there is */
  ASSERT_EQ(got[0], 1); ASSERT_EQ(got[1], 2);
  ASSERT_EQ(got[2], 3); ASSERT_EQ(got[3], 4);
  spsc_ring_destroy(r);
  puts("  test_batch_partial OK");
}

static void test_batch_wrap(void) {
  /* Regression for the heap-OOB: a batch must split into two memcpys when
   * (head % capacity) + n > capacity. */
  spsc_ring_t *r = spsc_ring_create(8, sizeof(int));
  int seed[6] = {0, 1, 2, 3, 4, 5};
  int dump[6] = {0};
  ASSERT_EQ(spsc_ring_push_n(r, seed, 6), 6);
  ASSERT_EQ(spsc_ring_pop_n (r, dump, 6), 6);   /* head=6, tail=6, empty */

  int next[5] = {10, 11, 12, 13, 14};
  ASSERT_EQ(spsc_ring_push_n(r, next, 5), 5);   /* slots 6,7 then wrap to 0,1,2 */
  int got[5] = {0};
  ASSERT_EQ(spsc_ring_pop_n(r, got, 5), 5);
  for (int i = 0; i < 5; i++) ASSERT_EQ(got[i], 10 + i);
  spsc_ring_destroy(r);
  puts("  test_batch_wrap OK");
}

/* ---- multithreaded SPSC ----------------------------------------------- */

#define CONC_CAP    1024u
#define CONC_ITEMS  (1u << 22)   /* ~4M */

static spsc_ring_t *g_ring;
static atomic_int   g_ready;

static void *producer_fn(void *arg) {
  (void)arg;
  while (!atomic_load_explicit(&g_ready, memory_order_acquire)) ;
  for (uint32_t i = 0; i < CONC_ITEMS; ) {
    if (spsc_ring_push(g_ring, &i) == 0) i++;
  }
  return NULL;
}

static void *consumer_fn(void *arg) {
  uint32_t expected = 0, got = 0;
  while (!atomic_load_explicit(&g_ready, memory_order_acquire)) ;
  while (expected < CONC_ITEMS) {
    if (spsc_ring_pop(g_ring, &got) == 0) {
      if (got != expected) {
        fprintf(stderr, "concurrency drift: got=%u expected=%u\n", got, expected);
        exit(2);
      }
      expected++;
    }
  }
  *(int *)arg = 1;
  return NULL;
}

static void test_concurrent_spsc(void) {
  g_ring = spsc_ring_create(CONC_CAP, sizeof(uint32_t));
  ASSERT_TRUE(g_ring != NULL);
  atomic_store_explicit(&g_ready, 0, memory_order_relaxed);
  int done = 0;
  pthread_t p, c;
  pthread_create(&p, NULL, producer_fn, NULL);
  pthread_create(&c, NULL, consumer_fn, &done);
  atomic_store_explicit(&g_ready, 1, memory_order_release);
  pthread_join(p, NULL);
  pthread_join(c, NULL);
  ASSERT_EQ(done, 1);
  ASSERT_TRUE(spsc_ring_empty(g_ring));
  spsc_ring_destroy(g_ring);
  puts("  test_concurrent_spsc OK");
}

int main(void) {
  puts("running spsc tests...");
  test_create_validation();
  test_single_push_pop();
  test_fill_to_capacity();
  test_wrap_around();
  test_batch_basic();
  test_batch_partial();
  test_batch_wrap();
  test_concurrent_spsc();
  puts("ALL OK");
  return 0;
}
