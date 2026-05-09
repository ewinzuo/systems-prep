# Day 3 — DMA Engine Driver with Descriptor Rings (C)

A Linux kernel module that implements an accelerator-style DMA engine driver:
submission and completion descriptor rings, doorbell registers, multi-engine
scheduling, and chained transfers. The "hardware" is simulated by kernel
threads, but all kernel APIs and data structures match what a real Trainium/NIC
driver looks like.

**Time budget: 2 days.** Day 1: core driver (single engine, SQ/CQ, doorbell,
basic DMA). Day 2: multi-engine, chained transfers, peer-to-peer, and
bandwidth tests.

## Conceptual model
```
+------------ driver (kernel module) -----------+      +-- DMA engine (kthread) --+
|                                                |      |                          |
|   SQ ring 0  [head/tail, coherent DMA buf]     | ---> |  engine 0: poll doorbell |
|   CQ ring 0  [head/tail, coherent DMA buf]     | <--- |    read SQE, do xfer    |
|   doorbell 0  (atomic write -> wake)           |      |    post CQE, wake host  |
|                                                |      +--------------------------+
|   SQ ring 1  ...                               | ---> |  engine 1 ...           |
|   CQ ring 1  ...                               | <--- |                          |
|   doorbell 1                                   |      +--------------------------+
|                                                |
|   DMA buffer pool (streaming-mapped regions)   |
|   Transfer dependency tracker (fence/chain)    |
+------------------------------------------------+
```

## Scope

### Day 1 — Single-engine core
Build a working driver with one DMA engine. This is the foundation.

**Must implement:**
- `struct nic_dev` — per-device state: rings, doorbell, kthread, buffer pool
- **SQ/CQ rings** — allocated with `dma_alloc_coherent`, power-of-two depth,
  masked head/tail indices. SQ is driver-writable/device-readable, CQ is
  device-writable/driver-readable.
- **Doorbell** — driver writes an atomic counter; kthread wakes via
  `wait_event`/`wake_up`. Coalescing: one doorbell wakes device to drain
  all available SQEs.
- **DMA transfers** — kthread reads SQE, does `memcpy` (simulating DMA),
  posts CQE with status and cookie. Use `dma_map_single`/`dma_unmap_single`
  on data buffers with correct direction flags.
- **Completion notification** — kthread posts CQE then calls `wake_up` on a
  waitqueue. Driver-side `drain_completions()` returns completed CQEs.
- **Backpressure** — submit returns `-EAGAIN` when SQ is full.
- **Shutdown** — stop flag + final doorbell; kthread drains in-flight then exits.

**Data structures:**
```c
typedef enum { DMA_OP_TX, DMA_OP_RX } dma_op_t;

/* Submission queue entry — 32 bytes, fits half a cache line */
typedef struct {
    uint64_t    cookie;         /* echoed on completion */
    dma_addr_t  src;            /* source DMA address */
    dma_addr_t  dst;            /* destination DMA address */
    uint32_t    len;
    dma_op_t    op;
} __attribute__((packed)) sqe_t;

/* Completion queue entry — 16 bytes */
typedef struct {
    uint64_t    cookie;
    int32_t     status;         /* 0 = ok, negative = error */
    uint32_t    bytes_xferred;
} __attribute__((packed)) cqe_t;

/* Per-engine state */
typedef struct {
    sqe_t              *sq;             /* coherent DMA buffer */
    cqe_t              *cq;             /* coherent DMA buffer */
    dma_addr_t          sq_dma, cq_dma; /* device-visible addresses */
    uint32_t            sq_head, sq_tail;
    uint32_t            cq_head, cq_tail;
    uint32_t            depth, mask;    /* depth is power-of-two */
    atomic_t            doorbell;
    wait_queue_head_t   dev_wq;         /* device waits here */
    wait_queue_head_t   host_wq;        /* host waits for completions */
    struct task_struct *kthread;
    bool                stopping;
    spinlock_t          sq_lock;        /* protects sq_tail */
    spinlock_t          cq_lock;        /* protects cq_head */
} engine_t;
```

**Driver API (internal kernel API, not userspace):**
```c
/* Lifecycle */
engine_t *engine_create(struct device *dev, size_t depth);
void      engine_destroy(engine_t *eng);

/* Submit one descriptor. Returns 0 or -EAGAIN. */
int       engine_submit(engine_t *eng, const sqe_t *sqe);

/* Ring the doorbell — batches multiple submits into one wake. */
void      engine_doorbell(engine_t *eng);

/* Drain up to max completions. Returns count. */
size_t    engine_drain(engine_t *eng, cqe_t *out, size_t max);

/* Block until at least one completion is available, or timeout. */
int       engine_wait(engine_t *eng, unsigned long timeout_jiffies);
```

**Tests (Day 1):**
1. Single submit + doorbell + drain: verify cookie echo and status.
2. Fill SQ to capacity, verify `-EAGAIN` on overflow.
3. Batch: submit N descriptors, one doorbell, verify N completions.
4. Concurrent submitters: multiple kthreads submit under `sq_lock`, verify
   no lost descriptors.
5. Shutdown: submit work, stop engine, verify all in-flight complete before
   kthread exits.

### Day 2 — Multi-engine, chaining, peer-to-peer

**Multi-engine scheduling:**
```c
#define MAX_ENGINES 4

typedef struct {
    engine_t   *engines[MAX_ENGINES];
    int         nr_engines;
    atomic_t    next_engine;    /* round-robin counter */
} nic_dev_t;

/* Submit to least-loaded or round-robin engine */
int  nic_submit(nic_dev_t *dev, const sqe_t *sqe);
/* Doorbell all engines */
void nic_doorbell_all(nic_dev_t *dev);
/* Drain completions across all engines */
size_t nic_drain_all(nic_dev_t *dev, cqe_t *out, size_t max);
```

**Chained / dependent transfers:**
A collective like AllReduce requires ordered phases (e.g., reduce-scatter
then all-gather). The driver must express "start transfer B only after
transfer A completes."

```c
/* Fence: all prior SQEs on this engine must complete before subsequent ones start */
#define SQE_FLAG_FENCE  (1 << 0)

/* Chain: link this SQE to the next one — device processes them atomically */
#define SQE_FLAG_CHAIN  (1 << 1)

typedef struct {
    uint64_t    cookie;
    dma_addr_t  src;
    dma_addr_t  dst;
    uint32_t    len;
    dma_op_t    op;
    uint32_t    flags;          /* SQE_FLAG_FENCE, SQE_FLAG_CHAIN */
} sqe_v2_t;
```

The kthread respects these: `FENCE` means "wait for all outstanding CQEs
before processing this SQE." `CHAIN` means "process next SQE immediately
without waiting for a doorbell."

**Peer-to-peer (device-to-device) DMA:**
In real accelerator fabrics, data moves chip-to-chip without touching host
memory. Simulate this with a second engine whose "device memory" is a
separate kernel buffer. A P2P transfer reads from engine A's device buffer
and writes to engine B's device buffer.

```c
/* P2P: src is engine A's device-mem, dst is engine B's device-mem */
typedef struct {
    int         src_engine;
    int         dst_engine;
    uint32_t    src_offset;
    uint32_t    dst_offset;
    uint32_t    len;
    uint64_t    cookie;
} p2p_desc_t;

int nic_submit_p2p(nic_dev_t *dev, const p2p_desc_t *desc);
```

**Tests (Day 2):**
1. Multi-engine round-robin: submit 4N descriptors, verify ~N land on each engine.
2. Chained transfer: submit A (CHAIN) + B, verify B starts only after A completes,
   and both share one doorbell.
3. Fenced transfer: submit A, B (FENCE), C — verify C starts only after A and B
   both complete.
4. Peer-to-peer: write data to engine 0's device buffer via TX, P2P copy to
   engine 1, read back via RX on engine 1, verify data integrity.
5. Bandwidth saturation: submit max-depth SQEs across all engines with large
   buffers, measure aggregate throughput, verify it scales ~linearly with
   engine count.

## Memory ordering rules
| Operation | Barrier | Why |
|---|---|---|
| Driver writes SQE then advances tail | `dma_wmb()` | Device must see descriptor data before new tail |
| Driver reads CQE after checking new head | `dma_rmb()` | Must see completion data, not stale cache |
| Doorbell write | `wmb()` after tail store | Doorbell must not pass the tail update |
| Kthread reads SQE | `smp_load_acquire` on tail | Pairs with driver's release on tail |
| Kthread writes CQE then advances head | `smp_store_release` on head | Driver must see CQE before new head |

## Likely interview questions
1. What's the difference between `dma_alloc_coherent` and `dma_map_single`?
   When would you use each?
2. Why can't the driver just use `virt_to_phys()` to get a DMA address?
3. What is a doorbell register in PCIe? Why is it MMIO write-only?
4. Walk through the memory ordering from SQE write to doorbell to device processing.
   What breaks if you drop the `dma_wmb()`?
5. Why do real DMA engines (NVMe, mlx5, Neuron) use 64-byte descriptors?
6. How does descriptor chaining help collective operations like AllReduce?
7. How would you implement peer-to-peer DMA between two accelerators without
   bouncing through host memory?
8. What is interrupt coalescing and why does it matter for throughput?
9. How would you schedule work across multiple DMA engines to maximize
   aggregate bandwidth?
10. How does this SQ/CQ model map to Trainium's collective engine — what are
    the analogues for reduce-scatter and all-gather phases?
