# PROJECT:  LiberNT WDDM 1.x synthetic test miniport (S0 vehicle)
# LICENSE:  GPL-3.0-or-later
# PURPOSE:  Exercises dxgkrnl legacy version negotiation with own code only

spec2def(legacytest.sys legacytest.spec ADD_IMPORTLIB)

remove_definitions(-D_WIN32_WINNT=${REACTOS_TARGET_NT})
remove_definitions(-DWINVER=${REACTOS_TARGET_NT})
add_definitions(-D_WIN32_WINNT=0x0603 -DWINVER=0x0603)
add_definitions(-DNTDDI_VERSION=0x06030000)

remove_definitions(-DDXGKDDI_INTERFACE_VERSION=0x1052)
add_definitions(-DDXGKDDI_INTERFACE_VERSION=0x11007)

add_library(legacytest MODULE
    legacytest.c
    legacytest.rc
    ${CMAKE_CURRENT_BINARY_DIR}/legacytest.def)

set_module_type(legacytest kernelmodedriver)
target_link_libraries(legacytest displib)
add_importlibs(legacytest ntoskrnl hal)
add_cd_file(TARGET legacytest DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(legacytest legacytest.inf)
