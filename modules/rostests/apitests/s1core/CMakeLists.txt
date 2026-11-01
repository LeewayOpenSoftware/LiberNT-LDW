# Copyright 2026 Leeway Open Software
# SPDX-License-Identifier: GPL-3.0-or-later

include_directories(${REACTOS_SOURCE_DIR}/base/system/servicing/pending)

add_executable(s1core_apitest
    s1core_test.c
    ${REACTOS_SOURCE_DIR}/base/system/servicing/pending/s1core.c)
set_module_type(s1core_apitest win32cui)
target_compile_definitions(s1core_apitest PRIVATE S1_HOST_TEST)
add_importlibs(s1core_apitest msvcrt kernel32 ntdll)
add_rostests_file(TARGET s1core_apitest SUBDIR s1core)
