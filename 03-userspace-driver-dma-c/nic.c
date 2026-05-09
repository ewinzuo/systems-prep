#include "nic.h"

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>

/* ========================================================================= */
/*  MMIO helper                                                              */
/* ========================================================================= */

static void mmio_write64(void __iomem *mmio, u32 reg_lo, u64 val)
{
    iowrite32((u32)val, mmio + reg_lo);
    iowrite32((u32)(val >> 32), mmio + reg_lo + 4);
}

/* ========================================================================= */
/*  IRQ handler                                                              */
/*                                                                           */
/*  Device raises MSI after writing CQEs. Acknowledge and wake waiters.      */
/*  Hint: read REG_IRQ_STATUS, write it to REG_IRQ_ACK, wake host_wq.       */
/* ========================================================================= */

static irqreturn_t dma_engine_irq(int irq, void *data)
{
    /* TODO: implement */
    return IRQ_NONE;
}

/* ========================================================================= */
/*  Engine API — YOUR CODE BELOW                                             */
/*                                                                           */
/*  No kthread needed. The device does all DMA processing.                   */
/*  The driver just writes SQEs, pokes the doorbell, and handles IRQs.       */
/*                                                                           */
/*  Useful functions:                                                        */
/*    pci_enable_device, pci_request_region, pci_iomap, pci_set_master       */
/*    dma_set_mask_and_coherent, dma_alloc_coherent, dma_free_coherent       */
/*    pci_alloc_irq_vectors, pci_irq_vector, request_irq, free_irq          */
/*    ioread32, iowrite32, mmio_write64 (above)                              */
/*    wmb, rmb                                                               */
/*    init_waitqueue_head, wait_event_interruptible_timeout                  */
/* ========================================================================= */

engine_t *engine_create(struct pci_dev *pdev, u32 sq_depth, u32 cq_depth)
{
    /* TODO:
     * 1. kzalloc engine_t
     * 2. PCI setup: enable, request region, iomap BAR0, set master
     * 3. dma_set_mask_and_coherent (64-bit)
     * 4. dma_alloc_coherent for SQ and CQ rings
     * 5. Write ring base addrs + depths to device registers
     * 6. Setup MSI: pci_alloc_irq_vectors + request_irq
     * 7. Init host_wq, sq_tail=0, cq_head=0
     * 8. Verify device ID: ioread32(mmio + REG_DEVICE_ID)
     *
     * Error handling: use goto chain to clean up on failure.
     */
    return NULL;
}

void engine_destroy(engine_t *eng)
{
    /* TODO:
     * 1. free_irq + pci_free_irq_vectors
     * 2. dma_free_coherent for SQ and CQ
     * 3. pci_clear_master, pci_iounmap, pci_release_region, pci_disable_device
     * 4. kfree(eng)
     */
}

int engine_submit(engine_t *eng, const sqe_t *sqe)
{
    /* TODO:
     * 1. Read REG_SQ_HEAD to check for backpressure
     * 2. If tail - head >= depth, return -EAGAIN
     * 3. Write SQE to sq[tail & (depth-1)]
     * 4. wmb() to ensure SQE is visible before tail advances
     * 5. Advance sq_tail
     */
    return -EAGAIN;
}

void engine_doorbell(engine_t *eng)
{
    /* TODO: single iowrite32 of sq_tail to REG_SQ_DOORBELL */
}

size_t engine_drain(engine_t *eng, cqe_t *out, size_t max)
{
    /* TODO:
     * 1. Read REG_CQ_TAIL from device
     * 2. rmb() before reading CQE data
     * 3. Copy CQEs from cq[head..tail] into out[]
     * 4. Advance cq_head
     */
    return 0;
}

int engine_wait(engine_t *eng, long timeout_jiffies)
{
    /* TODO: wait_event_interruptible_timeout on host_wq
     * Condition: REG_CQ_TAIL != cq_head */
    return -ETIMEDOUT;
}

/* ========================================================================= */
/*  PCI driver probe / remove                                                */
/*                                                                           */
/*  probe: create engine, run smoke test.                                    */
/*  The smoke test is provided — just implement the engine functions above.  */
/* ========================================================================= */

static int dma_engine_probe(struct pci_dev *pdev,
                            const struct pci_device_id *id)
{
    engine_t *eng;
    char *src, *dst;
    dma_addr_t src_dma, dst_dma;
    sqe_t sqe;
    cqe_t cqe;
    size_t n;
    int ret = 0;

    dev_info(&pdev->dev, "probing dma_engine device\n");

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

    src_dma = dma_map_single(&pdev->dev, src, 64, DMA_TO_DEVICE);
    if (dma_mapping_error(&pdev->dev, src_dma)) {
        ret = -ENOMEM;
        goto test_done;
    }

    dst_dma = dma_map_single(&pdev->dev, dst, 64, DMA_FROM_DEVICE);
    if (dma_mapping_error(&pdev->dev, dst_dma)) {
        dma_unmap_single(&pdev->dev, src_dma, 64, DMA_TO_DEVICE);
        ret = -ENOMEM;
        goto test_done;
    }

    /* TX: host -> device */
    sqe = (sqe_t){
        .cookie = 0xCAFE,
        .buf_addr = src_dma,
        .len = 18,
        .op = DMA_OP_TX,
    };
    engine_submit(eng, &sqe);
    engine_doorbell(eng);
    if (engine_wait(eng, 2 * HZ) != 0) {
        dev_err(&pdev->dev, "TX timed out\n");
        ret = -ETIMEDOUT;
        goto test_unmap;
    }
    n = engine_drain(eng, &cqe, 1);
    if (n != 1 || cqe.status != 0) {
        dev_err(&pdev->dev, "TX bad: n=%zu status=%d\n", n, cqe.status);
        ret = -EIO;
        goto test_unmap;
    }

    dma_unmap_single(&pdev->dev, src_dma, 64, DMA_TO_DEVICE);
    src_dma = 0;

    /* RX: device -> host */
    sqe = (sqe_t){
        .cookie = 0xBEEF,
        .buf_addr = dst_dma,
        .len = 18,
        .op = DMA_OP_RX,
    };
    engine_submit(eng, &sqe);
    engine_doorbell(eng);
    if (engine_wait(eng, 2 * HZ) != 0) {
        dev_err(&pdev->dev, "RX timed out\n");
        ret = -ETIMEDOUT;
        goto test_unmap;
    }
    n = engine_drain(eng, &cqe, 1);
    if (n != 1 || cqe.status != 0) {
        dev_err(&pdev->dev, "RX bad: n=%zu status=%d\n", n, cqe.status);
        ret = -EIO;
        goto test_unmap;
    }

    dma_unmap_single(&pdev->dev, dst_dma, 64, DMA_FROM_DEVICE);
    dst_dma = 0;

    if (memcmp(src, dst, 18) != 0) {
        dev_err(&pdev->dev, "DATA MISMATCH: src='%s' dst='%s'\n", src, dst);
        ret = -EIO;
        goto test_done;
    }

    dev_info(&pdev->dev,
             "smoke test PASSED — data roundtripped through DMA engine\n");
    goto test_done;

test_unmap:
    if (src_dma)
        dma_unmap_single(&pdev->dev, src_dma, 64, DMA_TO_DEVICE);
    if (dst_dma)
        dma_unmap_single(&pdev->dev, dst_dma, 64, DMA_FROM_DEVICE);
test_done:
    kfree(src);
    kfree(dst);

    if (ret) {
        engine_destroy(eng);
        pci_set_drvdata(pdev, NULL);
    }

    return ret;
}

static void dma_engine_remove(struct pci_dev *pdev)
{
    engine_t *eng = pci_get_drvdata(pdev);

    engine_destroy(eng);
    dev_info(&pdev->dev, "dma_engine removed\n");
}

static struct pci_device_id dma_engine_pci_ids[] = {
    { PCI_DEVICE(DMA_ENGINE_VENDOR_ID, DMA_ENGINE_DEVICE_ID) },
    { 0 },
};
MODULE_DEVICE_TABLE(pci, dma_engine_pci_ids);

static struct pci_driver dma_engine_pci_driver = {
    .name       = "dma_engine",
    .id_table   = dma_engine_pci_ids,
    .probe      = dma_engine_probe,
    .remove     = dma_engine_remove,
};

static int __init nic_init(void)
{
    return pci_register_driver(&dma_engine_pci_driver);
}

static void __exit nic_exit(void)
{
    pci_unregister_driver(&dma_engine_pci_driver);
}

module_init(nic_init);
module_exit(nic_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kernel driver for custom DMA engine with SQ/CQ rings");
