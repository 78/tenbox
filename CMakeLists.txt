cmake_minimum_required(VERSION 3.20)
project(TenClaw LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# zlib — required for qcow2 compressed cluster support
include(FetchContent)
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zlib)

set(SOURCES
    src/main.cpp
    src/vmm/vm.cpp
    src/vmm/address_space.cpp
    src/hypervisor/whvp_platform.cpp
    src/hypervisor/whvp_vm.cpp
    src/hypervisor/whvp_vcpu.cpp
    src/arch/x86_64/boot.cpp
    src/device/serial/uart_16550.cpp
    src/device/timer/i8254_pit.cpp
    src/device/rtc/cmos_rtc.cpp
    src/device/irq/ioapic.cpp
    src/arch/x86_64/acpi.cpp
    src/device/acpi/acpi_pm.cpp
    src/device/virtio/virtqueue.cpp
    src/device/virtio/virtio_mmio.cpp
    src/device/virtio/virtio_blk.cpp
    src/device/virtio/disk_image.cpp
    src/device/virtio/raw_image.cpp
    src/device/virtio/qcow2.cpp
)

add_executable(TenClaw ${SOURCES})
target_include_directories(TenClaw PRIVATE src/)

target_include_directories(TenClaw PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
target_link_libraries(TenClaw PRIVATE WinHvPlatform WinHvEmulation zlibstatic)
