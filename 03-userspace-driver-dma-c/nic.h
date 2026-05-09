#ifndef NIC_H
#define NIC_H

#include <linux/types.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/pci.h>

/* --- EDU device register offsets ----------------------------------------- */

#define EDU_REG_ID          0x00    /* RO: device identity */
#define EDU_REG_LIVENESS    0x04    /* RW: write X, read ~X */
#define EDU_REG_STATUS      0x20    /* RW: status flags */
#define EDU_REG_IRQ_STATUS  0x24    /* RO: interrupt status */
#define EDU_REG_IRQ_RAISE   0x60    /* WO: raise interrupt */
#define EDU_REG_IRQ_ACK     0x64    /* WO: acknowledge interrupt */

#define EDU_REG_DMA_SRC     0x80    /* RW 8B: DMA source address */
#define EDU_REG_DMA_DST     0x88    /* RW 8B: DMA destination address */
#define EDU_REG_DMA_COUNT   0x90    /* RW 8B: transfer byte count */
#define EDU_REG_DMA_CMD     0x98    /* RW 8B: DMA command register */

/* DMA command bits */
#define EDU_DMA_START       0x01    /* start transfer */
#define EDU_DMA_DIR         0x02    /* 0 = RAM->EDU, 1 = EDU->RAM */
#define EDU_DMA_IRQ         0x04    /* raise IRQ on completion */

/* Device internal DMA buffer */
#define EDU_BUF_OFFSET      0x40000
#define EDU_BUF_SIZE        4096

/* PCI IDs */
#define EDU_VENDOR_ID       0x1234
#define EDU_DEVICE_ID       0x11e8

/* Interrupt status bit for DMA completion */
#define EDU_IRQ_DMA_BIT     0x100

/* --- Descriptor types ---------------------------------------------------- */

typedef enum { DMA_OP_TX = 1, DMA_OP_RX = 2 } dma_op_t;

/* Submission queue entry — host -> device */
typedef struct {
    u64         cookie;     /* host-defined; echoed on completion */
    void       *buf;        /* TX: source in host RAM; RX: destination */
    u32         len;        /* transfer length (<= EDU_BUF_SIZE) */
    dma_op_t    op;
} sqe_t;

/* Completion queue entry — device -> host */
typedef struct {
    u64         cookie;
    s32         status;         /* 0 = ok, negative = error */
    u32         bytes_xferred;
} cqe_t;

/* --- SPSC ring (kernel-space) -------------------------------------------- */

struct spsc_ring {
    char       *buf;        /* CPU virtual address (from dma_alloc_coherent) */
    dma_addr_t  dma_handle; /* device-visible DMA address */
    u32         elem_size;
    u32         depth;
    u32         mask;
    u32         head;       /* consumer reads here */
    u32         tail;       /* producer writes here */
};

/* --- Engine (single DMA engine backed by QEMU edu device) ---------------- */

typedef struct {
    struct spsc_ring    sq;         /* SQE ring: host pushes, device pops */
    struct spsc_ring    cq;         /* CQE ring: device pushes, host pops */

    atomic_t            doorbell;   /* host increments to wake device */

    wait_queue_head_t   dev_wq;     /* device thread sleeps here */
    wait_queue_head_t   host_wq;    /* host waits for completions */
    wait_queue_head_t   dma_wq;     /* device thread waits for DMA IRQ */

    struct task_struct *dev_thread;
    struct pci_dev     *pdev;       /* PCI device handle */
    void __iomem       *mmio;       /* BAR0 mapped address */
    atomic_t            dma_done;   /* set by IRQ handler */
} engine_t;

/* --- API ----------------------------------------------------------------- */

engine_t *engine_create(struct pci_dev *pdev, u32 sq_depth, u32 cq_depth);
void      engine_destroy(engine_t *eng);
int       engine_submit(engine_t *eng, const sqe_t *sqe);
void      engine_doorbell(engine_t *eng);
size_t    engine_drain(engine_t *eng, cqe_t *out, size_t max);
int       engine_wait(engine_t *eng, long timeout_jiffies);

#endif /* NIC_H */
