
list(APPEND SOURCE
    BufferedRecv.c
    BusyLoopback.c
    CloseCompletion.c
    MulticastInterface.c
    ReuseDelivery.c
    accept_backlog.c
    addresslistsort.c
    afunix.c
    bind.c
    broadcast.c
    cancel.c
    close.c
    connect_select.c
    dualstack.c
    getaddrinfo.c
    gethostname.c
    getnameinfo.c
    getservbyname.c
    getservbyport.c
    helpers.c
    ioctlsocket.c
    nonblocking.c
    nostartup.c
    oob.c
    open_osfhandle.c
    pollable_event.c
    recv.c
    select_edge.c
    send.c
    shutdown_linger.c
    sockopt.c
    udp_quic.c
    WSAAsync.c
    WSAIoctl.c
    WSARecv.c
    WSAStartup.c)

list(APPEND PCH_SKIP_SOURCE
    testlist.c)

add_executable(ws2_32_apitest
    ${SOURCE}
    ${PCH_SKIP_SOURCE})

target_link_libraries(ws2_32_apitest wine ${PSEH_LIB})
set_module_type(ws2_32_apitest win32cui)
add_importlibs(ws2_32_apitest advapi32 iphlpapi user32 ws2_32 msvcrt kernel32 ntdll)
add_pch(ws2_32_apitest ws2_32.h "${PCH_SKIP_SOURCE}")
add_rostests_file(TARGET ws2_32_apitest)
