# libernt-update-ui.exe: GUI over libservicing (LiberNT Update stage 1 core).
# STANDALONE: not referenced by base/applications/CMakeLists.txt and not on the ISO; it ships in the S1
# bundle (see shared/leeway/libernt-update/ui/DESIGN.md). To build it in a local, uncommitted tree add to
# base/system/CMakeLists.txt:        add_subdirectory(servicing/libservicing)
# and to base/applications/CMakeLists.txt:   add_subdirectory(libernt-update-ui)

list(APPEND SOURCE
    main.cpp
    state.cpp)

file(GLOB libernt_update_ui_rc_deps res/*.*)
add_rc_deps(libernt-update-ui.rc ${libernt_update_ui_rc_deps})
add_executable(libernt-update-ui ${SOURCE} libernt-update-ui.rc)
set_target_properties(libernt-update-ui PROPERTIES OUTPUT_NAME "libernt-update-ui")
set_property(TARGET libernt-update-ui PROPERTY CXX_STANDARD 17)
set_property(TARGET libernt-update-ui PROPERTY CXX_STANDARD_REQUIRED ON)
set_module_type(libernt-update-ui win32gui UNICODE)
if(MSVC)
    set_target_cpp_properties(libernt-update-ui WITH_EXCEPTIONS)
endif()
target_link_libraries(libernt-update-ui shellframe libservicing cppstl cpprt uuid)
add_importlibs(libernt-update-ui advapi32 user32 gdi32 gdiplus shell32 shlwapi comctl32 dwmapi uxtheme ole32
               wintrust crypt32 msvcrt kernel32 ntdll)
