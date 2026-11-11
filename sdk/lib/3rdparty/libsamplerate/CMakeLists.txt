
list(APPEND SOURCE
    samplerate.c
    src_linear.c
    src_sinc.c
    src_zoh.c
    precomp.h)

add_library(libsamplerate ${SOURCE})
# assert() would need _assert, which the kernel consumer (kmixer, libcntpr) cannot link
target_compile_definitions(libsamplerate PRIVATE HAVE_CONFIG_H NDEBUG)
add_dependencies(libsamplerate psdk)
add_pch(libsamplerate precomp.h SOURCE)
