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
IMG_URL="https://cloud-images.ubuntu.com/jammy/current/jammy-server-cloudimg-amd64.img"

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

    cloud-localds "$SEED" "${VM_DIR}/user-data" "${VM_DIR}/meta-data"
    echo ">>> Seed image created."
fi

echo ""
echo "Setup complete. Run: bash boot.sh"
