add_library(tenbox_client STATIC
    ${CMAKE_SOURCE_DIR}/src/client/client.cpp
)

target_include_directories(tenbox_client
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}
)

target_link_libraries(tenbox_client
    PUBLIC
        tenbox_ipc
        nlohmann_json::nlohmann_json
)
