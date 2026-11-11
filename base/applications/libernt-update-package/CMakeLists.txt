# libernt-update-package.exe: the launcher stub a standalone update package is appended to
# (tools/libernt-update/New-UpdatePackageExe.ps1). STANDALONE like the rest of stage 1: not referenced by
# base/applications/CMakeLists.txt (see base/system/servicing/libservicing/CMakeLists.txt). The host build
# used for the stage 1 tests compiles it with clang in tools/libernt-update/Build-And-Test.ps1.

add_executable(libernt-update-package main.c)
set_module_type(libernt-update-package win32gui UNICODE)
add_importlibs(libernt-update-package shell32 user32 advapi32 msvcrt kernel32)
# no add_cd_file(): the stub ships inside packages, not on the ISO
