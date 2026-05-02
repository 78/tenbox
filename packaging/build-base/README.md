# tenbox release build base

This image bakes the static C dependencies tenbox links against
(`ffmpeg / x264 / opus / libyuv`) so the release pipeline can produce
binaries that run on any Debian-derived system with `glibc >= 2.35` and
`libssl3` available — Debian 12+, Ubuntu 22.04+, Raspberry Pi OS 12,
Armbian Bookworm, and the corresponding arm64 variants.

The image is consumed by `.github/workflows/release.yml`. On the matrix
runner side:

```bash
docker build -f packaging/build-base/Dockerfile.jammy \
    -t tenbox-build:jammy-${ARCH} \
    packaging/build-base
docker run --rm -v "$PWD:/src" -w /src tenbox-build:jammy-${ARCH} \
    bash -lc 'cmake -B build -DCMAKE_BUILD_TYPE=Release -DTENBOX_STATIC_FFMPEG=ON \
              && cmake --build build --parallel'
```

The image bundles `cmake / ninja / nasm / yasm / pkg-config / libssl-dev`,
plus the prebuilt static libs under `/opt/tenbox-deps`. tenbox CMake picks
them up automatically when `-DTENBOX_STATIC_FFMPEG=ON` is set
(`PKG_CONFIG_PATH` is exported in the image env).

The arm64 image is the same Dockerfile; just build it on an arm64 runner
(`ubuntu-22.04-arm`) — Ubuntu 22.04 base resolves to arm64 there
automatically.

GPL note: x264 is GPL. tenbox is GPLv3 (see top-level `LICENSE`), so
static linking is fine. Do not switch the build to OpenH264 unless tenbox
itself relicenses.
