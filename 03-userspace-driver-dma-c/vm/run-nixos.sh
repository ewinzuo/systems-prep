#!/bin/bash
set -euo pipefail

VM_DIR="$(cd "$(dirname "$0")" && pwd)"
MODULE_DIR="$(cd "${VM_DIR}/.." && pwd)"
QEMU="${VM_DIR}/qemu-build/qemu-system-x86_64"

cd "$MODULE_DIR" && make

BUSYBOX=$(command -v busybox 2>/dev/null || find /nix/store -maxdepth 2 -name busybox -type f 2>/dev/null | head -1)
[ -z "$BUSYBOX" ] && echo "ERROR: busybox not found" && exit 1

# Build minimal initramfs
WORK=$(mktemp -d)
mkdir -p "$WORK"/{bin,proc,sys,dev}

cp "$BUSYBOX" "$WORK/bin/busybox"
chmod +x "$WORK/bin/busybox"
ln -s busybox "$WORK/bin/sh"
for cmd in insmod dmesg mount grep cat ls echo; do
    ln -s busybox "$WORK/bin/$cmd"
done

cp "$MODULE_DIR/nic.ko" "$WORK/"

# /init must be a statically-linked executable.
# We use busybox sh via a script that has busybox as interpreter.
# But the kernel needs a binary at /init, so we use a trick:
# make /init a symlink to /bin/busybox, and pass init=/bin/sh to kernel,
# then sh reads /etc/profile or we use a different approach.

# Simplest: compile a tiny static C init? No.
# Simplest for real: the kernel can run a shell script if the interpreter exists.
# The shebang must point to something IN the initramfs.
cat > "$WORK/init" << INITEOF
#!./bin/sh
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev 2>/dev/null

echo
echo "=== insmod nic.ko ==="
insmod /nic.ko
echo "insmod returned: \$?"
echo
echo "=== dmesg ==="
dmesg | grep -iE "dma_engine|nic|smoke|PASSED|FAILED|probe|1234:dea1"
echo "============="
echo
echo "Shell ready. Ctrl-A X to exit QEMU."
while true; do sh; done
INITEOF
chmod 755 "$WORK/init"

INITRD="${VM_DIR}/.initrd.img"
(cd "$WORK" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip) > "$INITRD"
rm -rf "$WORK"

echo ">>> Booting..."
exec "$QEMU" \
    -machine accel=kvm:tcg -cpu host \
    -m 512M -nographic \
    -device dma_engine \
    -kernel /run/current-system/kernel \
    -initrd "$INITRD" \
    -append "console=ttyS0 rdinit=/init"
