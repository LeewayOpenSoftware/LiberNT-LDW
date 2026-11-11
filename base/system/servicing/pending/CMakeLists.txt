
# LiberNTUpdatePending — bootstrap-only in stage 1 (owner condition 3).
# Deliberately NOT added to base/system/servicing/CMakeLists.txt: the binary
# does not enter the default build tree nor the ISO in stage 1. The
# bootstrap (base/setup/libernt-update/) builds and installs it online or
# offline. To compile it manually:
#   ninja LiberNTUpdatePending
# in a configured amd64 build after configuring with
#   -DLIBERNT_BUILD_UPDATE_PENDING=ON
# (OFF by default: the default tree and the ISO do not contain it. CI sets it
# only to publish the applier as a separate artifact for field tests).

option(LIBERNT_BUILD_UPDATE_PENDING "Build the stage-1 boot applier LiberNTUpdatePending (not part of the ISO)" OFF)

if(LIBERNT_BUILD_UPDATE_PENDING)
add_executable(LiberNTUpdatePending pending.c s1core.c)
set_module_type(LiberNTUpdatePending nativecui)
add_importlibs(LiberNTUpdatePending ntdll)
add_dependencies(LiberNTUpdatePending psdk)
endif()

# Boot-class lab fixture stubs (tools/libernt-update/stubs): target-guarded by LIBERNT_BUILD_S1_LABSTUBS
# (OFF by default, so nothing here enters the default tree or the ISO). Wired from this stage-1 CMakeLists,
# which base/system/CMakeLists.txt add_subdirectory()s unconditionally. CI turns the option ON to publish
# the stubs as separate artifacts for the Lab04 boot-class job.
add_subdirectory(${CMAKE_SOURCE_DIR}/tools/libernt-update/stubs ${CMAKE_BINARY_DIR}/tools/libernt-update/stubs)
