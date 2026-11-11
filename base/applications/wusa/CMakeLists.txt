
# wusa.exe: the Windows Update Standalone Installer of LiberNT. It reads a KBxxxx.exe package (LNPK container),
# asks, and hands it to LiberNT Update (libernt-update.exe, the stage 1 core). No link-time dependency on
# libservicing: the container header lnpk.h is the only shared file. See S1-PACKAGE-EXE-DESIGN.md section 6.
add_rc_deps(resource.rc ${CMAKE_CURRENT_SOURCE_DIR}/res/setup.ico ${CMAKE_CURRENT_SOURCE_DIR}/res/wusa.exe.manifest)
add_executable(wusa wusa.c args.c lnpkview.c resource.rc)
set_module_type(wusa win32gui UNICODE)
add_importlibs(wusa comctl32 shell32 user32 advapi32 msvcrt kernel32)
add_cd_file(TARGET wusa DESTINATION reactos/system32 FOR all)
