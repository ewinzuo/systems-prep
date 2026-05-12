# Interview Prep — 5-Day Plan


The five projects build on each other: Days 1–2 produce ring buffers, Day 3 wraps a
ring buffer in a "device" with DMA descriptors, and Days 4–5 implement collective
algorithms that ride on top of those transports.

## Schedule

| Day | Project                                   | Lang | Concept                              |
|-----|-------------------------------------------|------|--------------------------------------|
| 1   | `01-spsc-ring-buffer-c`                   | C    | Lock-free SPSC ring, memory orders   |
| 2   | `02-mpmc-ring-buffer-cpp`                 | C++  | MPMC bounded queue (Vyukov-style)    |
| 3   | `03-userspace-driver-dma-c`               | C    | Char-device + DMA descriptor ring    |
| 4   | `04-collectives-cpp`                      | C++  | Ring all-reduce, recursive doubling  |
| 5   | `05-collectives-integration-cpp`          | C++  | Tree bcast, reduce-scatter, benches  |

## Why this split
- C for the kernel-flavored stuff (raw atomics, DMA descriptors, "driver" plumbing).
- C++ for the parts that look like NCCL / Neuron Collectives (templates, RAII, modern atomics, allocator awareness).
- Day 5 wires Day 4's collectives onto Day 3's transport, so the final demo is end-to-end.

## Build everything
```sh
for d in 0*; do (cd "$d" && make) ; done
```

## Run all tests
```sh
for d in 0*; do (cd "$d" && make test) ; done
```

## What "done" looks like for each project
Each subdirectory's README has its own success criteria. In general:
1. Builds clean with `-Wall -Wextra -Wpedantic` (and `-Werror` for the C ones).
2. Passes its own unit tests under TSan and ASan.
3. Has a one-page README explaining design, edge cases, and what an interviewer might ask.
