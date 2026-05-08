// Day 2 unit + concurrent tests.
//
// The queue API (push, pop, size, empty, full, capacity) is the same across
// SPSC / MPSC / SPMC / MPMC. Only the concurrent invariants change.
//
//   - Single-threaded tests below work for any variant (SPSC included).
//   - SPSC concurrent test: 1 producer + 1 consumer. Works now.
//   - MPSC concurrent test: N producers + 1 consumer. Enable once your
//     queue handles multiple producers safely.
//   - MPMC concurrent test: N producers + M consumers. Enable once your
//     queue handles both ends contended.
//
// Run under TSan (`make tsan`) and ASan (`make asan`).

#include "mpmc_queue.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#define ASSERT_EQ(a, b) do {                                           \
    auto _a = (a); auto _b = (b);                                      \
    if (_a != _b) {                                                    \
        std::cerr << __FILE__ << ":" << __LINE__                       \
                  << " ASSERT_EQ failed: " #a " (" << _a << ")"        \
                  << " != " #b " (" << _b << ")\n";                    \
        std::exit(1);                                                  \
    }                                                                  \
} while (0)

#define ASSERT_TRUE(c) do {                                            \
    if (!(c)) {                                                        \
        std::cerr << __FILE__ << ":" << __LINE__                       \
                  << " ASSERT_TRUE(" #c ") failed\n";                  \
        std::exit(1);                                                  \
    }                                                                  \
} while (0)

// ---- single-threaded -------------------------------------------------------

static void test_construction() {
    ip::MpscQueue<int> q(8);
    ASSERT_EQ(q.capacity(), 8u);
    ASSERT_TRUE(q.empty());
    ASSERT_TRUE(!q.full());
    ASSERT_EQ(q.size(), 0u);
    std::cout << "  test_construction OK\n";
}

static void test_single_push_pop() {
    ip::MpscQueue<int> q(8);
    int v = 0;
    ASSERT_TRUE(!q.pop(v));                // empty
    ASSERT_TRUE(q.push(42));
    ASSERT_EQ(q.size(), 1u);
    ASSERT_TRUE(q.pop(v));
    ASSERT_EQ(v, 42);
    ASSERT_TRUE(q.empty());
    std::cout << "  test_single_push_pop OK\n";
}

static void test_fill_to_capacity() {
    ip::MpscQueue<int> q(4);
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(q.push(i));
    ASSERT_TRUE(q.full());
    ASSERT_TRUE(!q.push(99));              // over-capacity rejected
    ASSERT_EQ(q.size(), 4u);
    int v;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.pop(v));
        ASSERT_EQ(v, i);                   // FIFO order preserved
    }
    ASSERT_TRUE(!q.pop(v));                // empty again
    std::cout << "  test_fill_to_capacity OK\n";
}

static void test_wrap_around() {
    // 1000 push/pop pairs through a 4-slot ring exercises masked-index wrap.
    ip::MpscQueue<int> q(4);
    int v;
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(q.push(i));
        ASSERT_TRUE(q.pop(v));
        ASSERT_EQ(v, i);
    }
    std::cout << "  test_wrap_around OK\n";
}

// ---- SPSC concurrent: 1 producer + 1 consumer -----------------------------

static constexpr std::uint32_t kSpscCap   = 1024;
static constexpr std::uint32_t kSpscItems = 1u << 22;     // ~4M

static void test_concurrent_spsc() {
    ip::MpscQueue<std::uint32_t> q(kSpscCap);
    std::atomic<bool> ready{false};
    std::atomic<bool> ok{true};

    std::thread producer([&] {
        while (!ready.load(std::memory_order_acquire)) ;
        for (std::uint32_t i = 0; i < kSpscItems; ) {
            if (q.push(i)) ++i;
        }
    });

    std::thread consumer([&] {
        std::uint32_t expected = 0, got = 0;
        while (!ready.load(std::memory_order_acquire)) ;
        while (expected < kSpscItems) {
            if (q.pop(got)) {
                if (got != expected) {
                    std::cerr << "spsc drift: got=" << got
                              << " expected=" << expected << "\n";
                    ok.store(false, std::memory_order_release);
                    return;
                }
                ++expected;
            }
        }
    });

    ready.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
    ASSERT_TRUE(ok.load());
    ASSERT_TRUE(q.empty());
    std::cout << "  test_concurrent_spsc OK\n";
}

// ---- MPSC concurrent: N producers + 1 consumer ----------------------------
// Invariants verified:
//   - every (producer_id, seq) pair pushed appears exactly once on the
//     consumer side
//   - per-producer ordering: the consumer sees producer P's items in
//     increasing seq (FIFO within a producer; cross-producer ordering is
//     not guaranteed)

static constexpr std::uint32_t kMpscProducers = 4;
static constexpr std::uint32_t kMpscPerProd   = 1u << 18;   // 256K each = 1M total
static constexpr std::uint32_t kMpscCap       = 1024;

// pack (producer_id, seq) into a uint64_t so we can push/pop a single value
static inline std::uint64_t pack(std::uint32_t pid, std::uint32_t seq) {
    return (static_cast<std::uint64_t>(pid) << 32) | seq;
}
static inline std::uint32_t unpack_pid(std::uint64_t v) { return static_cast<std::uint32_t>(v >> 32); }
static inline std::uint32_t unpack_seq(std::uint64_t v) { return static_cast<std::uint32_t>(v); }

[[maybe_unused]] static void test_concurrent_mpsc() {
    ip::MpscQueue<std::uint64_t> q(kMpscCap);
    std::atomic<bool> ready{false};

    std::vector<std::thread> producers;
    producers.reserve(kMpscProducers);
    for (std::uint32_t p = 0; p < kMpscProducers; ++p) {
        producers.emplace_back([&, p] {
            while (!ready.load(std::memory_order_acquire)) ;
            for (std::uint32_t i = 0; i < kMpscPerProd; ) {
                if (q.push(pack(p, i))) ++i;
            }
        });
    }

    // per-producer last-seen seq, for FIFO-within-producer check
    std::vector<std::uint32_t> last_seq(kMpscProducers, 0);
    std::vector<bool> first(kMpscProducers, true);
    std::atomic<bool> ok{true};

    std::thread consumer([&] {
        while (!ready.load(std::memory_order_acquire)) ;
        std::uint32_t total = 0;
        std::uint64_t got = 0;
        const std::uint32_t expected_total = kMpscProducers * kMpscPerProd;
        while (total < expected_total) {
            if (q.pop(got)) {
                std::uint32_t pid = unpack_pid(got);
                std::uint32_t seq = unpack_seq(got);
                if (pid >= kMpscProducers) {
                    ok.store(false, std::memory_order_release);
                    return;
                }
                if (first[pid]) {
                    if (seq != 0) { ok.store(false, std::memory_order_release); return; }
                    first[pid] = false;
                } else if (seq != last_seq[pid] + 1) {
                    std::cerr << "mpsc out-of-order for pid=" << pid
                              << " last=" << last_seq[pid] << " got=" << seq << "\n";
                    ok.store(false, std::memory_order_release);
                    return;
                }
                last_seq[pid] = seq;
                ++total;
            }
        }
    });

    ready.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    consumer.join();
    ASSERT_TRUE(ok.load());
    ASSERT_TRUE(q.empty());
    std::cout << "  test_concurrent_mpsc OK\n";
}

// ---- MPMC concurrent: N producers + M consumers ---------------------------
// Invariants verified:
//   - the union of all consumed items equals the set of all produced items
//     (no duplicates, no losses)

static constexpr std::uint32_t kMpmcProducers = 4;
static constexpr std::uint32_t kMpmcConsumers = 4;
static constexpr std::uint32_t kMpmcPerProd   = 1u << 17;   // 128K each
static constexpr std::uint32_t kMpmcCap       = 1024;

[[maybe_unused]] static void test_concurrent_mpmc() {
    ip::MpscQueue<std::uint64_t> q(kMpmcCap);
    std::atomic<bool> ready{false};
    std::atomic<std::uint64_t> total_consumed{0};
    const std::uint64_t expected_total = std::uint64_t(kMpmcProducers) * kMpmcPerProd;

    std::vector<std::thread> producers;
    producers.reserve(kMpmcProducers);
    for (std::uint32_t p = 0; p < kMpmcProducers; ++p) {
        producers.emplace_back([&, p] {
            while (!ready.load(std::memory_order_acquire)) ;
            for (std::uint32_t i = 0; i < kMpmcPerProd; ) {
                if (q.push(pack(p, i))) ++i;
            }
        });
    }

    std::vector<std::vector<std::uint64_t>> per_consumer(kMpmcConsumers);
    std::vector<std::thread> consumers;
    consumers.reserve(kMpmcConsumers);
    for (std::uint32_t c = 0; c < kMpmcConsumers; ++c) {
        consumers.emplace_back([&, c] {
            while (!ready.load(std::memory_order_acquire)) ;
            std::uint64_t got = 0;
            while (total_consumed.load(std::memory_order_relaxed) < expected_total) {
                if (q.pop(got)) {
                    per_consumer[c].push_back(got);
                    total_consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    ready.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    // Union of all consumer collections == set of all produced (pid, seq) pairs.
    std::set<std::uint64_t> seen;
    for (auto& v : per_consumer)
        for (auto x : v)
            ASSERT_TRUE(seen.insert(x).second);    // false → duplicate
    ASSERT_EQ(seen.size(), expected_total);

    for (std::uint32_t p = 0; p < kMpmcProducers; ++p)
        for (std::uint32_t i = 0; i < kMpmcPerProd; ++i)
            ASSERT_TRUE(seen.count(pack(p, i)) == 1);

    ASSERT_TRUE(q.empty());
    std::cout << "  test_concurrent_mpmc OK\n";
}

// ---------------------------------------------------------------------------

int main() {
    std::cout << "running queue tests...\n";
    test_construction();
    test_single_push_pop();
    test_fill_to_capacity();
    test_wrap_around();
    test_concurrent_spsc();
    test_concurrent_mpsc();        // ← MPSC implementation now in place

    // Uncomment when you implement MPMC:
    // test_concurrent_mpmc();

    std::cout << "ALL OK\n";
    return 0;
}
