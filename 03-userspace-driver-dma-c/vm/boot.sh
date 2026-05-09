#!/bin/bash
# Boot the QEMU VM for kernel module development
#
# Login: ubuntu / ubuntu
#
# First boot takes ~60s (cloud-init installs packages).
# Subsequent boots are fast.
#
# Once inside:
#   sudo mount -t 9p -o trans=virtio hostfs /mnt
#   cd /mnt/03-userspace-driver-dma-c
#   make
#   sudo insmod nic.ko
#   dmesg | tail          # look for "nic: smoke test PASSED"
#   sudo rmmod nic
#
# To exit: Ctrl-A then X

set -euo pipefail

VM_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${VM_DIR}/.." && pwd)"

# Share the entire systems-prep directory so the module can find sources
HOST_SHARE="$(cd "${PROJECT_DIR}/.." && pwd)"

QEMU_ARGS=(
    -m 2G
    -smp 2
    -nographic
    -drive "file=${VM_DIR}/disk.qcow2,format=qcow2"
    -drive "file=${VM_DIR}/seed.img,format=raw"
    -virtfs "local,path=${HOST_SHARE},mount_tag=hostfs,security_model=mapped-xattr,id=host"
    -net nic -net user
)

# Use KVM if available (Linux only, much faster)
if [ -e /dev/kvm ]; then
    QEMU_ARGS+=(-enable-kvm -cpu host)
    echo ">>> KVM enabled"
else
    echo ">>> No KVM — VM will be slow but functional"
fi

echo ">>> Booting VM (Ctrl-A X to quit)..."
echo ">>> Login: ubuntu / ubuntu"
echo ""

qemu-system-x86_64 "${QEMU_ARGS[@]}"
