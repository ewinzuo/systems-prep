#!/bin/bash
# Build QEMU from source with the custom dma_engine device.
#
# Prerequisites (provided by shell.nix):
#   meson, ninja, gcc, glib, pixman, python3, flex, bison, libslirp
#
# Usage:
#   cd 03-userspace-driver-dma-c/vm
#   nix-shell shell.nix --run "bash build-qemu.sh"
#
# Result: ./qemu-build/qemu-system-x86_64

set -euo pipefail

VM_DIR="$(cd "$(dirname "$0")" && pwd)"
QEMU_VERSION="10.0.0"
QEMU_TARBALL="qemu-${QEMU_VERSION}.tar.xz"
QEMU_URL="https://download.qemu.org/${QEMU_TARBALL}"
QEMU_SRC="${VM_DIR}/qemu-${QEMU_VERSION}"
BUILD_DIR="${VM_DIR}/qemu-build"

# Download QEMU source
if [ ! -f "${VM_DIR}/${QEMU_TARBALL}" ]; then
    echo ">>> Downloading QEMU ${QEMU_VERSION}..."
    curl -L -o "${VM_DIR}/${QEMU_TARBALL}" "${QEMU_URL}"
fi

# Extract
if [ ! -d "${QEMU_SRC}" ]; then
    echo ">>> Extracting..."
    tar xf "${VM_DIR}/${QEMU_TARBALL}" -C "${VM_DIR}"
fi

# Copy our custom device into the QEMU source tree
echo ">>> Installing dma_engine device..."
cp "${VM_DIR}/dma_engine.c" "${QEMU_SRC}/hw/misc/dma_engine.c"

# Patch meson.build to include our device
MESON_FILE="${QEMU_SRC}/hw/misc/meson.build"
if ! grep -q "dma_engine" "${MESON_FILE}"; then
    echo ">>> Patching meson.build..."
    echo "" >> "${MESON_FILE}"
    echo "system_ss.add(when: 'CONFIG_DMA_ENGINE', if_true: files('dma_engine.c'))" >> "${MESON_FILE}"
fi

# Patch Kconfig to enable our device
KCONFIG_FILE="${QEMU_SRC}/hw/misc/Kconfig"
if ! grep -q "DMA_ENGINE" "${KCONFIG_FILE}"; then
    echo ">>> Patching Kconfig..."
    cat >> "${KCONFIG_FILE}" << 'EOF'

config DMA_ENGINE
    bool
    default y
    depends on PCI && MSI_NONBROKEN
EOF
fi

# Configure (minimal build: x86_64 only)
if [ ! -f "${BUILD_DIR}/build.ninja" ]; then
    echo ">>> Configuring QEMU (x86_64 only)..."
    mkdir -p "${BUILD_DIR}"
    cd "${QEMU_SRC}"
    ./configure \
        --target-list=x86_64-softmmu \
        --enable-kvm \
        --enable-slirp \
        --disable-docs \
        --disable-gtk \
        --disable-sdl \
        --disable-opengl \
        --prefix="${BUILD_DIR}/install" \
        --extra-cflags="-O2"
    cd "${VM_DIR}"
fi

# Build
echo ">>> Building QEMU (this takes a few minutes)..."
cd "${QEMU_SRC}"
make -j"$(nproc)"

# Symlink the binary for easy access
ln -sf "${QEMU_SRC}/build/qemu-system-x86_64" "${BUILD_DIR}/qemu-system-x86_64"

echo ""
echo ">>> Build complete: ${BUILD_DIR}/qemu-system-x86_64"
echo ">>> Run: bash boot.sh"
