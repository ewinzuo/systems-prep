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
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "hw/qdev-properties.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qom/object.h"

/* --- Register offsets (BAR0 MMIO) ---------------------------------------- */
/*                                                                           */
/* These are the "hardware registers" the driver sees when it does           */
/* ioread32/iowrite32 on the mapped BAR0 address. Each offset corresponds    */
/* to a case in dma_engine_mmio_read/write below.                            */
/*                                                                           */
/* The driver programs the device by writing ring addresses and depths,      */
/* then kicks processing by writing the SQ tail to the doorbell register.    */

#define REG_DEVICE_ID       0x00    /* RO: magic 0xDEA10001 — driver sanity check */
#define REG_DEVICE_STATUS   0x04    /* RO: bit 0 = rings configured and ready */
#define REG_SQ_BASE_LO      0x08    /* RW: SQ ring DMA address (low 32 bits) */
#define REG_SQ_BASE_HI      0x0C    /* RW: SQ ring DMA address (high 32 bits) */
#define REG_CQ_BASE_LO      0x10    /* RW: CQ ring DMA address (low 32 bits) */
#define REG_CQ_BASE_HI      0x14    /* RW: CQ ring DMA address (high 32 bits) */
#define REG_SQ_DEPTH        0x18    /* RW: number of SQE slots (power of 2) */
#define REG_CQ_DEPTH        0x1C    /* RW: number of CQE slots (power of 2) */
#define REG_SQ_DOORBELL     0x20    /* WO: driver writes new SQ tail index */
#define REG_IRQ_STATUS      0x24    /* RO: which interrupts are pending */
#define REG_IRQ_ACK         0x28    /* WO: driver writes to clear interrupt */
#define REG_SQ_HEAD         0x2C    /* RO: device's SQ consumer index */
#define REG_CQ_TAIL         0x30    /* RO: device's CQ producer index */

#define IRQ_CQ_READY        0x01    /* bit set when CQEs are available */

#define DEVICE_ID_MAGIC     0xDEA10001
#define DEV_BUF_SIZE        4096    /* on-chip SRAM simulation */

/* --- Descriptor formats (must match kernel driver exactly) --------------- */
/*                                                                           */
/* These are the structs that live in the SQ/CQ rings in guest RAM.          */
/* The device DMA-reads DevSqe from the SQ, and DMA-writes DevCqe to CQ.    */
/* Both sides (this file + nic.h) must agree on layout and size.             */

/* SQE: 24 bytes — one DMA transfer request */
typedef struct {
    uint64_t cookie;        /* opaque ID, echoed back in CQE */
    uint64_t buf_addr;      /* guest DMA address of data buffer */
    uint32_t len;           /* bytes to transfer */
    uint32_t op;            /* 1=TX (host→device), 2=RX (device→host) */
} __attribute__((packed)) DevSqe;

/* CQE: 16 bytes — one DMA completion notification */
typedef struct {
    uint64_t cookie;        /* matches the SQE that triggered this */
    int32_t  status;        /* 0=success, negative=error */
    uint32_t bytes_xferred; /* actual bytes moved */
} __attribute__((packed)) DevCqe;

#define OP_TX  1  /* host RAM → device internal buffer */
#define OP_RX  2  /* device internal buffer → host RAM */

/* --- Device state -------------------------------------------------------- */
/*                                                                           */
/* This struct is the entire "hardware" of our device. In real silicon,      */
/* these would be flip-flops, SRAM, and register banks on the chip.          */
/* QEMU allocates one of these per -device dma_engine instance.              */

#define TYPE_DMA_ENGINE "dma_engine"
OBJECT_DECLARE_SIMPLE_TYPE(DmaEngineState, DMA_ENGINE)

struct DmaEngineState {
    PCIDevice parent_obj;   /* base class — gives us PCI config space, BARs */

    MemoryRegion mmio;      /* BAR0 — 4KB of register space */

    /* Ring configuration (driver programs these via MMIO writes) */
    uint64_t sq_base;       /* guest physical addr of SQ ring */
    uint64_t cq_base;       /* guest physical addr of CQ ring */
    uint32_t sq_depth;      /* number of SQE slots */
    uint32_t cq_depth;      /* number of CQE slots */

    /* Ring indices (device-maintained, driver reads via MMIO) */
    uint32_t sq_head;       /* next SQE to consume (device advances) */
    uint32_t sq_tail;       /* last doorbell value (driver's producer index) */
    uint32_t cq_tail;       /* next CQE slot to write (device advances) */

    /* Interrupt state */
    uint32_t irq_status;    /* bits indicating pending interrupts */

    /* Timer for async DMA — models real hardware processing latency */
    QEMUTimer dma_timer;

    /* Device internal memory — like on-chip HBM/SRAM on an accelerator.
     * TX copies data here, RX copies data from here. */
    uint8_t dev_buf[DEV_BUF_SIZE];
};

/* --- IRQ helpers --------------------------------------------------------- */
/*                                                                           */
/* In real hardware, raising an IRQ sends an MSI message (a PCIe write to    */
/* a special address) that the CPU's interrupt controller turns into a        */
/* vector delivery. QEMU simulates this with msi_notify().                   */
/*                                                                           */
/* Legacy INTx is supported as fallback (pci_set_irq toggles the pin).       */

static void dma_engine_raise_irq(DmaEngineState *s)
{
    s->irq_status |= IRQ_CQ_READY;
    if (msi_enabled(&s->parent_obj)) {
        msi_notify(&s->parent_obj, 0);  /* send MSI vector 0 */
    } else {
        pci_set_irq(&s->parent_obj, 1); /* assert legacy INTx line */
    }
}

static void dma_engine_lower_irq(DmaEngineState *s)
{
    s->irq_status = 0;
    if (!msi_enabled(&s->parent_obj)) {
        pci_set_irq(&s->parent_obj, 0); /* de-assert INTx line */
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
/*                                                                           */
/* These functions are called by QEMU when the guest kernel does             */
/* ioread32/iowrite32 on the BAR0 address range. From the guest's            */
/* perspective, it's reading/writing memory-mapped hardware registers.        */
/* From our perspective, it's a function call.                                */

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
        /* THE DOORBELL — this is the critical write that kicks the device.
         * In real hardware, this is a PCIe posted write to a BAR register.
         * We save the new tail value and schedule processing via timer
         * (models the ~1us latency of real DMA engine startup). */
        s->sq_tail = (uint32_t)val;
        timer_mod(&s->dma_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000); /* 1ms delay */
        break;
    case REG_IRQ_ACK:
        dma_engine_lower_irq(s);
        break;
    default:
        break;
    }
}

/* Connect our read/write handlers to the MMIO region.
 * All accesses are 32-bit (driver uses two writes for 64-bit values). */
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
/*                                                                           */
/* realize() = "power on". Called when QEMU instantiates the device.         */
/* This is where we set up the PCI config space, BAR, and interrupt mode.    */
/* The device is now on the bus — the guest kernel will see it in lspci.     */

static void dma_engine_realize(PCIDevice *pdev, Error **errp)
{
    DmaEngineState *s = DMA_ENGINE(pdev);

    /* Legacy INTx pin as fallback (pin A) */
    pci_config_set_interrupt_pin(pdev->config, 1);

    /* MSI: 1 vector. Driver calls pci_alloc_irq_vectors to use it. */
    if (msi_init(pdev, 0, 1, true, false, errp)) {
        return;
    }

    /* Timer for deferred DMA processing.
     * When doorbell is written, we schedule this to fire after 1ms.
     * This models real hardware latency and avoids re-entrancy. */
    timer_init_ns(&s->dma_timer, QEMU_CLOCK_VIRTUAL,
                  dma_engine_process, s);

    /* BAR0: 4KB MMIO region containing our registers.
     * memory_region_init_io connects our read/write handlers.
     * pci_register_bar puts it in PCI config space so the OS can map it. */
    memory_region_init_io(&s->mmio, OBJECT(s), &dma_engine_mmio_ops, s,
                          "dma-engine-mmio", 4096);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
}

/* exit() = "power off". Called when QEMU shuts down or device is removed. */
static void dma_engine_exit(PCIDevice *pdev)
{
    DmaEngineState *s = DMA_ENGINE(pdev);

    timer_del(&s->dma_timer);
    msi_uninit(pdev);
}

/* --- QOM type registration ----------------------------------------------- */
/*                                                                           */
/* QEMU's Object Model (QOM) is how devices are registered. This is         */
/* analogous to module_init + pci_register_driver in the kernel.              */
/*                                                                           */
/* class_init: sets PCI IDs, function pointers. Runs once at QEMU startup.   */
/* type_init: GCC constructor — registers our type before main() runs.       */
/*                                                                           */
/* When the user passes -device dma_engine, QEMU looks up "dma_engine"       */
/* in the type registry, allocates DmaEngineState, and calls realize().       */

static void dma_engine_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    pc->realize = dma_engine_realize;   /* called on device instantiation */
    pc->exit = dma_engine_exit;         /* called on shutdown */
    pc->vendor_id = PCI_VENDOR_ID_QEMU; /* 0x1234 — QEMU's test vendor */
    pc->device_id = 0xdea1;             /* our unique device ID */
    pc->revision = 0x01;
    pc->class_id = PCI_CLASS_OTHERS;

    dc->desc = "DMA Engine with SQ/CQ descriptor rings";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo dma_engine_info = {
    .name = TYPE_DMA_ENGINE,            /* string name for -device flag */
    .parent = TYPE_PCI_DEVICE,          /* inherits PCI config space, BARs */
    .instance_size = sizeof(DmaEngineState),
    .class_init = dma_engine_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

/* GCC constructor: runs before main(), registers type in QEMU's registry */
static void dma_engine_register_types(void)
{
    type_register_static(&dma_engine_info);
}

type_init(dma_engine_register_types)
