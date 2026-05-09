/*
 * QEMU DMA Engine PCI Device
 *
 * A PCI device that implements SQ/CQ descriptor ring-based DMA,
 * matching how real accelerator DMA engines (NVMe, Trainium) work.
 *
 * The device:
 *   1. Accepts ring base addresses and depths from the driver via MMIO
 *   2. On doorbell write, DMA-reads SQEs from the SQ ring in guest RAM
 *   3. Performs data DMA (host RAM <-> device internal buffer)
 *   4. DMA-writes CQEs to the CQ ring in guest RAM
 *   5. Raises MSI interrupt
 *
 * Build: copy into qemu/hw/misc/, add to meson.build and Kconfig.
 * Usage: -device dma_engine
 *
 * PCI IDs: vendor=0x1234 device=0xdea1
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/qdev-properties.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qom/object.h"

/* --- Register offsets ---------------------------------------------------- */

#define REG_DEVICE_ID       0x00    /* RO: 0xDEA10001 */
#define REG_DEVICE_STATUS   0x04    /* RO: bit 0 = ready */
#define REG_SQ_BASE_LO      0x08    /* RW: SQ ring DMA address (low 32) */
#define REG_SQ_BASE_HI      0x0C    /* RW: SQ ring DMA address (high 32) */
#define REG_CQ_BASE_LO      0x10    /* RW: CQ ring DMA address (low 32) */
#define REG_CQ_BASE_HI      0x14    /* RW: CQ ring DMA address (high 32) */
#define REG_SQ_DEPTH        0x18    /* RW: SQ depth (power of 2) */
#define REG_CQ_DEPTH        0x1C    /* RW: CQ depth (power of 2) */
#define REG_SQ_DOORBELL     0x20    /* WO: driver writes new SQ tail */
#define REG_IRQ_STATUS      0x24    /* RO: interrupt status */
#define REG_IRQ_ACK         0x28    /* WO: write to clear IRQ */
#define REG_SQ_HEAD         0x2C    /* RO: device's SQ consumer index */
#define REG_CQ_TAIL         0x30    /* RO: device's CQ producer index */

#define IRQ_CQ_READY        0x01

#define DEVICE_ID_MAGIC     0xDEA10001
#define DEV_BUF_SIZE        4096

/* --- Descriptor formats (must match kernel driver) ----------------------- */

/* SQE: 24 bytes */
typedef struct {
    uint64_t cookie;
    uint64_t buf_addr;      /* guest DMA address of data buffer */
    uint32_t len;
    uint32_t op;            /* 1=TX (host->dev), 2=RX (dev->host) */
} __attribute__((packed)) DevSqe;

/* CQE: 16 bytes */
typedef struct {
    uint64_t cookie;
    int32_t  status;
    uint32_t bytes_xferred;
} __attribute__((packed)) DevCqe;

#define OP_TX  1
#define OP_RX  2

/* --- Device state -------------------------------------------------------- */

#define TYPE_DMA_ENGINE "dma_engine"
OBJECT_DECLARE_SIMPLE_TYPE(DmaEngineState, DMA_ENGINE)

struct DmaEngineState {
    PCIDevice parent_obj;

    MemoryRegion mmio;

    /* Ring configuration (set by driver via MMIO) */
    uint64_t sq_base;
    uint64_t cq_base;
    uint32_t sq_depth;
    uint32_t cq_depth;

    /* Ring indices (device-maintained) */
    uint32_t sq_head;       /* device consumes from here */
    uint32_t sq_tail;       /* last doorbell value from driver */
    uint32_t cq_tail;       /* device produces here */

    /* IRQ */
    uint32_t irq_status;

    /* Async processing timer */
    QEMUTimer dma_timer;

    /* Device internal memory (like on-chip SRAM) */
    uint8_t dev_buf[DEV_BUF_SIZE];
};

/* --- IRQ helpers --------------------------------------------------------- */

static void dma_engine_raise_irq(DmaEngineState *s)
{
    s->irq_status |= IRQ_CQ_READY;
    if (msi_enabled(&s->parent_obj)) {
        msi_notify(&s->parent_obj, 0);
    } else {
        pci_set_irq(&s->parent_obj, 1);
    }
}

static void dma_engine_lower_irq(DmaEngineState *s)
{
    s->irq_status = 0;
    if (!msi_enabled(&s->parent_obj)) {
        pci_set_irq(&s->parent_obj, 0);
    }
}

/* --- DMA processing (timer callback) ------------------------------------ */
/*                                                                          */
/* This runs asynchronously after a doorbell write, modeling real hardware   */
/* latency. It processes all pending SQEs between sq_head and sq_tail.      */
/*                                                                          */
/* For each SQE:                                                            */
/*   1. DMA-read the SQE from the SQ ring in guest RAM                     */
/*   2. DMA-read (TX) or DMA-write (RX) the data buffer                   */
/*   3. Build a CQE and DMA-write it to the CQ ring in guest RAM          */
/*   4. Advance sq_head and cq_tail                                        */
/*                                                                          */
/* After draining all SQEs, raise an interrupt.                             */
/* ----------------------------------------------------------------------- */

static void dma_engine_process(void *opaque)
{
    DmaEngineState *s = opaque;
    PCIDevice *pdev = &s->parent_obj;
    bool did_work = false;

    while (s->sq_head != s->sq_tail) {
        DevSqe sqe;
        DevCqe cqe;
        uint64_t sqe_addr;
        uint64_t cqe_addr;
        uint32_t sq_idx;
        uint32_t cq_idx;
        uint32_t len;

        /* Check CQ has space */
        if (s->cq_tail - s->sq_head >= s->cq_depth) {
            break;  /* CQ full, stop processing */
        }

        /* 1. DMA-read SQE from guest RAM */
        sq_idx = s->sq_head & (s->sq_depth - 1);
        sqe_addr = s->sq_base + (uint64_t)sq_idx * sizeof(DevSqe);
        pci_dma_read(pdev, sqe_addr, &sqe, sizeof(sqe));

        /* Clamp length to device buffer size */
        len = sqe.len;
        if (len > DEV_BUF_SIZE) {
            len = DEV_BUF_SIZE;
        }

        /* 2. Perform data DMA */
        cqe.status = 0;
        if (sqe.op == OP_TX) {
            /* Host -> device: read data from guest buffer into dev_buf */
            if (pci_dma_read(pdev, sqe.buf_addr, s->dev_buf, len) != 0) {
                cqe.status = -1;    /* DMA error */
            }
        } else if (sqe.op == OP_RX) {
            /* Device -> host: write dev_buf to guest buffer */
            if (pci_dma_write(pdev, sqe.buf_addr, s->dev_buf, len) != 0) {
                cqe.status = -1;
            }
        } else {
            cqe.status = -22;      /* -EINVAL: unknown op */
        }

        /* 3. Build CQE and DMA-write to guest CQ ring */
        cqe.cookie = sqe.cookie;
        cqe.bytes_xferred = (cqe.status == 0) ? len : 0;

        cq_idx = s->cq_tail & (s->cq_depth - 1);
        cqe_addr = s->cq_base + (uint64_t)cq_idx * sizeof(DevCqe);
        pci_dma_write(pdev, cqe_addr, &cqe, sizeof(cqe));

        /* 4. Advance indices */
        s->sq_head++;
        s->cq_tail++;
        did_work = true;
    }

    /* Raise interrupt if we completed any work */
    if (did_work) {
        dma_engine_raise_irq(s);
    }
}

/* --- MMIO register access ------------------------------------------------ */

static uint64_t dma_engine_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    DmaEngineState *s = opaque;

    switch (addr) {
    case REG_DEVICE_ID:
        return DEVICE_ID_MAGIC;
    case REG_DEVICE_STATUS:
        return (s->sq_base && s->cq_base && s->sq_depth && s->cq_depth)
               ? 1 : 0;
    case REG_SQ_BASE_LO:
        return (uint32_t)s->sq_base;
    case REG_SQ_BASE_HI:
        return (uint32_t)(s->sq_base >> 32);
    case REG_CQ_BASE_LO:
        return (uint32_t)s->cq_base;
    case REG_CQ_BASE_HI:
        return (uint32_t)(s->cq_base >> 32);
    case REG_SQ_DEPTH:
        return s->sq_depth;
    case REG_CQ_DEPTH:
        return s->cq_depth;
    case REG_IRQ_STATUS:
        return s->irq_status;
    case REG_SQ_HEAD:
        return s->sq_head;
    case REG_CQ_TAIL:
        return s->cq_tail;
    default:
        return 0;
    }
}

static void dma_engine_mmio_write(void *opaque, hwaddr addr,
                                  uint64_t val, unsigned size)
{
    DmaEngineState *s = opaque;

    switch (addr) {
    case REG_SQ_BASE_LO:
        s->sq_base = (s->sq_base & 0xFFFFFFFF00000000ULL) | (uint32_t)val;
        break;
    case REG_SQ_BASE_HI:
        s->sq_base = (s->sq_base & 0x00000000FFFFFFFFULL) | ((uint64_t)val << 32);
        break;
    case REG_CQ_BASE_LO:
        s->cq_base = (s->cq_base & 0xFFFFFFFF00000000ULL) | (uint32_t)val;
        break;
    case REG_CQ_BASE_HI:
        s->cq_base = (s->cq_base & 0x00000000FFFFFFFFULL) | ((uint64_t)val << 32);
        break;
    case REG_SQ_DEPTH:
        s->sq_depth = (uint32_t)val;
        break;
    case REG_CQ_DEPTH:
        s->cq_depth = (uint32_t)val;
        break;
    case REG_SQ_DOORBELL:
        /* Driver wrote new SQ tail — schedule async processing */
        s->sq_tail = (uint32_t)val;
        timer_mod(&s->dma_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000); /* 1ms */
        break;
    case REG_IRQ_ACK:
        dma_engine_lower_irq(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps dma_engine_mmio_ops = {
    .read = dma_engine_mmio_read,
    .write = dma_engine_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* --- PCI lifecycle ------------------------------------------------------- */

static void dma_engine_realize(PCIDevice *pdev, Error **errp)
{
    DmaEngineState *s = DMA_ENGINE(pdev);

    /* Set up INTx pin as fallback */
    pci_config_set_interrupt_pin(pdev->config, 1);

    /* Enable MSI (1 vector) */
    if (msi_init(pdev, 0, 1, true, false, errp)) {
        return;
    }

    /* Init async DMA timer */
    timer_init_ns(&s->dma_timer, QEMU_CLOCK_VIRTUAL,
                  dma_engine_process, s);

    /* Register BAR0: 4KB MMIO region */
    memory_region_init_io(&s->mmio, OBJECT(s), &dma_engine_mmio_ops, s,
                          "dma-engine-mmio", 4096);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
}

static void dma_engine_exit(PCIDevice *pdev)
{
    DmaEngineState *s = DMA_ENGINE(pdev);

    timer_del(&s->dma_timer);
    msi_uninit(pdev);
}

/* --- QOM type registration ----------------------------------------------- */

static void dma_engine_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    pc->realize = dma_engine_realize;
    pc->exit = dma_engine_exit;
    pc->vendor_id = PCI_VENDOR_ID_QEMU;     /* 0x1234 */
    pc->device_id = 0xdea1;
    pc->revision = 0x01;
    pc->class_id = PCI_CLASS_OTHERS;

    dc->desc = "DMA Engine with SQ/CQ descriptor rings";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo dma_engine_info = {
    .name = TYPE_DMA_ENGINE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(DmaEngineState),
    .class_init = dma_engine_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void dma_engine_register_types(void)
{
    type_register_static(&dma_engine_info);
}

type_init(dma_engine_register_types)
