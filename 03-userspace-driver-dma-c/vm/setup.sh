#!/bin/bash
# QEMU VM for kernel module development
#
# Prerequisites (install on your host):
#   Ubuntu/Debian: sudo apt install qemu-system-x86 cloud-image-utils
#   Fedora:        sudo dnf install qemu-system-x86 cloud-utils
#   macOS:         brew install qemu (no KVM — will be slow)
#   Arch:          sudo pacman -S qemu-full cloud-image-utils
#
# Usage:
#   cd 03-userspace-driver-dma-c/vm
#   bash setup.sh      # downloads image + creates disk (first time only)
#   bash boot.sh       # boots the VM
#
#   Inside the VM:
#     sudo mount -t 9p -o trans=virtio hostfs /mnt
#     cd /mnt/03-userspace-driver-dma-c
#     make
#     sudo insmod nic.ko
#     dmesg | tail
#     sudo rmmod nic

set -euo pipefail

VM_DIR="$(cd "$(dirname "$0")" && pwd)"
DISK="${VM_DIR}/disk.qcow2"
SEED="${VM_DIR}/seed.img"
IMG="${VM_DIR}/base.img"
# Primary: Ubuntu cloud images. Fallback: use Debian if Ubuntu is unreachable.
IMG_URL="https://cloud.debian.org/images/cloud/bookworm/latest/debian-12-genericcloud-amd64.qcow2"

# Download Ubuntu cloud image
if [ ! -f "$IMG" ]; then
    echo ">>> Downloading Ubuntu 22.04 cloud image..."
    curl -L -o "$IMG" "$IMG_URL"
fi

# Create VM disk from cloud image
if [ ! -f "$DISK" ]; then
    echo ">>> Creating VM disk (10G)..."
    cp "$IMG" "$DISK"
    qemu-img resize "$DISK" 10G
fi

# Create cloud-init seed (auto-configures user, installs build deps)
if [ ! -f "$SEED" ]; then
    echo ">>> Creating cloud-init config..."

    cat > "${VM_DIR}/user-data" << 'EOF'
#cloud-config
password: ubuntu
chpasswd: { expire: false }
ssh_pwauth: true
packages:
  - build-essential
  - linux-headers-generic
runcmd:
  - mkdir -p /mnt
EOF

    cat > "${VM_DIR}/meta-data" << 'EOF'
instance-id: nic-dev
local-hostname: nic-dev
EOF

    # Create seed ISO with whatever tool is available
    if command -v cloud-localds &> /dev/null; then
        cloud-localds "$SEED" "${VM_DIR}/user-data" "${VM_DIR}/meta-data"
    elif command -v genisoimage &> /dev/null; then
        genisoimage -output "$SEED" -volid cidata -joliet -rock \
            "${VM_DIR}/user-data" "${VM_DIR}/meta-data"
    elif command -v xorriso &> /dev/null; then
        xorriso -as mkisofs -o "$SEED" -V cidata -J -R \
            "${VM_DIR}/user-data" "${VM_DIR}/meta-data"
    elif command -v mkisofs &> /dev/null; then
        mkisofs -output "$SEED" -volid cidata -joliet -rock \
            "${VM_DIR}/user-data" "${VM_DIR}/meta-data"
    else
        echo "ERROR: No ISO tool found. Install one of: cloud-image-utils, genisoimage, xorriso"
        exit 1
    fi
    echo ">>> Seed image created."
fi

echo ""
echo "Setup complete. Run: bash boot.sh"
