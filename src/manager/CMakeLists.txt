add_executable(tenbox-manager WIN32
    ${CMAKE_SOURCE_DIR}/src/manager/main.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/manager_service.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/app_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/app.manifest
    ${CMAKE_SOURCE_DIR}/src/manager/toolbar.rc
)

target_include_directories(tenbox-manager
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}
)

target_link_libraries(tenbox-manager
    PRIVATE
        tenbox_ipc
        tenbox_ui_shell_win32
        nlohmann_json::nlohmann_json
        shell32
        advapi32
)

if(MSVC)
    target_link_options(tenbox-manager PRIVATE /ENTRY:mainCRTStartup)
endif()
