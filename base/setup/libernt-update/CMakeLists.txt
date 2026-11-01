
# LiberNTUpdateBootstrap — installs/uninstalls the stage-1 LiberNT Update
# infrastructure (boot applier, servicing tree, BootExecute, service).
# Built in the tree for verification; deliberately not added to the ISO
# (owner condition: no ISO integration without approval).

include_directories(
    ${REACTOS_SOURCE_DIR}/base/system/servicing/pending)

list(APPEND SOURCE
    bootstrap.h
    bundle.c
    main.c
    multisz.c
    reg.c
    selftest.c
    service.c
    target.c
    ${REACTOS_SOURCE_DIR}/base/system/servicing/pending/s1core.c)

add_executable(libernt-update-bootstrap ${SOURCE})
set_module_type(libernt-update-bootstrap win32cui UNICODE)
add_importlibs(libernt-update-bootstrap advapi32 shell32 ole32 msvcrt kernel32 ntdll)
