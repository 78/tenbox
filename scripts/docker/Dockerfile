# Build environment for AgentSphere rootfs/initramfs/kernel images.
# Native builds only — no cross-compilation. Use matching-arch runners.
#
# Two build targets:
#   base  — lightweight image for kernel/initramfs (no qemu-img)
#   full  — adds qemu-img with zstd support for rootfs builds
#
# Usage (via build.sh wrapper — selects the right target automatically):
#   ./scripts/docker/build.sh arm64 rootfs-chromium
#   ./scripts/docker/build.sh x86_64 initramfs
#   ./scripts/docker/build.sh x86_64 kernel
#
# Or manually:
#   docker build --target base -t tenbox-builder-base scripts/docker/
#   docker build --target full -t tenbox-builder scripts/docker/

# --- Stage 1: build qemu-img with zstd (only pulled in by "full" target) ---
FROM debian:bookworm-slim AS qemu-build

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

# --- Stage 2: base runtime (kernel / initramfs) ---
FROM debian:bookworm-slim AS base

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
    build-essential bc kmod file \
    flex bison libelf-dev libssl-dev dwarves \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
ENTRYPOINT ["/bin/bash"]

# --- Stage 3: full runtime (rootfs — adds qemu-img) ---
FROM base AS full

COPY --from=qemu-build /tmp/qemu-src/build/qemu-img /usr/local/bin/qemu-img

RUN apt-get update && apt-get install -y --no-install-recommends \
    libglib2.0-0 libzstd1 libpixman-1-0 \
    && rm -rf /var/lib/apt/lists/* \
    && qemu-img --version
