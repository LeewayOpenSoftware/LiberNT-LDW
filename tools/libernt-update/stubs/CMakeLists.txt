# Boot-class lab fixtures (LAB-S1-BOOTCLASS-JOB.md). STANDALONE like the rest of stage 1: not referenced by
# any parent CMakeLists. To build the two stubs that need the kernel/native toolchain, add in a local,
# uncommitted tree:   add_subdirectory(tools/libernt-update/stubs)
# then build the targets `lnfailstub` (a .sys) and `lnfailstub_smss` (a native .exe). New-BrokenFixture.ps1
# wraps and signs them; the Win32 stub it compiles itself with clang (no build tree needed).
# These are TEST FIXTURES, never shipped: no add_cd_file(), not on the ISO.

add_library(lnfailstub MODULE lnfailstub_driver.c)
set_module_type(lnfailstub kernelmodedriver)
add_importlibs(lnfailstub ntoskrnl hal)

add_executable(lnfailstub_smss WIN32 lnfailstub_native.c)
target_link_libraries(lnfailstub_smss nt)
set_module_type(lnfailstub_smss nativecui)
