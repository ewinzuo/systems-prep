#include "nic.h"

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>

/* ========================================================================= */
/*  MMIO helpers                                                             */
/* ========================================================================= */

static void mmio_write64(void __iomem *mmio, u32 reg_lo, u64 val)
{
    iowrite32((u32)val, mmio + reg_lo);
    iowrite32((u32)(val >> 32), mmio + reg_lo + 4);
}

/* ========================================================================= */
/*  IRQ handler                                                              */
/*                                                                           */
/*  Device raises MSI after writing CQEs to the CQ ring. All we do is       */
/*  acknowledge and wake anyone waiting for completions.                     */
/* ========================================================================= */

static irqreturn_t dma_engine_irq(int irq, void *data)
{
    engine_t *eng = data;
    u32 status;

    status = ioread32(eng->mmio + REG_IRQ_STATUS);
    if (!status)
        return IRQ_NONE;

    iowrite32(status, eng->mmio + REG_IRQ_ACK);
    wake_up_interruptible(&eng->host_wq);

    return IRQ_HANDLED;
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
    eng->sq_depth = sq_depth;
    eng->cq_depth = cq_depth;

    /* --- PCI setup --- */

    ret = pci_enable_device(pdev);
    if (ret)
        goto err_free;

    ret = pci_request_region(pdev, 0, "dma_engine");
    if (ret)
        goto err_disable;

    eng->mmio = pci_iomap(pdev, 0, 0);
    if (!eng->mmio) {
        ret = -ENOMEM;
        goto err_region;
    }

    pci_set_master(pdev);

    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
    if (ret)
        goto err_unmap;

    /* --- Allocate SQ/CQ rings in DMA-coherent memory --- */

    eng->sq = dma_alloc_coherent(&pdev->dev,
                                 sq_depth * sizeof(sqe_t),
                                 &eng->sq_dma, GFP_KERNEL);
    if (!eng->sq)
        goto err_unmap;

    eng->cq = dma_alloc_coherent(&pdev->dev,
                                 cq_depth * sizeof(cqe_t),
                                 &eng->cq_dma, GFP_KERNEL);
    if (!eng->cq)
        goto err_sq;

    /* --- Program device registers with ring addresses --- */

    mmio_write64(eng->mmio, REG_SQ_BASE_LO, eng->sq_dma);
    mmio_write64(eng->mmio, REG_CQ_BASE_LO, eng->cq_dma);
    iowrite32(sq_depth, eng->mmio + REG_SQ_DEPTH);
    iowrite32(cq_depth, eng->mmio + REG_CQ_DEPTH);

    /* --- MSI interrupt setup --- */

    ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_INTX);
    if (ret < 0)
        goto err_cq;

    ret = request_irq(pci_irq_vector(pdev, 0), dma_engine_irq, 0,
                      "dma_engine", eng);
    if (ret)
        goto err_irq_vec;

    /* --- Init driver state --- */

    eng->sq_tail = 0;
    eng->cq_head = 0;
    init_waitqueue_head(&eng->host_wq);

    /* Verify device identity */
    id = ioread32(eng->mmio + REG_DEVICE_ID);
    dev_info(&pdev->dev, "dma_engine device id=0x%08x\n", id);

    return eng;

err_irq_vec:
    pci_free_irq_vectors(pdev);
err_cq:
    dma_free_coherent(&pdev->dev, cq_depth * sizeof(cqe_t),
                      eng->cq, eng->cq_dma);
err_sq:
    dma_free_coherent(&pdev->dev, sq_depth * sizeof(sqe_t),
                      eng->sq, eng->sq_dma);
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

    free_irq(pci_irq_vector(eng->pdev, 0), eng);
    pci_free_irq_vectors(eng->pdev);

    dma_free_coherent(&eng->pdev->dev,
                      eng->sq_depth * sizeof(sqe_t),
                      eng->sq, eng->sq_dma);
    dma_free_coherent(&eng->pdev->dev,
                      eng->cq_depth * sizeof(cqe_t),
                      eng->cq, eng->cq_dma);

    pci_clear_master(eng->pdev);
    pci_iounmap(eng->pdev, eng->mmio);
    pci_release_region(eng->pdev, 0);
    pci_disable_device(eng->pdev);

    kfree(eng);
}

int engine_submit(engine_t *eng, const sqe_t *sqe)
{
    u32 sq_head = ioread32(eng->mmio + REG_SQ_HEAD);
    u32 tail = eng->sq_tail;

    /* Check if SQ is full */
    if (tail - sq_head >= eng->sq_depth)
        return -EAGAIN;

    /* Write SQE into coherent DMA memory */
    eng->sq[tail & (eng->sq_depth - 1)] = *sqe;

    /* Ensure SQE is visible in memory before we advance tail */
    wmb();

    eng->sq_tail = tail + 1;
    return 0;
}

void engine_doorbell(engine_t *eng)
{
    /* Single MMIO write — device wakes up and DMA-reads the SQEs */
    iowrite32(eng->sq_tail, eng->mmio + REG_SQ_DOORBELL);
}

size_t engine_drain(engine_t *eng, cqe_t *out, size_t max)
{
    u32 cq_tail = ioread32(eng->mmio + REG_CQ_TAIL);
    u32 head = eng->cq_head;
    u32 avail = cq_tail - head;
    u32 n = min_t(u32, avail, (u32)max);
    u32 i;

    /* Read CQE data before advancing head */
    rmb();

    for (i = 0; i < n; i++)
        out[i] = eng->cq[(head + i) & (eng->cq_depth - 1)];

    eng->cq_head = head + n;
    return n;
}

int engine_wait(engine_t *eng, long timeout_jiffies)
{
    long ret;

    if (timeout_jiffies < 0) {
        wait_event_interruptible(eng->host_wq,
            ioread32(eng->mmio + REG_CQ_TAIL) != eng->cq_head);
        return 0;
    }

    ret = wait_event_interruptible_timeout(eng->host_wq,
        ioread32(eng->mmio + REG_CQ_TAIL) != eng->cq_head,
        timeout_jiffies);
    return ret > 0 ? 0 : -ETIMEDOUT;
}

/* ========================================================================= */
/*  PCI driver: probe / remove                                               */
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

    /* Map buffers for DMA */
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

    /* Unmap TX buffer, we're done with it */
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

    /* Unmap RX buffer so CPU can read the data */
    dma_unmap_single(&pdev->dev, dst_dma, 64, DMA_FROM_DEVICE);
    dst_dma = 0;

    /* Verify data roundtripped through the device */
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
