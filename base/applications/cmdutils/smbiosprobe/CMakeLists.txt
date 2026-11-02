# smbiosprobe - SMBIOS vs WMI probe (observability objective C, local only; not added to the ISO).
add_executable(smbiosprobe smbiosprobe.c)
set_module_type(smbiosprobe win32cui UNICODE)
target_link_libraries(smbiosprobe uuid)
add_importlibs(smbiosprobe advapi32 oleaut32 ole32 msvcrt kernel32)
