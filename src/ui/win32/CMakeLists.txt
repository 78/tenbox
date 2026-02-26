add_library(tenbox_ui_shell_win32 STATIC
    ${CMAKE_SOURCE_DIR}/src/ui/win32/win32_ui_shell.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/win32/win32_dialogs.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/win32/win32_display_panel.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/common/vm_forms.cpp
)

target_include_directories(tenbox_ui_shell_win32
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(tenbox_ui_shell_win32 PUBLIC comctl32 uxtheme)
