#include "nic.h"

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>

/* ========================================================================= */
/*  SPSC ring helpers (same as memcpy version — ring logic doesn't change)   */
/* ========================================================================= */

static int ring_init(struct spsc_ring *r, u32 depth, u32 elem_size)
{
    if (depth == 0 || (depth & (depth - 1)) != 0 || elem_size == 0)
        return -EINVAL;

    r->buf = kmalloc_array(depth, elem_size, GFP_KERNEL);
    if (!r->buf)
        return -ENOMEM;

    r->elem_size = elem_size;
    r->depth = depth;
    r->mask = depth - 1;
    r->head = 0;
    r->tail = 0;
    return 0;
}

static void ring_destroy(struct spsc_ring *r)
{
    kfree(r->buf);
    r->buf = NULL;
}

static int ring_push(struct spsc_ring *r, const void *elem)
{
    u32 tail = r->tail;
    u32 head = smp_load_acquire(&r->head);

    if (tail - head >= r->depth)
        return -1;

    memcpy(r->buf + (tail & r->mask) * r->elem_size, elem, r->elem_size);
    smp_store_release(&r->tail, tail + 1);
    return 0;
}

static int ring_pop(struct spsc_ring *r, void *elem)
{
    u32 head = r->head;
    u32 tail = smp_load_acquire(&r->tail);

    if (head == tail)
        return -1;

    memcpy(elem, r->buf + (head & r->mask) * r->elem_size, r->elem_size);
    smp_store_release(&r->head, head + 1);
    return 0;
}

static u32 ring_pop_n(struct spsc_ring *r, void *elems, u32 max)
{
    u32 head = r->head;
    u32 tail = smp_load_acquire(&r->tail);
    u32 avail = tail - head;
    u32 n = min(avail, max);
    u32 i;

    for (i = 0; i < n; i++) {
        memcpy((char *)elems + i * r->elem_size,
               r->buf + ((head + i) & r->mask) * r->elem_size,
               r->elem_size);
    }

    smp_store_release(&r->head, head + n);
    return n;
}

static bool ring_has_data(struct spsc_ring *r)
{
    return r->head != smp_load_acquire(&r->tail);
}

/* ========================================================================= */
/*  MMIO helpers                                                             */
/* ========================================================================= */

/* Write a 64-bit value to an edu register using two 32-bit writes.
 * The edu device accepts both 4-byte and 8-byte accesses for regs >= 0x80. */
static void edu_write64(void __iomem *mmio, u32 reg, u64 val)
{
    iowrite32((u32)val, mmio + reg);
    iowrite32((u32)(val >> 32), mmio + reg + 4);
}

/* ========================================================================= */
/*  IRQ handler                                                              */
/*                                                                           */
/*  In real hardware this would be an MSI-X interrupt from the DMA engine.   */
/*  The edu device raises an interrupt when DMA completes (if DMA_IRQ set).  */
/*  We read the status register to see what fired, acknowledge it, and       */
/*  wake the device thread that's waiting for the transfer to finish.        */
/* ========================================================================= */

static irqreturn_t edu_irq_handler(int irq, void *data)
{
    engine_t *eng = data;
    u32 status;

    status = ioread32(eng->mmio + EDU_REG_IRQ_STATUS);
    if (!status)
        return IRQ_NONE;

    /* Acknowledge all pending interrupts */
    iowrite32(status, eng->mmio + EDU_REG_IRQ_ACK);

    /* If DMA completed, wake the device thread */
    if (status & EDU_IRQ_DMA_BIT) {
        atomic_set(&eng->dma_done, 1);
        wake_up_interruptible(&eng->dma_wq);
    }

    return IRQ_HANDLED;
}

/* ========================================================================= */
/*  DMA transfer via edu device                                              */
/*                                                                           */
/*  This is the core difference from the memcpy version. Instead of          */
/*  memcpy(dst, src, len), we:                                               */
/*    1. Map the host buffer for DMA (dma_map_single)                        */
/*    2. Program the edu device's DMA registers via MMIO                     */
/*    3. Start the transfer — device reads/writes host RAM autonomously      */
/*    4. Wait for the IRQ that signals completion                            */
/*    5. Unmap the buffer                                                    */
/* ========================================================================= */

static int do_dma_transfer(engine_t *eng, const sqe_t *sqe)
{
    dma_addr_t buf_dma;
    enum dma_data_direction dir;
    u32 cmd;
    long ret;

    if (sqe->len > EDU_BUF_SIZE)
        return -EINVAL;

    /* Step 1: Map host buffer for DMA.
     * This gives us a dma_addr_t that the device can use to access
     * host RAM. On systems with an IOMMU, this is an IOVA, not a
     * physical address. */
    dir = (sqe->op == DMA_OP_TX) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
    buf_dma = dma_map_single(&eng->pdev->dev, sqe->buf, sqe->len, dir);
    if (dma_mapping_error(&eng->pdev->dev, buf_dma))
        return -ENOMEM;

    /* Clear completion flag before starting */
    atomic_set(&eng->dma_done, 0);

    /* Step 2: Program edu DMA registers via MMIO.
     * TX (host -> device): src = host RAM, dst = device buffer
     * RX (device -> host): src = device buffer, dst = host RAM */
    if (sqe->op == DMA_OP_TX) {
        edu_write64(eng->mmio, EDU_REG_DMA_SRC, buf_dma);
        edu_write64(eng->mmio, EDU_REG_DMA_DST, EDU_BUF_OFFSET);
        cmd = EDU_DMA_START | EDU_DMA_IRQ;
    } else {
        edu_write64(eng->mmio, EDU_REG_DMA_SRC, EDU_BUF_OFFSET);
        edu_write64(eng->mmio, EDU_REG_DMA_DST, buf_dma);
        cmd = EDU_DMA_START | EDU_DMA_DIR | EDU_DMA_IRQ;
    }

    edu_write64(eng->mmio, EDU_REG_DMA_COUNT, sqe->len);

    /* Step 3: Start the DMA — device now reads/writes host RAM
     * over the (virtual) PCIe bus. CPU is free. */
    edu_write64(eng->mmio, EDU_REG_DMA_CMD, cmd);

    /* Step 4: Wait for completion IRQ */
    ret = wait_event_interruptible_timeout(eng->dma_wq,
        atomic_read(&eng->dma_done), 5 * HZ);

    /* Step 5: Unmap the buffer (flushes caches on non-coherent archs) */
    dma_unmap_single(&eng->pdev->dev, buf_dma, sqe->len, dir);

    if (ret <= 0) {
        dev_err(&eng->pdev->dev, "DMA transfer timed out\n");
        return -ETIMEDOUT;
    }

    return 0;
}

/* ========================================================================= */
/*  Device thread                                                            */
/*                                                                           */
/*  Translates SQ descriptors into edu DMA register writes.                  */
/*  This is what a real Trainium driver does: read a descriptor from the     */
/*  submission ring, program the hardware DMA engine, wait for completion,   */
/*  post a CQE.                                                             */
/* ========================================================================= */

static void drain_sq(engine_t *eng)
{
    sqe_t sqe;

    while (ring_pop(&eng->sq, &sqe) == 0) {
        cqe_t cqe;
        int status;

        status = do_dma_transfer(eng, &sqe);

        cqe.cookie = sqe.cookie;
        cqe.status = status;
        cqe.bytes_xferred = (status == 0) ? sqe.len : 0;

        if (ring_push(&eng->cq, &cqe) != 0)
            dev_warn(&eng->pdev->dev,
                     "CQ full, dropped cookie=%llu\n", sqe.cookie);
    }

    wake_up_interruptible(&eng->host_wq);
}

static int device_thread_fn(void *data)
{
    engine_t *eng = data;
    int last_bell = 0;

    while (!kthread_should_stop()) {
        wait_event_interruptible(eng->dev_wq,
            atomic_read(&eng->doorbell) != last_bell ||
            kthread_should_stop());

        last_bell = atomic_read(&eng->doorbell);
        drain_sq(eng);
    }

    /* Final drain after stop */
    drain_sq(eng);
    return 0;
}

/* ========================================================================= */
/*  Engine API                                                               */
/* ========================================================================= */

engine_t *engine_create(struct pci_dev *pdev, u32 sq_depth, u32 cq_depth)
{
    engine_t *eng;
    u32 id;
    int ret;

    eng = kzalloc(sizeof(*eng), GFP_KERNEL);
    if (!eng)
        return NULL;

    eng->pdev = pdev;

    /* --- PCI setup --- */

    ret = pci_enable_device(pdev);
    if (ret)
        goto err_free;

    ret = pci_request_region(pdev, 0, "nic_edu");
    if (ret)
        goto err_disable;

    eng->mmio = pci_iomap(pdev, 0, 0);
    if (!eng->mmio) {
        ret = -ENOMEM;
        goto err_region;
    }

    /* Enable bus mastering: allows the device to initiate DMA reads/writes
     * to host memory. Without this, the device can't do DMA. */
    pci_set_master(pdev);

    /* Set DMA mask: edu device supports 28-bit addresses (256 MB) */
    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(28));
    if (ret)
        goto err_unmap;

    /* Register IRQ handler for DMA completion interrupts */
    ret = request_irq(pdev->irq, edu_irq_handler, IRQF_SHARED,
                      "nic_edu", eng);
    if (ret)
        goto err_unmap;

    /* Verify device identity */
    id = ioread32(eng->mmio + EDU_REG_ID);
    dev_info(&pdev->dev, "edu device v%d.%d\n",
             (id >> 24) & 0xff, (id >> 16) & 0xff);

    /* --- Ring + thread setup (same as memcpy version) --- */

    ret = ring_init(&eng->sq, sq_depth, sizeof(sqe_t));
    if (ret)
        goto err_irq;

    ret = ring_init(&eng->cq, cq_depth, sizeof(cqe_t));
    if (ret)
        goto err_sq;

    atomic_set(&eng->doorbell, 0);
    atomic_set(&eng->dma_done, 0);
    init_waitqueue_head(&eng->dev_wq);
    init_waitqueue_head(&eng->host_wq);
    init_waitqueue_head(&eng->dma_wq);

    eng->dev_thread = kthread_run(device_thread_fn, eng, "nic_dev");
    if (IS_ERR(eng->dev_thread))
        goto err_cq;

    return eng;

err_cq:
    ring_destroy(&eng->cq);
err_sq:
    ring_destroy(&eng->sq);
err_irq:
    free_irq(pdev->irq, eng);
err_unmap:
    pci_clear_master(pdev);
    pci_iounmap(pdev, eng->mmio);
err_region:
    pci_release_region(pdev, 0);
err_disable:
    pci_disable_device(pdev);
err_free:
    kfree(eng);
    return NULL;
}

void engine_destroy(engine_t *eng)
{
    if (!eng)
        return;

    kthread_stop(eng->dev_thread);

    ring_destroy(&eng->sq);
    ring_destroy(&eng->cq);

    free_irq(eng->pdev->irq, eng);
    pci_clear_master(eng->pdev);
    pci_iounmap(eng->pdev, eng->mmio);
    pci_release_region(eng->pdev, 0);
    pci_disable_device(eng->pdev);

    kfree(eng);
}

int engine_submit(engine_t *eng, const sqe_t *sqe)
{
    return ring_push(&eng->sq, sqe);
}

void engine_doorbell(engine_t *eng)
{
    atomic_inc(&eng->doorbell);
    wake_up_interruptible(&eng->dev_wq);
}

size_t engine_drain(engine_t *eng, cqe_t *out, size_t max)
{
    return ring_pop_n(&eng->cq, out, (u32)max);
}

int engine_wait(engine_t *eng, long timeout_jiffies)
{
    long ret;

    if (timeout_jiffies < 0) {
        wait_event_interruptible(eng->host_wq,
            ring_has_data(&eng->cq));
        return 0;
    }

    ret = wait_event_interruptible_timeout(eng->host_wq,
        ring_has_data(&eng->cq), timeout_jiffies);
    return ret > 0 ? 0 : -ETIMEDOUT;
}

/* ========================================================================= */
/*  PCI driver: probe / remove                                               */
/*                                                                           */
/*  probe is called by the kernel when it finds a PCI device matching our    */
/*  vendor/device ID. We create the engine, run a smoke test, and leave      */
/*  the engine running. remove is called on rmmod.                           */
/* ========================================================================= */

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    engine_t *eng;
    char *src, *dst;
    sqe_t sqe;
    cqe_t cqe;
    size_t n;
    int ret = 0;

    dev_info(&pdev->dev, "nic: probing edu device\n");

    eng = engine_create(pdev, 16, 16);
    if (!eng)
        return -ENOMEM;

    pci_set_drvdata(pdev, eng);

    /* --- Smoke test: TX to device, RX back, compare --- */

    src = kmalloc(64, GFP_KERNEL);
    dst = kmalloc(64, GFP_KERNEL);
    if (!src || !dst) {
        ret = -ENOMEM;
        goto test_done;
    }

    memcpy(src, "hello DMA engine!", 18);
    memset(dst, 0, 64);

    /* TX: upload src to edu device buffer */
    sqe.cookie = 0xCAFE;
    sqe.buf = src;
    sqe.len = 18;
    sqe.op = DMA_OP_TX;

    if (engine_submit(eng, &sqe) != 0) {
        dev_err(&pdev->dev, "TX submit failed\n");
        ret = -EIO;
        goto test_done;
    }
    engine_doorbell(eng);
    if (engine_wait(eng, 2 * HZ) != 0) {
        dev_err(&pdev->dev, "TX timed out\n");
        ret = -ETIMEDOUT;
        goto test_done;
    }
    n = engine_drain(eng, &cqe, 1);
    if (n != 1 || cqe.status != 0) {
        dev_err(&pdev->dev, "TX bad: n=%zu status=%d\n", n, cqe.status);
        ret = -EIO;
        goto test_done;
    }

    /* RX: download from edu device buffer into dst */
    sqe.cookie = 0xBEEF;
    sqe.buf = dst;
    sqe.len = 18;
    sqe.op = DMA_OP_RX;

    if (engine_submit(eng, &sqe) != 0) {
        dev_err(&pdev->dev, "RX submit failed\n");
        ret = -EIO;
        goto test_done;
    }
    engine_doorbell(eng);
    if (engine_wait(eng, 2 * HZ) != 0) {
        dev_err(&pdev->dev, "RX timed out\n");
        ret = -ETIMEDOUT;
        goto test_done;
    }
    n = engine_drain(eng, &cqe, 1);
    if (n != 1 || cqe.status != 0) {
        dev_err(&pdev->dev, "RX bad: n=%zu status=%d\n", n, cqe.status);
        ret = -EIO;
        goto test_done;
    }

    /* Verify data roundtripped through the device */
    if (memcmp(src, dst, 18) != 0) {
        dev_err(&pdev->dev, "DATA MISMATCH: src='%s' dst='%s'\n", src, dst);
        ret = -EIO;
        goto test_done;
    }

    dev_info(&pdev->dev, "smoke test PASSED — data roundtripped through edu DMA\n");

test_done:
    kfree(src);
    kfree(dst);

    if (ret) {
        engine_destroy(eng);
        pci_set_drvdata(pdev, NULL);
    }

    return ret;
}

static void edu_remove(struct pci_dev *pdev)
{
    engine_t *eng = pci_get_drvdata(pdev);

    engine_destroy(eng);
    dev_info(&pdev->dev, "nic: removed\n");
}

/* PCI device table: tells the kernel which devices this driver handles */
static struct pci_device_id edu_pci_ids[] = {
    { PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
    { 0 },
};
MODULE_DEVICE_TABLE(pci, edu_pci_ids);

static struct pci_driver edu_pci_driver = {
    .name       = "nic_edu",
    .id_table   = edu_pci_ids,
    .probe      = edu_probe,
    .remove     = edu_remove,
};

/* module_init registers the PCI driver; probe fires when edu device found */
static int __init nic_init(void)
{
    return pci_register_driver(&edu_pci_driver);
}

static void __exit nic_exit(void)
{
    pci_unregister_driver(&edu_pci_driver);
}

module_init(nic_init);
module_exit(nic_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DMA engine driver for QEMU edu device");
