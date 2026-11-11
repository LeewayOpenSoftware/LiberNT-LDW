
# libmpg123 1.33.7, the tree Wine ships in libs/mpg123; the decoder behind
# l3codeca.acm and quartz. Built the way Wine builds it: generic decoder with
# dithering, float arithmetic, no signal handling.
add_definitions(
    -DOPT_GENERIC_DITHER
    -DREAL_IS_FLOAT
    -DNO_CATCHSIGNAL
    -DNOXFERMEM
    -DHAVE_CONFIG_H
    -DEOVERFLOW=75
    -Dstrcasecmp=_stricmp
    -Dstrncasecmp=_strnicmp
    -D_DLL -D__USE_CRTIMP
    -D_CRT_NO_POSIX_ERROR_CODES)

include_directories(BEFORE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compat
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libmpg123
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include)

list(APPEND SOURCE
    src/compat/compat.c
    src/compat/compat_str.c
    src/libmpg123/dct64.c
    src/libmpg123/dither.c
    src/libmpg123/equalizer.c
    src/libmpg123/format.c
    src/libmpg123/frame.c
    src/libmpg123/icy.c
    src/libmpg123/icy2utf8.c
    src/libmpg123/id3.c
    src/libmpg123/index.c
    src/libmpg123/layer1.c
    src/libmpg123/layer2.c
    src/libmpg123/layer3.c
    src/libmpg123/lfs_wrap.c
    src/libmpg123/libmpg123.c
    src/libmpg123/ntom.c
    src/libmpg123/parse.c
    src/libmpg123/readers.c
    src/libmpg123/stringbuf.c
    src/libmpg123/synth.c
    src/libmpg123/synth_8bit.c
    src/libmpg123/synth_real.c
    src/libmpg123/synth_s32.c
    src/libmpg123/tabinit.c)

list(APPEND PCH_SKIP_SOURCE
    src/libmpg123/optimize.c)

add_library(libmpg123
    ${SOURCE}
    ${PCH_SKIP_SOURCE})

# consumers include <mpg123.h> from here
target_include_directories(libmpg123 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/include)
target_link_libraries(libmpg123 oldnames)
add_dependencies(libmpg123 psdk)
add_pch(libmpg123 precomp.h "${PCH_SKIP_SOURCE}")
