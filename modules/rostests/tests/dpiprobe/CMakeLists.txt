# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 Leeway Open Software
add_executable(dpiprobe dpiprobe.c)
set_module_type(dpiprobe win32cui ENTRYPOINT mainCRTStartup)
add_importlibs(dpiprobe advapi32 gdi32 user32 msvcrt kernel32)
add_rostests_file(TARGET dpiprobe)
