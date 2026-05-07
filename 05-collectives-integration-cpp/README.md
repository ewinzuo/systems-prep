# Day 5 — Collectives Integration & Benchmarks (C++)

Pulls Days 1–4 together. Adds two more algorithms, swaps the in-memory transport
for a thin wrapper over Day 3's NIC simulation, and runs latency / bandwidth
microbenchmarks.

## Deliverables

### New algorithms
- **Tree broadcast** (binomial tree). Latency `log₂N`, used for sending one
  rank's tensor to every other rank.
- **Reduce-scatter** standalone (the first half of ring all-reduce, exposed
  separately because it's a primitive on its own).

### New transport
- `transport_dma.hpp/.cpp` — adapts the Day 3 `nic_t` to the Day 4 `Transport`
  interface. Each rank runs in its own thread; "remote" sends use the NIC
  simulator instead of an in-process queue.

### Benchmarks (`bench_collectives.cpp`)
- Sweep `N ∈ {2, 4, 8}` and tensor sizes `S ∈ {1KB, 64KB, 4MB}`.
- For each: run ring all-reduce vs recursive-doubling all-reduce vs tree
  broadcast where applicable.
- Print latency (per call) and effective bandwidth (bytes / wallclock).
- Show the small-msg / large-msg crossover where ring overtakes recursive
  doubling.

## Edge cases
1. **N not power of 2** — implement binomial tree broadcast properly (handles
   any N) and ring all-reduce (handles any N if you do remainder chunks).
2. **DMA transport latency floor** — the simulated device adds a fixed cost
   per descriptor; verify it shows up in the small-message latency numbers.
3. **Concurrency between collectives** — multiple collectives in flight on
   the same NIC need either serialization or stream IDs. Use a single
   in-flight collective for the bench.

## Likely interview questions
1. How does a binomial-tree broadcast compare to a chain broadcast?
2. Why is reduce-scatter a useful standalone primitive? (Hint: ZeRO sharding.)
3. What's the latency floor for any algorithm that sends one byte across `N` ranks
   in a ring topology?
4. How would topology-aware NCCL pick algorithms when ranks span PCIe + NVLink?
5. How do you handle stragglers / failed ranks in a real collective?
