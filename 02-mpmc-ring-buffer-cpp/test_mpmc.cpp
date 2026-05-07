// Day 2 unit + concurrent tests for MpmcQueue.
//
// TODO (Day 2 implementation):
//   - port test_create_validation / single push-pop / fill / over-capacity from Day 1
//   - 4P/4C concurrent test that records (producer_id, seq) per item and verifies
//     each pair shows up exactly once on the consumer side
//   - run under TSan and ASan

#include "mpmc_queue.hpp"

#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    ip::MpmcQueue<int> q(8);
    int v;
    assert(!q.try_pop(v));
    for (int i = 0; i < 8; i++) assert(q.try_push(i));
    assert(!q.try_push(99));            // full
    for (int i = 0; i < 8; i++) {
        assert(q.try_pop(v));
        assert(v == i);
    }
    assert(!q.try_pop(v));
    std::cout << "smoke OK\n";
}
