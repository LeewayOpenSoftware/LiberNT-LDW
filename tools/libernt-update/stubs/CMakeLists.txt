# Boot-class lab fixtures (LAB-S1-BOOTCLASS-JOB.md / LAB04-S1-BOOTCLASS-HANDOFF.md).
#
# TEST FIXTURES, never shipped: no add_cd_file(), not on the ISO. Built only when explicitly asked for, so
# the default tree and the images never contain them:
#   configure ... -DLIBERNT_BUILD_S1_LABSTUBS=ON
#   ninja lnfailstub lnfailstub_smss lnfailstub_win32 lnokstub
# base/system/CMakeLists.txt add_subdirectory()s this folder unconditionally (like servicing/pending); the
# option below guards every target, so OFF (the default) builds nothing here. CI turns it ON to publish the
# stubs as separate artifacts for the Lab04 boot-class job (artifact-only: Lab04 builds no LiberNT code).
#
#   lnfailstub       (.sys)   broken boot-start/system-start driver: DriverEntry returns a failure -> D2/D3
#   lnfailstub_smss  (native) broken session manager: NtProcessStartup self-terminates          -> G2
#   lnfailstub_win32 (win32)  broken critical process: logs to COM1 and returns 1               -> G3 (and csrss/wininit/services/lsass)
#   lnokstub         (.sys)   good driver: DriverEntry returns STATUS_SUCCESS                    -> D1
# The good trio/ntdll/win32k/winlogon legs (K1/G1) use the real image build, not stubs.

option(LIBERNT_BUILD_S1_LABSTUBS "Build the boot-class lab fixture stubs (not part of the ISO)" OFF)

if(LIBERNT_BUILD_S1_LABSTUBS)

add_library(lnfailstub MODULE lnfailstub_driver.c)
set_module_type(lnfailstub kernelmodedriver)
add_importlibs(lnfailstub ntoskrnl hal)

add_library(lnokstub MODULE lnokstub_driver.c)
set_module_type(lnokstub kernelmodedriver)
add_importlibs(lnokstub ntoskrnl hal)

add_executable(lnfailstub_smss WIN32 lnfailstub_native.c)
target_link_libraries(lnfailstub_smss nt)
set_module_type(lnfailstub_smss nativecui)
# ntdll supplies the native syscall imports (NtTerminateProcess), as smss and the applier do.
add_importlibs(lnfailstub_smss ntdll)

add_executable(lnfailstub_win32 lnfailstub_win32.c)
set_module_type(lnfailstub_win32 win32cui)
# msvcrt supplies the win32cui entry (mainCRTStartup) and __main, as the CLI (libernt-update) does.
add_importlibs(lnfailstub_win32 msvcrt kernel32)

endif()
