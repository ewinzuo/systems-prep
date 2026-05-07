/* Day 1 microbench. Stub — implement alongside spsc_ring.c.
 *
 * Two scenarios:
 *   1. single-element push/pop, ~16M items, report Mops/s
 *   2. batched push_n / pop_n, n=64, same total, report Mops/s
 * Expectation: batch wins by ~10-50x because the release-store on the index
 * is amortized over the whole batch and the slot writes coalesce.
 *
 * Notes:
 *   - clock_gettime(CLOCK_MONOTONIC, ...) for wall time
 *   - use atomic "go" flag so both threads start measuring at the same instant
 *   - on macOS you can't easily pin threads to cores; numbers will be noisy
 */

#include "spsc_ring.h"

#include <stdio.h>

int main(void) {
    puts("spsc bench stub — implement me");
    return 0;
}
