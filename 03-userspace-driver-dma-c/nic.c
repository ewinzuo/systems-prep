#include "nic.h"

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

/* ========================================================================= */
/*  SPSC ring helpers                                                        */
/*                                                                           */
/*  Same design as Day 1, but using kernel APIs:                             */
/*    malloc/free          -> kmalloc/kfree                                  */
/*    atomic_store_release -> smp_store_release                              */
/*    atomic_load_acquire  -> smp_load_acquire                               */
/*                                                                           */
/*  SPSC means no locks needed on the ring itself:                           */
/*    - Producer owns tail (only writer)                                     */
/*    - Consumer owns head (only writer)                                     */
/*    - release/acquire on stores/loads ensure the other side sees data      */
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
/*  Device thread (simulates DMA hardware)                                   */
/* ========================================================================= */

static void drain_sq(engine_t *eng)
{
    sqe_t sqe;

    while (ring_pop(&eng->sq, &sqe) == 0) {
        cqe_t cqe;

        memcpy(sqe.dst, sqe.src, sqe.len);

        cqe.cookie = sqe.cookie;
        cqe.status = 0;
        cqe.bytes_xferred = sqe.len;

        if (ring_push(&eng->cq, &cqe) != 0)
            pr_warn("nic: CQ full, completion dropped (cookie=%llu)\n",
                    sqe.cookie);
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

    drain_sq(eng);
    return 0;
}

/* ========================================================================= */
/*  Engine API                                                               */
/* ========================================================================= */

engine_t *engine_create(u32 sq_depth, u32 cq_depth)
{
    engine_t *eng;
    int ret;

    eng = kzalloc(sizeof(*eng), GFP_KERNEL);
    if (!eng)
        return NULL;

    ret = ring_init(&eng->sq, sq_depth, sizeof(sqe_t));
    if (ret)
        goto err_free;

    ret = ring_init(&eng->cq, cq_depth, sizeof(cqe_t));
    if (ret)
        goto err_sq;

    atomic_set(&eng->doorbell, 0);
    init_waitqueue_head(&eng->dev_wq);
    init_waitqueue_head(&eng->host_wq);

    eng->dev_thread = kthread_run(device_thread_fn, eng, "nic_dev");
    if (IS_ERR(eng->dev_thread))
        goto err_cq;

    return eng;

err_cq:
    ring_destroy(&eng->cq);
err_sq:
    ring_destroy(&eng->sq);
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
/*  Module init / exit — smoke test on insmod                                */
/* ========================================================================= */

static engine_t *test_eng;

static int __init nic_init(void)
{
    char src[] = "hello DMA engine";
    char dst[32] = {0};
    sqe_t sqe;
    cqe_t cqe;
    size_t n;

    pr_info("nic: loading module\n");

    test_eng = engine_create(16, 16);
    if (!test_eng) {
        pr_err("nic: engine_create failed\n");
        return -ENOMEM;
    }

    sqe.cookie = 0xCAFE;
    sqe.src = src;
    sqe.dst = dst;
    sqe.len = sizeof(src);
    sqe.op = DMA_OP_TX;

    if (engine_submit(test_eng, &sqe) != 0) {
        pr_err("nic: submit failed\n");
        goto fail;
    }

    engine_doorbell(test_eng);

    if (engine_wait(test_eng, HZ) != 0) {
        pr_err("nic: wait timed out\n");
        goto fail;
    }

    n = engine_drain(test_eng, &cqe, 1);
    if (n != 1 || cqe.cookie != 0xCAFE || cqe.status != 0) {
        pr_err("nic: bad completion: n=%zu cookie=0x%llx status=%d\n",
               n, cqe.cookie, cqe.status);
        goto fail;
    }

    if (memcmp(src, dst, sizeof(src)) != 0) {
        pr_err("nic: data mismatch after transfer\n");
        goto fail;
    }

    pr_info("nic: smoke test PASSED\n");
    return 0;

fail:
    engine_destroy(test_eng);
    test_eng = NULL;
    return -EINVAL;
}

static void __exit nic_exit(void)
{
    engine_destroy(test_eng);
    pr_info("nic: module unloaded\n");
}

module_init(nic_init);
module_exit(nic_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DMA engine with descriptor rings (simulated)");
