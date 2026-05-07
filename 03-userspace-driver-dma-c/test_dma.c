/* Day 3 test stub. Filled in alongside the implementation. */

#include "nic.h"

#include <stdio.h>

int main(void) {
    /* TODO:
     *   open device, submit a few SQEs, doorbell, drain CQEs, assert cookies.
     *   concurrent test: M producers submitting under a mutex + 1 device worker.
     *   shutdown test: stop flag drains in-flight then exits.
     */
    puts("nic stub");
    return 0;
}
