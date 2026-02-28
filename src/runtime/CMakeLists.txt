add_executable(tenbox-vm-runtime
    ${CMAKE_SOURCE_DIR}/src/runtime/main.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/runtime_service.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/runtime.rc
)

target_include_directories(tenbox-vm-runtime
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}
)

target_link_libraries(tenbox-vm-runtime
    PRIVATE
        tenbox_core
        tenbox_ipc
        WinHvPlatform
        WinHvEmulation
        ws2_32
)
