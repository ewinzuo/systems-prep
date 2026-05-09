#ifndef NIC_H
#define NIC_H

#include <linux/types.h>
#include <linux/wait.h>
#include <linux/pci.h>

/* --- DMA engine device register offsets ---------------------------------- */
/*                                                                           */
/* These match the QEMU dma_engine device (vm/dma_engine.c).                 */
/* All registers are 32-bit. 64-bit addresses use two consecutive regs.      */

#define REG_DEVICE_ID       0x00    /* RO: magic 0xDEA10001 */
#define REG_DEVICE_STATUS   0x04    /* RO: bit 0 = ready */
#define REG_SQ_BASE_LO      0x08    /* RW: SQ ring DMA addr low */
#define REG_SQ_BASE_HI      0x0C    /* RW: SQ ring DMA addr high */
#define REG_CQ_BASE_LO      0x10    /* RW: CQ ring DMA addr low */
#define REG_CQ_BASE_HI      0x14    /* RW: CQ ring DMA addr high */
#define REG_SQ_DEPTH        0x18    /* RW: number of SQ slots (power of 2) */
#define REG_CQ_DEPTH        0x1C    /* RW: number of CQ slots (power of 2) */
#define REG_SQ_DOORBELL     0x20    /* WO: write new SQ tail to kick device */
#define REG_IRQ_STATUS      0x24    /* RO: interrupt status bits */
#define REG_IRQ_ACK         0x28    /* WO: write to clear IRQ */
#define REG_SQ_HEAD         0x2C    /* RO: device's SQ consumer index */
#define REG_CQ_TAIL         0x30    /* RO: device's CQ producer index */

#define DEVICE_ID_MAGIC     0xDEA10001
#define DEV_BUF_SIZE        4096

/* PCI IDs */
#define DMA_ENGINE_VENDOR_ID  0x1234
#define DMA_ENGINE_DEVICE_ID  0xdea1

/* --- Descriptor types ---------------------------------------------------- */
/*                                                                           */
/* These structs are shared between the kernel driver and the QEMU device.   */
/* The device DMA-reads SQEs and DMA-writes CQEs, so layouts must match.     */

typedef enum { DMA_OP_TX = 1, DMA_OP_RX = 2 } dma_op_t;

/* Submission queue entry — driver -> device (24 bytes, packed) */
typedef struct {
    u64         cookie;     /* driver-defined; echoed on completion */
    u64         buf_addr;   /* DMA address of data buffer */
    u32         len;        /* transfer length (<= DEV_BUF_SIZE) */
    u32         op;         /* DMA_OP_TX or DMA_OP_RX */
} __packed sqe_t;

/* Completion queue entry — device -> driver (16 bytes, packed) */
typedef struct {
    u64         cookie;
    s32         status;         /* 0 = ok, negative = error */
    u32         bytes_xferred;
} __packed cqe_t;

/* --- Engine -------------------------------------------------------------- */
/*                                                                           */
/* No kthread, no SPSC ring helpers. The device does all DMA processing.     */
/* The driver just writes SQEs into coherent memory and pokes the doorbell.  */

typedef struct {
    /* SQ ring (allocated with dma_alloc_coherent) */
    sqe_t          *sq;         /* CPU virtual address */
    dma_addr_t      sq_dma;     /* device-visible DMA address */
    u32             sq_depth;
    u32             sq_tail;    /* driver's next write slot */

    /* CQ ring (allocated with dma_alloc_coherent) */
    cqe_t          *cq;         /* CPU virtual address */
    dma_addr_t      cq_dma;     /* device-visible DMA address */
    u32             cq_depth;
    u32             cq_head;    /* driver's next read slot */

    /* PCI / MMIO */
    struct pci_dev *pdev;
    void __iomem   *mmio;

    /* Completion notification */
    wait_queue_head_t host_wq;
} engine_t;

/* --- API ----------------------------------------------------------------- */

engine_t *engine_create(struct pci_dev *pdev, u32 sq_depth, u32 cq_depth);
void      engine_destroy(engine_t *eng);

/* Write SQE into SQ ring. Returns 0 on success, -EAGAIN if full.
 * Caller must dma_map_single the data buffer first and pass the
 * resulting dma_addr_t in sqe->buf_addr. */
int       engine_submit(engine_t *eng, const sqe_t *sqe);

/* Write SQ tail to doorbell register — one MMIO write.
 * Coalesces: submit N then doorbell once. */
void      engine_doorbell(engine_t *eng);

/* Read completed CQEs from CQ ring. Returns count copied.
 * Caller must dma_unmap_single the data buffers after processing. */
size_t    engine_drain(engine_t *eng, cqe_t *out, size_t max);

/* Block until at least one CQE is available, or timeout.
 * timeout_jiffies < 0 means wait forever. */
int       engine_wait(engine_t *eng, long timeout_jiffies);

#endif /* NIC_H */
