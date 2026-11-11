
list(APPEND SOURCE
    notify.c
    tables.c
    testlist.c)

add_executable(nsi_apitest ${SOURCE})
target_include_directories(nsi_apitest PRIVATE ${REACTOS_SOURCE_DIR}/sdk/include)
target_link_libraries(nsi_apitest uuid wine ${PSEH_LIB})
set_module_type(nsi_apitest win32cui)
add_importlibs(nsi_apitest nsi iphlpapi ws2_32 advapi32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET nsi_apitest)
