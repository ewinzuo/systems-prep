#include "nic.h"

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

/* ========================================================================= */
/*  MMIO helper                                                              */
/* ========================================================================= */

static void mmio_write64(void __iomem *mmio, u32 reg_lo, u64 val) {
  iowrite32((u32)val, mmio + reg_lo);
  iowrite32((u32)(val >> 32), mmio + reg_lo + 4);
}

/* ========================================================================= */
/*  IRQ handler                                                              */
/*                                                                           */
/*  Device raises MSI after writing CQEs. Acknowledge and wake waiters.      */
/*  Hint: read REG_IRQ_STATUS, write it to REG_IRQ_ACK, wake host_wq.       */
/* ========================================================================= */

static irqreturn_t dma_engine_irq(int irq, void *data) {
  /* TODO: implement */
  engine_t *eng = data;
  u32 status = ioread32(eng->mmio + REG_IRQ_STATUS);
  if (status) {
    iowrite32(status, eng->mmio + REG_IRQ_ACK);
    wake_up_interruptible(&eng->host_wq);
    return IRQ_HANDLED;
  }
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

engine_t *engine_create(struct pci_dev *pdev, u32 sq_depth, u32 cq_depth) {
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
  engine_t *eng;
  eng = kzalloc(sizeof(engine_t), GFP_KERNEL);
  eng->sq_depth = sq_depth;
  eng->cq_depth = cq_depth;
  eng->pdev = pdev;

  dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
  eng->sq = dma_alloc_coherent(&pdev->dev, sq_depth * sizeof(sqe_t),
                               &eng->sq_dma, GFP_KERNEL);
  eng->cq = dma_alloc_coherent(&pdev->dev, cq_depth * sizeof(cqe_t),
                               &eng->cq_dma, GFP_KERNEL);

  void __iomem *bar_pointer = pci_iomap(pdev, 0, 0);
  mmio_write64(bar_pointer, REG_SQ_BASE_LO, eng->sq_dma);
  mmio_write64(bar_pointer, REG_CQ_BASE_LO, eng->cq_dma);
  eng->mmio = bar_pointer;

  iowrite32(sq_depth, eng->mmio + REG_SQ_DEPTH);
  iowrite32(cq_depth, eng->mmio + REG_CQ_DEPTH);

  /* Allocate MSI vector and register IRQ handler */
  pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_INTX);
  request_irq(pci_irq_vector(pdev, 0), dma_engine_irq, 0, "dma_engine", eng);

  init_waitqueue_head(&eng->host_wq);

  return eng;
}

void engine_destroy(engine_t *eng) {
  if (!eng)
    return;

  free_irq(pci_irq_vector(eng->pdev, 0), eng);
  pci_free_irq_vectors(eng->pdev);

  dma_free_coherent(&eng->pdev->dev, eng->sq_depth * sizeof(sqe_t), eng->sq,
                    eng->sq_dma);
  dma_free_coherent(&eng->pdev->dev, eng->cq_depth * sizeof(cqe_t), eng->cq,
                    eng->cq_dma);

  pci_clear_master(eng->pdev);
  pci_iounmap(eng->pdev, eng->mmio);
  pci_release_region(eng->pdev, 0);
  pci_disable_device(eng->pdev);

  kfree(eng);
}

int engine_submit(engine_t *eng, const sqe_t *sqe) {
  /* TODO:
   * 1. Read REG_SQ_HEAD to check for backpressure
   * 2. If tail - head >= depth, return -EAGAIN
   * 3. Write SQE to sq[tail & (depth-1)]
   * 4. wmb() to ensure SQE is visible before tail advances
   * 5. Advance sq_tail
   */

  u32 head = ioread32(eng->mmio + REG_SQ_HEAD);
  u32 tail = eng->sq_tail;
  if (tail - head >= eng->sq_depth)
    return -EAGAIN;

  eng->sq[tail & (eng->sq_depth - 1)] = *sqe;

  wmb();
  eng->sq_tail += 1;

  return 0;
}

void engine_doorbell(engine_t *eng) {
  iowrite32(eng->sq_tail, eng->mmio + REG_SQ_DOORBELL);
}

size_t engine_drain(engine_t *eng, cqe_t *out, size_t max) {
  /* TODO:
   * 1. Read REG_CQ_TAIL from device
   * 2. rmb() before reading CQE data
   * 3. Copy CQEs from cq[head..tail] into out[]
   * 4. Advance cq_head
   */
  u32 cq_head = eng->cq_head;
  u32 cq_tail = ioread32(eng->mmio + REG_CQ_TAIL);
  u32 n = min_t(u32, cq_tail - cq_head, (u32)max);
  u32 i;

  if (n == 0)
    return 0;

  /* rmb: ensure we read CQE data AFTER seeing the updated tail. */
  rmb();

  for (i = 0; i < n; i++)
    out[i] = eng->cq[(cq_head + i) & (eng->cq_depth - 1)];
  eng->cq_head = cq_head + n;
  return n;
}

int engine_wait(engine_t *eng, long timeout_jiffies) {
  long ret = wait_event_interruptible_timeout(
      eng->host_wq, ioread32(eng->mmio + REG_CQ_TAIL) != eng->cq_head,
      timeout_jiffies);
  return ret > 0 ? 0 : -ETIMEDOUT;
}

/* ========================================================================= */
/*  PCI driver probe / remove                                                */
/*                                                                           */
/*  probe: called when kernel matches our PCI ID (1234:dea1).                */
/*    - Create engine                                                        */
/*    - Run a smoke test: TX data to device, RX back, verify match           */
/*    - Use dma_map_single for data buffers, engine_submit/doorbell/wait/    */
/*      drain for the DMA operations                                         */
/*                                                                           */
/*  remove: called on rmmod. Destroy engine.                                 */
/* ========================================================================= */

static int dma_engine_probe(struct pci_dev *pdev,
                            const struct pci_device_id *id) {
  /* TODO:
   * 1. engine_create(pdev, 16, 16)
   * 2. pci_set_drvdata(pdev, eng)
   * 3. Smoke test:
   *    a. kmalloc src and dst buffers
   *    b. dma_map_single both (src=DMA_TO_DEVICE, dst=DMA_FROM_DEVICE)
   *    c. TX: submit SQE with src_dma, doorbell, wait, drain CQE
   *    d. dma_unmap_single src
   *    e. RX: submit SQE with dst_dma, doorbell, wait, drain CQE
   *    f. dma_unmap_single dst
   *    g. memcmp(src, dst) to verify roundtrip
   *    h. Clean up on failure: unmap, kfree, engine_destroy
   */
  int ret;
  ret = pci_enable_device(pdev);
  pci_set_master(pdev);

  engine_t *eng = engine_create(pdev, 16, 16);
  pci_set_drvdata(pdev, eng);
  ret = pci_request_region(pdev, 0, "dma_engine");

  void *src = kmalloc(64, GFP_KERNEL);
  void *dst = kmalloc(64, GFP_KERNEL);

  memcpy(src, "hello DMA engine!", 18);
  memset(dst, 0, 64);

  dma_addr_t src_dma_addr = dma_map_single(&pdev->dev, src, 64, DMA_TO_DEVICE);
  dma_addr_t dst_dma_addr =
      dma_map_single(&pdev->dev, dst, 64, DMA_FROM_DEVICE);

  sqe_t sqe = (sqe_t){
      .cookie = 0xCAFE,
      .buf_addr = src_dma_addr,
      .len = 18,
      .op = DMA_OP_TX,
  };

  engine_submit(eng, &sqe);
  engine_doorbell(eng);
  if (engine_wait(eng, 2 * HZ) != 0) {
    dev_err(&pdev->dev, "TX timed out\n");
    goto err_unmap;
  }
  cqe_t cqe;
  size_t n = engine_drain(eng, &cqe, 1);
  if (n != 1 || cqe.status != 0) {
    dev_err(&pdev->dev, "TX bad: n=%zu status=%d\n", n, cqe.status);
    goto err_unmap;
  }

  /* Unmap TX buffer — device is done reading from it */
  dma_unmap_single(&pdev->dev, src_dma_addr, 64, DMA_TO_DEVICE);
  src_dma_addr = 0;

  /* RX: device -> host */
  sqe = (sqe_t){
      .cookie = 0xBEEF,
      .buf_addr = dst_dma_addr,
      .len = 18,
      .op = DMA_OP_RX,
  };
  engine_submit(eng, &sqe);
  engine_doorbell(eng);
  if (engine_wait(eng, 2 * HZ) != 0) {
    dev_err(&pdev->dev, "RX timed out\n");
    goto err_unmap;
  }
  n = engine_drain(eng, &cqe, 1);
  if (n != 1 || cqe.status != 0) {
    dev_err(&pdev->dev, "RX bad: n=%zu status=%d\n", n, cqe.status);
    goto err_unmap;
  }

  /* Unmap RX buffer — CPU needs to read what device wrote */
  dma_unmap_single(&pdev->dev, dst_dma_addr, 64, DMA_FROM_DEVICE);
  dst_dma_addr = 0;

  /* Verify data roundtripped through device */
  if (memcmp(src, dst, 18) != 0) {
    dev_err(&pdev->dev, "DATA MISMATCH: src='%s' dst='%s'\n", (char *)src,
            (char *)dst);
    goto err_free;
  }

  dev_info(&pdev->dev,
           "smoke test PASSED — data roundtripped through DMA engine\n");
  kfree(src);
  kfree(dst);
  return 0;

err_unmap:
  if (src_dma_addr)
    dma_unmap_single(&pdev->dev, src_dma_addr, 64, DMA_TO_DEVICE);
  if (dst_dma_addr)
    dma_unmap_single(&pdev->dev, dst_dma_addr, 64, DMA_FROM_DEVICE);
err_free:
  kfree(src);
  kfree(dst);
  engine_destroy(eng);
  pci_set_drvdata(pdev, NULL);
  return -EIO;
}

static void dma_engine_remove(struct pci_dev *pdev) {
  engine_t *eng = pci_get_drvdata(pdev);
  engine_destroy(eng);
}

static struct pci_device_id dma_engine_pci_ids[] = {
    {PCI_DEVICE(DMA_ENGINE_VENDOR_ID, DMA_ENGINE_DEVICE_ID)},
    {0},
};
MODULE_DEVICE_TABLE(pci, dma_engine_pci_ids);

static struct pci_driver dma_engine_pci_driver = {
    .name = "dma_engine",
    .id_table = dma_engine_pci_ids,
    .probe = dma_engine_probe,
    .remove = dma_engine_remove,
};

static int __init nic_init(void) {
  return pci_register_driver(&dma_engine_pci_driver);
}

static void __exit nic_exit(void) {
  pci_unregister_driver(&dma_engine_pci_driver);
}

module_init(nic_init);
module_exit(nic_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kernel driver for custom DMA engine with SQ/CQ rings");
