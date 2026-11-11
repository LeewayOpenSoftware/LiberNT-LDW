include_directories(../include)

list(APPEND NSISTORE_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    nsi_store.c)

add_library(nsistore_drv MODULE ${NSISTORE_DRV_SOURCE})
set_module_type(nsistore_drv kernelmodedriver)
target_link_libraries(nsistore_drv kmtest_printf uuid ${PSEH_LIB})
add_importlibs(nsistore_drv netio ntoskrnl hal)
target_compile_definitions(nsistore_drv PRIVATE KMT_STANDALONE_DRIVER)
add_rostests_file(TARGET nsistore_drv)
