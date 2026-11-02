
# wmiprobe - WMI live-data probe (observability objective C, local only).
# Not added to the ISO (owner: no ISO integration without approval); the build
# produces the exe under the build dir for offline injection into a test VM.
add_executable(wmiprobe wmiprobe.c)
set_module_type(wmiprobe win32cui UNICODE)
target_link_libraries(wmiprobe uuid)
add_importlibs(wmiprobe advapi32 oleaut32 ole32 psapi msvcrt kernel32)
