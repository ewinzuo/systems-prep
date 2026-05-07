# Day 4 — Collective Communication Algorithms (C++)

Implement the two cornerstone collective algorithms in-process, with threads
playing the role of "ranks" and a `Transport` interface for sending bytes
between them. Today we use a simple in-memory transport (a 2D matrix of MPMC
queues from Day 2). Day 5 swaps in the Day 3 DMA-ring transport.

## Algorithms

### 1. Ring all-reduce (Baidu / Horovod / NCCL)
Used by every modern deep-learning framework for gradient averaging.

For `N` ranks and a tensor of size `S`:
1. Conceptually split the tensor into `N` chunks.
2. **Reduce-scatter phase** — `N-1` steps: each step, rank `r` sends chunk
   `(r - step) mod N` to rank `(r+1) mod N` and receives chunk
   `(r - step - 1) mod N` from rank `(r-1) mod N`, accumulating into its copy.
3. After reduce-scatter, each rank owns the fully-reduced version of one chunk.
4. **All-gather phase** — another `N-1` steps: rotate the fully-reduced chunks
   around the ring until everyone has every chunk.

Bandwidth-optimal: each rank sends `2(N-1)/N · S ≈ 2S` bytes, regardless of `N`.

### 2. Recursive-doubling all-gather (and all-reduce variant)
Better latency for small messages. For `N = 2^k` ranks:
- Step `i` (i = 0…k-1): rank `r` exchanges its current accumulated buffer
  with rank `r XOR 2^i`, doubling the data each rank holds.
- Total `log₂N` steps, but each step transfers up to `S/2` then `S` etc.

Latency-optimal: `log₂N` rounds; bandwidth-suboptimal at large `S`.

## Decision rule
- Small message (≪ N · MTU): recursive doubling — fewer rounds wins.
- Large message (≫ N · MTU): ring — bandwidth-optimal wins.
- NCCL chooses dynamically based on size and topology.

## Files
- `collective.hpp` — `Transport` interface, `Rank` / `Group` types.
- `transport_inproc.{hpp,cpp}` — in-memory transport: `N×N` MPMC queues.
- `allreduce_ring.cpp` — ring all-reduce.
- `allgather_recdouble.cpp` — recursive-doubling all-gather.
- `test_collectives.cpp` — verify correctness for a few sizes / N values.

## Edge cases
1. **Non-power-of-2 N for recursive doubling** — handle by either padding
   (pair leftover ranks with binomial-tree partners) or falling back to ring.
   Day 4: assert pow2 and document the limitation.
2. **Tensor size not divisible by N (ring)** — the last chunk takes the
   remainder; sends/recvs adjust per step.
3. **Numeric precision** — float reductions are non-associative, so the
   answer can differ from a sequential reduction. Tests compare with a
   known-correct sequential value but allow `eps`.
4. **Send/recv ordering** — every rank sends *before* the corresponding recv
   so we don't deadlock on tiny rendezvous queues.
5. **Buffer ownership** — pass-by-pointer with the contract that the caller
   keeps the buffer alive for the duration of the collective.

## Likely interview questions
1. Walk through ring all-reduce. Why "2S" bytes per rank?
2. When would you pick recursive doubling over ring?
3. What's reduce-scatter? How do you turn it + all-gather into all-reduce?
4. What about non-power-of-2 ranks?
5. How does NCCL pick between algorithms?
6. What's the impact of non-associative float math on multi-step reductions?
   How do frameworks deal with reproducibility?
