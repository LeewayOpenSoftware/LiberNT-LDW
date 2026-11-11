add_executable(leewayui_ribbon_test
    ribbon_test.cpp
    ribbon_test.rc
)

set_module_type(leewayui_ribbon_test win32gui UNICODE)

add_definitions(-DUNICODE -D_UNICODE)

target_link_libraries(leewayui_ribbon_test
    leewayui_static
)

add_importlibs(leewayui_ribbon_test user32 gdi32 advapi32 uxtheme msvcrt kernel32)

add_dependencies(leewayui_ribbon_test psdk)

add_cd_file(TARGET leewayui_ribbon_test DESTINATION reactos/bin FOR all)
