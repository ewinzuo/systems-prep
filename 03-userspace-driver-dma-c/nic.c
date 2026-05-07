/* Day 3 implementation goes here.
 *
 * Sketch:
 *   - Two SPSC-ish rings: one for SQEs (host -> device), one for CQEs (device -> host).
 *     Reuse Day 1's ring or inline equivalent.
 *   - A worker pthread that loops:
 *       wait_doorbell();
 *       while (sq has entries) {
 *           sqe = sq.pop();
 *           // simulate "DMA": device-side memcpy into a private buffer
 *           cq.push({sqe.cookie, 0});
 *       }
 *   - Doorbell: an atomic_uint counter the host increments and the device reads
 *     under a mutex+condvar (or busy-polls under a flag).
 *   - Shutdown: atomic stop flag + a final doorbell; worker drains and exits.
 *
 * Edge cases to surface in tests:
 *   - SQ full backpressure
 *   - Doorbell coalescing (one bell, many SQEs)
 *   - Many-to-one host-to-device (with a host-side mutex around submit)
 *   - In-flight tracking with a checksum on the buffer to verify the device "saw"
 *     the descriptor before the host modified the buffer
 */

#include "nic.h"

nic_t *nic_open(size_t sq_depth, size_t cq_depth) {
    (void)sq_depth; (void)cq_depth;
    return 0;
}

void nic_close(nic_t *n) { (void)n; }
int  nic_submit(nic_t *n, const nic_sqe_t *sqe) { (void)n; (void)sqe; return -1; }
void nic_doorbell(nic_t *n) { (void)n; }
size_t nic_drain(nic_t *n, nic_cqe_t *out, size_t max) {
    (void)n; (void)out; (void)max; return 0;
}
