add_library(tenbox_ui_shell_win32 STATIC
    ${CMAKE_SOURCE_DIR}/src/ui/win32/win32_ui_shell.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/win32/win32_dialogs.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/win32/win32_display_panel.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/win32/components/info_tab.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/win32/components/console_tab.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/win32/components/vm_listbox.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/win32/components/shared_folders_tab.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/common/vm_forms.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/common/i18n.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/windows/audio/wasapi_audio_player.cpp
)

target_include_directories(tenbox_ui_shell_win32
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(tenbox_ui_shell_win32 PUBLIC comctl32 uxtheme ole32)
