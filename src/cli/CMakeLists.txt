add_executable(tenbox
    ${CMAKE_SOURCE_DIR}/src/cli/main.cpp
)

target_include_directories(tenbox
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}
)

target_link_libraries(tenbox
    PRIVATE
        tenbox_client
        nlohmann_json::nlohmann_json
)
