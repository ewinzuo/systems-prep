// Day 2 unit + concurrent tests for MpmcQueue. Stub — implement alongside mpmc_queue.hpp.
//
// Cases to cover:
//   - construction: 0 capacity throws; non-power-of-two throws;
//     valid capacity initializes empty (size_approx() == 0).
//   - try_pop on empty returns false.
//   - try_push fills to capacity; (capacity+1)th push returns false.
//   - try_pop drains FIFO; subsequent try_pop returns false.
//   - 4 producers x 4 consumers concurrent: each producer pushes
//     1M (producer_id, seq) pairs; consumers collect; verify every pair
//     appears exactly once across all consumers (FIFO per producer; not
//     across producers).
//   - run under TSan (`make tsan`) and ASan (`make asan`).

#include "mpmc_queue.hpp"

#include <iostream>

int main() {
    std::cout << "mpmc tests stub — implement me\n";
    return 0;
}
