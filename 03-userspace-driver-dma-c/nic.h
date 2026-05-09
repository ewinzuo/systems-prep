#ifndef NIC_H
#define NIC_H

#include <linux/types.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/atomic.h>

/* --- Descriptor types ---------------------------------------------------- */

typedef enum { DMA_OP_TX = 1, DMA_OP_RX = 2 } dma_op_t;

/* Submission queue entry — host -> device */
typedef struct {
    u64         cookie;     /* host-defined; echoed on completion */
    void       *src;        /* source buffer (kernel virtual address) */
    void       *dst;        /* destination buffer */
    u32         len;        /* transfer length in bytes */
    dma_op_t    op;
} sqe_t;

/* Completion queue entry — device -> host */
typedef struct {
    u64         cookie;
    s32         status;         /* 0 = ok, negative = error */
    u32         bytes_xferred;
} cqe_t;

/* --- SPSC ring (kernel-space, no libc) ----------------------------------- */

struct spsc_ring {
    char   *buf;
    u32     elem_size;
    u32     depth;
    u32     mask;       /* depth - 1 */
    u32     head;       /* consumer reads here, advances after read */
    u32     tail;       /* producer writes here, advances after write */
};

/* --- Engine (single DMA engine) ------------------------------------------ */

typedef struct {
    struct spsc_ring    sq;         /* SQE ring: host pushes, device pops */
    struct spsc_ring    cq;         /* CQE ring: device pushes, host pops */

    atomic_t            doorbell;   /* host increments to wake device */

    wait_queue_head_t   dev_wq;     /* device thread sleeps here */
    wait_queue_head_t   host_wq;    /* host waits for completions */

    struct task_struct *dev_thread;
} engine_t;

/* --- API ----------------------------------------------------------------- */

engine_t *engine_create(u32 sq_depth, u32 cq_depth);
void      engine_destroy(engine_t *eng);
int       engine_submit(engine_t *eng, const sqe_t *sqe);
void      engine_doorbell(engine_t *eng);
size_t    engine_drain(engine_t *eng, cqe_t *out, size_t max);
int       engine_wait(engine_t *eng, long timeout_jiffies);

#endif /* NIC_H */
