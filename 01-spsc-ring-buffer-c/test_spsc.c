/* Day 1 unit tests. Stub — implement alongside spsc_ring.c.
 *
 * Cases to cover (from README's edge-case list):
 *   - test_create_validation:     0/non-pow2 capacity, 0 elem_size all return NULL.
 *   - test_single_push_pop:       push then pop returns the same value; pop on empty returns -1.
 *   - test_fill_to_capacity:      push capacity items succeeds; capacity+1 returns -1; size is correct.
 *   - test_wrap_around:           1000 push/pop pairs through a tiny ring exercises masked-index wrap.
 *   - test_batch:                 push_n / pop_n; partial-fill, partial-drain, wrap inside a batch.
 *   - test_concurrent:            1 producer + 1 consumer, ~4M items, consumer sees strict monotonic sequence.
 *   - run under TSan and ASan (`make tsan`, `make asan`).
 *
 * Suggested helper:
 *   #define ASSERT_EQ(a, b) ...    fprintf to stderr, exit(1) on failure
 */

#include "spsc_ring.h"

#include <stdio.h>

int main(void) {
    puts("spsc tests stub — implement me");
    return 0;
}
