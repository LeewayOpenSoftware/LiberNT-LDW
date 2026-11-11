# PROJECT:     LiberNT host-native tests
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     cmlib (configuration-manager hive library) host-test build
# COPYRIGHT:   Copyright 2026 Leeway Open Software
#
# Standalone, like submodules/host-tests/ntfslib: compiles the real cmlib sources
# with the host shim (CMLIB_HOST) and links the HvMarkCellDirty regression against
# them. Configure with -S submodules/host-tests/cmlib.

cmake_minimum_required(VERSION 3.16)
project(cmlib_host_tests LANGUAGES C)
include(CTest)

get_filename_component(REACTOS_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(CMLIB_DIR "${REACTOS_SOURCE_ROOT}/sdk/lib/cmlib")

# Same translation units and definitions the in-tree cmlibhost library uses.
set(CMLIB_SOURCES
    cmcheck.c cminit.c cmheal.c cmindex.c cmkeydel.c cmname.c cmse.c cmvalue.c
    hivebin.c hivecell.c hiveinit.c hivelog.c hivesum.c hivewrt.c)
list(TRANSFORM CMLIB_SOURCES PREPEND "${CMLIB_DIR}/")

# cmlib_host_support.c pulls in the real Rtl bitmap code (sdk/lib/rtl/bitmap.c via
# #include <bitmap.c>) and the hive allocator/flush callbacks, exactly as mkhive does.
add_library(cmlib_host STATIC ${CMLIB_SOURCES} cmlib_host_support.c)
target_include_directories(cmlib_host PUBLIC
    "${REACTOS_SOURCE_ROOT}/sdk/include/host"
    "${REACTOS_SOURCE_ROOT}/sdk/include"
    "${REACTOS_SOURCE_ROOT}/sdk/include/psdk"
    "${REACTOS_SOURCE_ROOT}/sdk/lib/rtl"
    "${CMLIB_DIR}")
target_compile_definitions(cmlib_host PUBLIC
    _NTSYSTEM_ NASSERT NTDDI_VERSION=0x0A000010 CMLIB_HOST __NO_CTYPE_INLINES TARGET_amd64)
if(NOT MSVC)
    target_compile_options(cmlib_host PRIVATE -fshort-wchar -Wno-multichar)
endif()

option(CMLIB_SANITIZE "Build the cmlib host tests under ASan/UBSan" OFF)
if(CMLIB_SANITIZE AND NOT MSVC)
    target_compile_options(cmlib_host PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(cmlib_host PUBLIC -fsanitize=address,undefined)
endif()

add_executable(cmlib_mark_dirty t_hvmarkdirty.c)
target_link_libraries(cmlib_mark_dirty PRIVATE cmlib_host)
if(NOT MSVC)
    target_compile_options(cmlib_mark_dirty PRIVATE -fshort-wchar -Wall -Wextra -g)
endif()
add_test(NAME cmlib_mark_dirty COMMAND cmlib_mark_dirty)
set_tests_properties(cmlib_mark_dirty PROPERTIES TIMEOUT 30)

# Self-contained arithmetic guard for the discriminator (no cmlib link).
add_executable(cmlib_dirty_arith t_dirty_arith.c)
if(NOT MSVC)
    target_compile_options(cmlib_dirty_arith PRIVATE -Wall -Wextra -O2)
endif()
add_test(NAME cmlib_dirty_arith COMMAND cmlib_dirty_arith)
set_tests_properties(cmlib_dirty_arith PROPERTIES TIMEOUT 20)
