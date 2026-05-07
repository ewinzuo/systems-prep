# Day 3 — Userspace "Device" + DMA Descriptor Ring (C)

A pure-userspace simulation of a NIC/NPU-style device: descriptor ring, doorbell
register, completion ring, scatter-gather buffers. No kernel, no drivers — but
the abstraction matches what real driver code looks like.

## Conceptual model
```
+------------ host (driver) ------------+        +-- device (worker thread) --+
|   submission ring (head, tail)        |        |                            |
|   completion ring (head, tail)        |        |                            |
|   doorbell register  (atomic counter) | -----> |   polls doorbell           |
|                                       | <----- |   reads SQ descriptor      |
|   data buffers (host memory)          |  DMA   |   memcpy into "device mem" |
|                                       |  copy  |   writes CQ entry          |
+---------------------------------------+        +----------------------------+
```

Descriptors describe one transfer: buffer pointer, length, op (read/write),
user cookie. The "device" thread polls (or `cv.wait`s on) the doorbell, walks
new descriptors, "performs DMA" by memcpy, and posts completions.

## Files
- `descriptor_ring.h/.c` — submission and completion ring (each is an SPSC ring
  reused from Day 1's design, or built fresh).
- `device.h/.c` — simulated device: worker thread that consumes SQ entries and
  produces CQ entries.
- `host.h/.c` — driver-side API: `submit(buf, len, op)` returns a request id;
  `poll_completions(callback)` drains the CQ.
- `test_dma.c` — smoke + concurrent submission test.

## Edge cases & realism
1. **Doorbells coalesce** — host can queue multiple descriptors and ring the
   doorbell once. Device drains all available, so doorbells are level-triggered
   not edge-triggered.
2. **Completion order ≠ submission order** (in real life). For Day 3 we keep
   them in-order; Day 5 may relax this for benchmarking.
3. **Backpressure** — submit returns `-EAGAIN` when SQ is full; caller decides
   whether to spin, sleep, or drop.
4. **Memory ordering across "DMA"** — device sees descriptor only after the
   host's release-store on the SQ tail; host sees completion data only after
   the device's release-store on the CQ tail. Same release/acquire pattern
   as the rings; matches the real `fence` rules in PCIe.
5. **In-flight tracking** — descriptors are owned by the device until completion
   posts; the host must not recycle the buffer until then.
6. **Shutdown** — a "stop" descriptor lets the device drain and exit cleanly.
7. **Latency vs throughput** — busy-poll vs futex/condvar tradeoff. Both
   provided behind a flag.

## API sketch
```c
typedef struct nic_t nic_t;
typedef enum { OP_TX, OP_RX } op_t;
typedef struct { uint64_t cookie; const void *buf; size_t len; op_t op; } sqe_t;
typedef struct { uint64_t cookie; int status; } cqe_t;

nic_t *nic_open(size_t sq_depth, size_t cq_depth);
void   nic_close(nic_t *);

int    nic_submit(nic_t *, const sqe_t *);                    // 0 ok, -EAGAIN full
size_t nic_drain_completions(nic_t *, cqe_t *out, size_t max);
void   nic_doorbell(nic_t *);                                 // host -> device wake
```

## Likely interview questions
1. Why have separate submission and completion rings instead of one bidirectional one?
2. What's a doorbell in PCIe? Why is it write-only and how is the device notified?
3. Why is the host's release-store on the SQ tail required *after* writing the descriptor?
4. Why do real NICs (e.g., NVMe) prefer 64-byte descriptors? (Cache line.)
5. How would you batch doorbells across many submissions?
6. What happens if the device is much slower than the host? Slower than the host?
7. How does this map onto Trainium / Neuron — what's the analogue of SQ/CQ for collective ops?
