add_executable(test_qcow2
    test_qcow2.cpp
    ${CMAKE_SOURCE_DIR}/src/core/disk/qcow2.cpp
    ${CMAKE_SOURCE_DIR}/src/core/disk/qcow2_check.cpp
    ${CMAKE_SOURCE_DIR}/src/core/disk/disk_image.cpp
    ${CMAKE_SOURCE_DIR}/src/core/disk/disk_worker.cpp
    ${CMAKE_SOURCE_DIR}/src/core/disk/raw_image.cpp
)

target_include_directories(test_qcow2 PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${zlib_SOURCE_DIR}
    ${zlib_BINARY_DIR}
    ${zstd_SOURCE_DIR}/lib
)

target_link_libraries(test_qcow2 PRIVATE zlibstatic libzstd_static)

find_program(SH_EXECUTABLE sh REQUIRED)
add_test(
    NAME test_agent_tools_guest
    COMMAND ${SH_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_agent_tools_guest.sh
        ${CMAKE_SOURCE_DIR}/src/agent_tools/guest/agent_tools.sh
)
