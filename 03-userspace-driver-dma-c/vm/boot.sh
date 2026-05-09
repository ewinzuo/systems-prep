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
#   sudo insmod nic.ko    # probe fires, smoke test runs
#   dmesg | tail          # look for "smoke test PASSED"
#   sudo rmmod nic
#   lspci                 # should show "1234:dea1"
#
# To exit: Ctrl-A then X

set -euo pipefail

VM_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${VM_DIR}/.." && pwd)"
HOST_SHARE="$(cd "${PROJECT_DIR}/.." && pwd)"

# Use custom QEMU if built, otherwise fall back to system QEMU
QEMU="${VM_DIR}/qemu-build/qemu-system-x86_64"
if [ ! -x "$QEMU" ]; then
    QEMU="$(command -v qemu-system-x86_64 2>/dev/null || true)"
    if [ -z "$QEMU" ]; then
        echo ">>> ERROR: No QEMU found. Run: nix-shell shell.nix --run 'bash build-qemu.sh'"
        exit 1
    fi
    echo ">>> WARNING: Using system QEMU — custom dma_engine device may not be available"
    echo ">>>          Build custom QEMU with: nix-shell shell.nix --run 'bash build-qemu.sh'"
fi

QEMU_ARGS=(
    -m 2G
    -smp 2
    -nographic
    -drive "file=${VM_DIR}/disk.qcow2,format=qcow2"
    -drive "file=${VM_DIR}/seed.img,format=raw"
    -virtfs "local,path=${HOST_SHARE},mount_tag=hostfs,security_model=mapped-xattr,id=host"
    -device dma_engine
    -net nic -net user
)

# Use KVM if available (Linux only, much faster)
if [ -e /dev/kvm ]; then
    QEMU_ARGS+=(-enable-kvm -cpu host)
    echo ">>> KVM enabled"
else
    echo ">>> No KVM — VM will be slow but functional"
fi

echo ">>> Using QEMU: ${QEMU}"
echo ">>> Booting VM (Ctrl-A X to quit)..."
echo ">>> Login: ubuntu / ubuntu"
echo ""

"$QEMU" "${QEMU_ARGS[@]}"
