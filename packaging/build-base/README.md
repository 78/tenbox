# tenbox release build base

This image bakes the static C dependencies tenbox links against
(`ffmpeg / x264 / opus / libyuv / libcurl / openssl`) so the release
pipeline can produce binaries that run on any Debian-derived system
with `glibc >= 2.31` — Debian 11+, Ubuntu 20.04+, Raspberry Pi OS 11+,
Armbian Bullseye+, and the corresponding arm64 variants. Combined with
the `AGENTSPHERE_STATIC_RUNTIME=ON` link flags (`-static-libstdc++` /
`-static-libgcc`) the resulting deb has only `libc6 (>= 2.31)` +
`ca-certificates` as runtime deps.

The image is consumed by `.github/workflows/release.yml` and
`.github/workflows/pr-check.yml`. On the matrix runner side:

```bash
docker build -f packaging/build-base/Dockerfile.bullseye \
    -t tenbox-build:bullseye-${ARCH} \
    packaging/build-base
docker run --rm -v "$PWD:/src" -w /src tenbox-build:bullseye-${ARCH} \
    bash -lc 'cmake -B build -DCMAKE_BUILD_TYPE=Release \
              -DAGENTSPHERE_STATIC_FFMPEG=ON -DAGENTSPHERE_STATIC_RUNTIME=ON \
              && cmake --build build --parallel'
```

The image bundles `cmake / ninja / nasm / yasm / pkg-config` plus
bullseye's stock GCC 10. tenbox is on `-std=c++20` but doesn't use the
C++20 features GCC 10 lacks (`<ranges>`, header `<concepts>`,
`<format>`, `<source_location>`, coroutines), so stock GCC 10 is
sufficient. The prebuilt static libs land under `/opt/tenbox-deps`;
tenbox CMake picks them up automatically when `-DAGENTSPHERE_STATIC_FFMPEG=ON`
is set (`PKG_CONFIG_PATH` is exported in the image env, and
`OPENSSL_ROOT_DIR` is steered at the same prefix).

The arm64 image is the same Dockerfile; just build it on an arm64 runner
(`ubuntu-22.04-arm`) — `debian:11` resolves to arm64 there
automatically.

GPL note: x264 is GPL. tenbox is GPLv3 (see top-level `LICENSE`), so
static linking is fine. Do not switch the build to OpenH264 unless tenbox
itself relicenses.

## Why bullseye, not jammy?

Earlier releases used `Dockerfile.jammy` (Ubuntu 22.04, glibc 2.35).
That cut off Debian 11 / Ubuntu 20.04 hosts running on ARM BSP kernels
(RK35xx, Allwinner H6xx, …) where the vendor SoC kernel keeps the OS
stuck on bullseye for hardware-acceleration reasons. Moving the build
floor to bullseye widens the supported surface without giving anything
up: glibc symbols are forward-compatible, so the same binary still runs
on bookworm/jammy/trixie/noble. `Dockerfile.jammy` is kept in-tree for
one release cycle as a quick-rollback option, but `build-base.yml` and
both PR/release workflows now build from `Dockerfile.bullseye`. The
legacy `ghcr.io/78/tenbox-build:jammy-<arch>` image tags are kept as
aliases pointing at the bullseye image so any in-flight branch keeps
building until it merges.
