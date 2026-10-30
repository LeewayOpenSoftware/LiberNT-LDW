add_executable(utf8acp_on utf8acp.c utf8acp_on.rc)
set_module_type(utf8acp_on win32cui)
add_importlibs(utf8acp_on msvcrt kernel32)
add_rostests_file(TARGET utf8acp_on)

add_executable(utf8acp_off utf8acp.c utf8acp_off.rc)
set_module_type(utf8acp_off win32cui)
add_importlibs(utf8acp_off msvcrt kernel32)
add_rostests_file(TARGET utf8acp_off)
