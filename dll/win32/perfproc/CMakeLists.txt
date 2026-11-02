
spec2def(perfproc.dll perfproc.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    perfproc.c
    ../pdh/perfdata.c
    ${CMAKE_CURRENT_BINARY_DIR}/perfproc_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/perfproc.def)

add_library(perfproc MODULE ${SOURCE})
set_module_type(perfproc win32dll)
target_compile_definitions(perfproc PRIVATE __WINESRC__)
target_include_directories(perfproc PRIVATE ../pdh)
target_link_libraries(perfproc wine)
add_importlibs(perfproc msvcrt kernel32_vista kernel32 ntdll)
add_cd_file(TARGET perfproc DESTINATION reactos/system32 FOR all)
set_wine_module(perfproc)
