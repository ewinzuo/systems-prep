/* Day 1 microbench. One producer thread + one consumer thread, both wait on
 * an atomic "go" flag. Wall clock starts just before flipping go, stops once
 * both threads join. Throughput = items / wall time.
 *
 * Three runs:
 *   1. single-element push/pop
 *   2. batched push_n / pop_n with n = 64
 *   3. batched push_n / pop_n with n = 256
 *
 * Expectation: batches win big. Each push_n / pop_n call still costs the same
 * function-call + index-update overhead as a single push, but transfers many
 * elements per call. Once atomics are added, the bigger win will be amortizing
 * one release-store across the whole batch.
 *
 * Notes on numbers:
 *   - macOS can't easily pin threads. Run a few times; look at the median.
 *   - At -O2 the loop body is small enough to fit in icache; you're measuring
 *     memcpy + the cost of a cache-line ping-pong on head/tail.
 *   - Compare against `make tsan` numbers — TSan slows ops/sec ~10x. */

#include "spsc_ring.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define N_ITEMS (1u << 24)   /* 16M */
#define CAP     1024u
#define BUFSZ   256u         /* must be >= largest batch size */

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static spsc_ring_t *g_ring;
static atomic_int   g_go;
static uint32_t     g_batch;   /* batch size for the batched runs */

/* ---- single-element ---- */

static void *producer_single(void *arg) {
  (void)arg;
  while (!atomic_load_explicit(&g_go, memory_order_acquire)) ;
  for (uint32_t i = 0; i < N_ITEMS; ) {
    if (spsc_ring_push(g_ring, &i) == 0) i++;
  }
  return NULL;
}

static void *consumer_single(void *arg) {
  (void)arg;
  while (!atomic_load_explicit(&g_go, memory_order_acquire)) ;
  uint32_t v;
  for (uint32_t i = 0; i < N_ITEMS; ) {
    if (spsc_ring_pop(g_ring, &v) == 0) i++;
  }
  return NULL;
}

static void bench_single(void) {
  g_ring = spsc_ring_create(CAP, sizeof(uint32_t));
  atomic_store_explicit(&g_go, 0, memory_order_relaxed);
  pthread_t p, c;
  pthread_create(&p, NULL, producer_single, NULL);
  pthread_create(&c, NULL, consumer_single, NULL);
  double t0 = now_sec();
  atomic_store_explicit(&g_go, 1, memory_order_release);
  pthread_join(p, NULL);
  pthread_join(c, NULL);
  double dt = now_sec() - t0;
  printf("  single push/pop : %u items in %.3fs = %7.2f Mops/s (%5.1f ns/op)\n",
         N_ITEMS, dt, N_ITEMS / dt / 1e6, dt * 1e9 / N_ITEMS);
  spsc_ring_destroy(g_ring);
}

/* ---- batched ---- */

static void *producer_batch(void *arg) {
  (void)arg;
  uint32_t buf[BUFSZ];
  for (uint32_t i = 0; i < BUFSZ; i++) buf[i] = i;  /* values aren't checked */
  while (!atomic_load_explicit(&g_go, memory_order_acquire)) ;
  for (uint32_t pushed = 0; pushed < N_ITEMS; ) {
    uint32_t want = N_ITEMS - pushed;
    if (want > g_batch) want = g_batch;
    pushed += (uint32_t)spsc_ring_push_n(g_ring, buf, want);
  }
  return NULL;
}

static void *consumer_batch(void *arg) {
  (void)arg;
  uint32_t buf[BUFSZ];
  while (!atomic_load_explicit(&g_go, memory_order_acquire)) ;
  for (uint32_t popped = 0; popped < N_ITEMS; ) {
    uint32_t want = N_ITEMS - popped;
    if (want > g_batch) want = g_batch;
    popped += (uint32_t)spsc_ring_pop_n(g_ring, buf, want);
  }
  return NULL;
}

static void bench_batch(uint32_t n) {
  g_batch = n;
  g_ring = spsc_ring_create(CAP, sizeof(uint32_t));
  atomic_store_explicit(&g_go, 0, memory_order_relaxed);
  pthread_t p, c;
  pthread_create(&p, NULL, producer_batch, NULL);
  pthread_create(&c, NULL, consumer_batch, NULL);
  double t0 = now_sec();
  atomic_store_explicit(&g_go, 1, memory_order_release);
  pthread_join(p, NULL);
  pthread_join(c, NULL);
  double dt = now_sec() - t0;
  printf("  batch n=%-3u     : %u items in %.3fs = %7.2f Mops/s (%5.1f ns/op)\n",
         n, N_ITEMS, dt, N_ITEMS / dt / 1e6, dt * 1e9 / N_ITEMS);
  spsc_ring_destroy(g_ring);
}

int main(void) {
  printf("spsc bench (1 producer + 1 consumer, %u items, ring cap %u):\n",
         N_ITEMS, CAP);
  bench_single();
  bench_batch(64);
  bench_batch(256);
  return 0;
}
