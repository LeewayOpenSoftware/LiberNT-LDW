
add_executable(elfdump elfdump.c)
set_module_type(elfdump win32cui UNICODE)
add_importlibs(elfdump advapi32 psapi msvcrt kernel32)
add_cd_file(TARGET elfdump DESTINATION reactos/system32 FOR all)
