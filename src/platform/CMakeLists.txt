if(WIN32)
    add_library(tenbox_platform STATIC
        ${CMAKE_SOURCE_DIR}/src/platform/windows/hypervisor/whvp_platform.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/hypervisor/whvp_vm.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/hypervisor/whvp_vcpu.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/console/std_console_port.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/shared_framebuffer_win32.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/vm_platform_win.cpp
    )

    target_include_directories(tenbox_platform
        PUBLIC
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_BINARY_DIR}
    )

    target_link_libraries(tenbox_platform
        PUBLIC
            tenbox_core
            WinHvPlatform
            WinHvEmulation
    )
elseif(APPLE)
    set(HVF_COMMON_SOURCES
        ${CMAKE_SOURCE_DIR}/src/platform/macos/hypervisor/hvf_platform.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/posix/console/posix_console_port.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/macos/vm_platform_macos.cpp
    )

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(HVF_ARCH_SOURCES
            ${CMAKE_SOURCE_DIR}/src/platform/macos/hypervisor/aarch64/hvf_vm.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/macos/hypervisor/aarch64/hvf_vcpu.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/macos/hypervisor/aarch64/hvf_mmio_decode.cpp
        )
    else()
        set(HVF_ARCH_SOURCES
            ${CMAKE_SOURCE_DIR}/src/platform/macos/hypervisor/x86_64/hvf_vm.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/macos/hypervisor/x86_64/hvf_vcpu.cpp
        )
    endif()

    add_library(tenbox_platform STATIC ${HVF_COMMON_SOURCES} ${HVF_ARCH_SOURCES})

    target_include_directories(tenbox_platform
        PUBLIC
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_BINARY_DIR}
    )

    target_link_libraries(tenbox_platform
        PUBLIC
            tenbox_core
            "-framework Hypervisor"
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(KVM_COMMON_SOURCES
        ${CMAKE_SOURCE_DIR}/src/platform/linux/hypervisor/kvm_platform.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/vm_platform_linux.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/posix/console/posix_console_port.cpp
    )

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(KVM_ARCH_SOURCES
            ${CMAKE_SOURCE_DIR}/src/platform/linux/hypervisor/aarch64/kvm_vm.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/linux/hypervisor/aarch64/kvm_vcpu.cpp
        )
    else()
        set(KVM_ARCH_SOURCES
            ${CMAKE_SOURCE_DIR}/src/platform/linux/hypervisor/x86_64/kvm_vm.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/linux/hypervisor/x86_64/kvm_vcpu.cpp
        )
    endif()

    add_library(tenbox_platform STATIC ${KVM_COMMON_SOURCES} ${KVM_ARCH_SOURCES})

    target_include_directories(tenbox_platform
        PUBLIC
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_BINARY_DIR}
    )

    target_link_libraries(tenbox_platform
        PUBLIC
            tenbox_core
            pthread
    )
endif()
