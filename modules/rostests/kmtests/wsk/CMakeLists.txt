include_directories(../include)

list(APPEND WSKECHO_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    wsk_echo.c)

add_library(wskecho_drv MODULE ${WSKECHO_DRV_SOURCE})
set_module_type(wskecho_drv kernelmodedriver)
target_link_libraries(wskecho_drv kmtest_printf uuid ${PSEH_LIB})
add_importlibs(wskecho_drv netio ntoskrnl hal)
target_compile_definitions(wskecho_drv PRIVATE KMT_STANDALONE_DRIVER)
add_rostests_file(TARGET wskecho_drv)
