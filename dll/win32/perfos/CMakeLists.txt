
spec2def(perfos.dll perfos.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    perfos.c
    ../pdh/perfdata.c
    ${CMAKE_CURRENT_BINARY_DIR}/perfos_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/perfos.def)

add_library(perfos MODULE ${SOURCE})
set_module_type(perfos win32dll)
target_compile_definitions(perfos PRIVATE __WINESRC__)
target_include_directories(perfos PRIVATE ../pdh)
target_link_libraries(perfos wine)
add_importlibs(perfos msvcrt kernel32_vista kernel32 ntdll)
add_cd_file(TARGET perfos DESTINATION reactos/system32 FOR all)
set_wine_module(perfos)
