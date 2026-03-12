set(RUNTIME_SOURCES
    ${CMAKE_SOURCE_DIR}/src/runtime/main.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/runtime_service.cpp
)

if(WIN32)
    list(APPEND RUNTIME_SOURCES
        ${CMAKE_SOURCE_DIR}/src/runtime/runtime.rc
        ${CMAKE_SOURCE_DIR}/src/runtime/app.manifest
    )
endif()

add_executable(tenbox-vm-runtime ${RUNTIME_SOURCES})

target_include_directories(tenbox-vm-runtime
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}
)

target_link_libraries(tenbox-vm-runtime
    PRIVATE
        tenbox_platform
        tenbox_core
        tenbox_ipc
)

if(WIN32)
    target_link_libraries(tenbox-vm-runtime PRIVATE ws2_32)
elseif(APPLE)
    target_link_libraries(tenbox-vm-runtime PRIVATE
        "-framework Hypervisor"
        "-framework Foundation"
    )

    # Auto-sign with hypervisor entitlement after every build
    set(ENTITLEMENTS_FILE "${CMAKE_SOURCE_DIR}/runtime.entitlements")
    add_custom_command(TARGET tenbox-vm-runtime POST_BUILD
        COMMAND codesign --force --sign - --entitlements "${ENTITLEMENTS_FILE}"
                "$<TARGET_FILE:tenbox-vm-runtime>"
        COMMENT "Signing tenbox-vm-runtime with hypervisor entitlement"
    )
endif()
