# Common sources shared by all platforms
set(TENBOX_CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/vmm/vm.cpp
    ${CMAKE_SOURCE_DIR}/src/core/vmm/address_space.cpp
    ${CMAKE_SOURCE_DIR}/src/core/device/virtio/virtqueue.cpp
    ${CMAKE_SOURCE_DIR}/src/core/device/virtio/virtio_mmio.cpp
    ${CMAKE_SOURCE_DIR}/src/core/device/virtio/virtio_blk.cpp
    ${CMAKE_SOURCE_DIR}/src/core/disk/disk_image.cpp
    ${CMAKE_SOURCE_DIR}/src/core/disk/disk_worker.cpp
    ${CMAKE_SOURCE_DIR}/src/core/disk/raw_image.cpp
    ${CMAKE_SOURCE_DIR}/src/core/disk/qcow2.cpp
    ${CMAKE_SOURCE_DIR}/src/core/device/virtio/virtio_net.cpp
    ${CMAKE_SOURCE_DIR}/src/core/device/virtio/virtio_input.cpp
    ${CMAKE_SOURCE_DIR}/src/core/device/virtio/virtio_gpu.cpp
    ${CMAKE_SOURCE_DIR}/src/core/device/virtio/virtio_serial.cpp
    ${CMAKE_SOURCE_DIR}/src/core/device/virtio/virtio_fs.cpp
    ${CMAKE_SOURCE_DIR}/src/core/device/virtio/virtio_snd.cpp
    ${CMAKE_SOURCE_DIR}/src/core/vdagent/vdagent_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/core/guest_agent/guest_agent_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/core/net/net_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/core/net/net_dhcp.cpp
    ${CMAKE_SOURCE_DIR}/src/core/net/net_nat.cpp
    ${CMAKE_SOURCE_DIR}/src/core/net/net_icmp.cpp
    ${CMAKE_SOURCE_DIR}/src/core/net/net_port_forward.cpp
)

# Architecture-specific sources
if(WIN32 OR CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    list(APPEND TENBOX_CORE_SOURCES
        ${CMAKE_SOURCE_DIR}/src/core/arch/x86_64/boot.cpp
        ${CMAKE_SOURCE_DIR}/src/core/arch/x86_64/acpi.cpp
        ${CMAKE_SOURCE_DIR}/src/core/arch/x86_64/x86_machine.cpp
        ${CMAKE_SOURCE_DIR}/src/core/device/serial/uart_16550.cpp
        ${CMAKE_SOURCE_DIR}/src/core/device/timer/i8254_pit.cpp
        ${CMAKE_SOURCE_DIR}/src/core/device/rtc/cmos_rtc.cpp
        ${CMAKE_SOURCE_DIR}/src/core/device/irq/ioapic.cpp
        ${CMAKE_SOURCE_DIR}/src/core/device/irq/local_apic.cpp
        ${CMAKE_SOURCE_DIR}/src/core/device/acpi/acpi_pm.cpp
    )
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    list(APPEND TENBOX_CORE_SOURCES
        ${CMAKE_SOURCE_DIR}/src/core/arch/aarch64/boot.cpp
        ${CMAKE_SOURCE_DIR}/src/core/arch/aarch64/fdt_builder.cpp
        ${CMAKE_SOURCE_DIR}/src/core/arch/aarch64/pl011.cpp
        ${CMAKE_SOURCE_DIR}/src/core/arch/aarch64/aarch64_machine.cpp
        ${CMAKE_SOURCE_DIR}/src/core/device/rtc/pl031_rtc.cpp
    )
endif()

add_library(tenbox_core STATIC ${TENBOX_CORE_SOURCES})
target_include_directories(tenbox_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}
        ${zlib_SOURCE_DIR}
        ${zlib_BINARY_DIR}
        ${zstd_SOURCE_DIR}/lib
        ${LWIP_INCLUDE_DIRS}
)
if(APPLE)
    add_dependencies(tenbox_core lwipcore)
    target_link_libraries(tenbox_core
        PUBLIC
            zlibstatic
            libzstd_static
            -Wl,-force_load,$<TARGET_FILE:lwipcore>
            uv_a
    )
else()
    target_link_libraries(tenbox_core
        PUBLIC
            zlibstatic
            libzstd_static
            lwipcore
            uv_a
    )
endif()
