#ifndef NIC_H
#define NIC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nic nic_t;

typedef enum { NIC_OP_TX = 1, NIC_OP_RX = 2 } nic_op_t;

typedef struct {
    uint64_t  cookie;     /* host-defined; echoed on completion */
    void     *buf;        /* TX: source; RX: destination */
    uint32_t  len;
    nic_op_t  op;
} nic_sqe_t;

typedef struct {
    uint64_t cookie;
    int32_t  status;      /* 0 = ok, negative = errno-like */
} nic_cqe_t;

/* sq_depth and cq_depth must be power-of-two. */
nic_t *nic_open(size_t sq_depth, size_t cq_depth);
void   nic_close(nic_t *n);

/* Returns 0 on success, -1 if SQ full. Buffer must remain alive
 * until a matching CQE arrives. */
int    nic_submit(nic_t *n, const nic_sqe_t *sqe);

/* Doorbell the device — wakes any blocked worker; cheap if not blocked. */
void   nic_doorbell(nic_t *n);

/* Drain up to max CQEs into out[]. Returns count actually copied. */
size_t nic_drain(nic_t *n, nic_cqe_t *out, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* NIC_H */
