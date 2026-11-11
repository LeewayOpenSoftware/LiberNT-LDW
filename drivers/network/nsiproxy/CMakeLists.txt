
list(APPEND SOURCE
    nsiproxy.c
    nsiproxy.h)

add_library(nsiproxy MODULE ${SOURCE} nsiproxy.rc)
set_module_type(nsiproxy kernelmodedriver)
target_link_libraries(nsiproxy wdmsec)
target_compile_definitions(nsiproxy PRIVATE NDIS620)
add_importlibs(nsiproxy netio ntoskrnl hal)
add_cd_file(TARGET nsiproxy DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(nsiproxy_reg.inf)
