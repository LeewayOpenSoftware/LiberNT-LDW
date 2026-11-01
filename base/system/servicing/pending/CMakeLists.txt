
# LiberNTUpdatePending — bootstrap-only in stage 1 (owner condition 3).
# Deliberately NOT added to base/system/servicing/CMakeLists.txt: the binary
# does not enter the default build tree nor the ISO in stage 1. The
# bootstrap (base/setup/libernt-update/) builds and installs it online or
# offline. To compile it manually:
#   ninja LiberNTUpdatePending
# in a configured amd64 build after uncommenting the line below.

if(FALSE)
add_executable(LiberNTUpdatePending pending.c)
set_module_type(LiberNTUpdatePending nativecui)
add_importlibs(LiberNTUpdatePending ntdll)
add_dependencies(LiberNTUpdatePending psdk)
endif()
