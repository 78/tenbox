# Build environment for TenBox rootfs/initramfs/kernel images.
# Native builds only — no cross-compilation. Use matching-arch runners.
#
# Includes a qemu-img built from source with zstd support.
#
# Usage (via build.sh wrapper):
#   ./scripts/docker/build.sh arm64 rootfs-chromium
#   ./scripts/docker/build.sh x86_64 rootfs-chromium
#
# Or manually:
#   docker build -t tenbox-builder scripts/docker/
#   docker run --privileged -v $(pwd):/workspace tenbox-builder \
#       /workspace/scripts/arm64/make-rootfs-chromium.sh

# --- Stage 1: build qemu-img with zstd ---
FROM debian:trixie-slim AS qemu-build

ARG QEMU_VERSION=9.2.2

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential pkg-config python3 python3-venv ninja-build \
    libglib2.0-dev libzstd-dev libpixman-1-dev \
    curl ca-certificates xz-utils \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fSL "https://download.qemu.org/qemu-${QEMU_VERSION}.tar.xz" \
        -o /tmp/qemu.tar.xz \
    && mkdir /tmp/qemu-src \
    && tar -xf /tmp/qemu.tar.xz -C /tmp/qemu-src --strip-components=1 \
    && cd /tmp/qemu-src \
    && ./configure \
        --target-list="" \
        --without-default-features \
        --enable-tools \
        --enable-zstd \
    && make -j"$(nproc)" qemu-img \
    && strip build/qemu-img

# --- Stage 2: runtime ---
FROM debian:trixie-slim

COPY --from=qemu-build /tmp/qemu-src/build/qemu-img /usr/local/bin/qemu-img

RUN apt-get update && apt-get install -y --no-install-recommends \
    debootstrap \
    e2fsprogs \
    curl \
    ca-certificates \
    xz-utils \
    zstd \
    gzip \
    cpio \
    python3 \
    coreutils \
    binutils \
    sudo \
    libglib2.0-0t64 libzstd1 libpixman-1-0 \
    build-essential bc kmod file \
    && rm -rf /var/lib/apt/lists/*

RUN qemu-img --version

WORKDIR /workspace
ENTRYPOINT ["/bin/bash"]
