add_executable(tenbox-manager WIN32
    ${CMAKE_SOURCE_DIR}/src/manager/main.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/manager_service.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/app_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/image_source.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/http_download.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/update_checker.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/app.manifest
    ${CMAKE_SOURCE_DIR}/src/manager/toolbar.rc
    # UI layer
    ${CMAKE_SOURCE_DIR}/src/manager/ui/win32_ui_shell.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/ui/create_vm_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/ui/edit_vm_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/ui/shared_folders_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/ui/update_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/ui/port_forward_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/ui/settings_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/ui/win32_display_panel.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/ui/info_tab.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/ui/console_tab.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/ui/vm_listview.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/audio/wasapi_audio_player.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/vm_forms.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/i18n.cpp
)

target_include_directories(tenbox-manager
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}
)

target_link_libraries(tenbox-manager
    PRIVATE
        tenbox_ipc
        nlohmann_json::nlohmann_json
        shell32
        advapi32
        comctl32
        uxtheme
        ole32
        winhttp
        bcrypt
)

if(MSVC)
    # Mitigate Defender heuristic false positives (Wacatac.H!ml):
    # - /GL whole-program optimization produces cleaner code layout
    # - /Gy function-level linking for standard binary structure
    # - /guard:cf Control Flow Guard adds MS-recognized security metadata
    target_compile_options(tenbox-manager PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:/GL /Gy /guard:cf /sdl>
    )
    target_link_options(tenbox-manager PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:/LTCG /GUARD:CF /DYNAMICBASE /NXCOMPAT /CETCOMPAT>
    )
endif()
