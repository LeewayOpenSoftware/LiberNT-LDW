/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** config.h for the LiberNT build of libsamplerate 0.2.2 (clang/gcc/MSVC,
** Win32/Win64). Hand-written from src/config.h.in; the keys the sources
** read are the converter switches, the CPU clipping flags and the sizes.
*/

#define CPU_CLIPS_NEGATIVE 0
#define CPU_CLIPS_POSITIVE 0
#define CPU_IS_BIG_ENDIAN 0
#define CPU_IS_LITTLE_ENDIAN 1

#define ENABLE_SINC_BEST_CONVERTER 1
#define ENABLE_SINC_FAST_CONVERTER 1
#define ENABLE_SINC_MEDIUM_CONVERTER 1

#define HAVE_CALLOC 1
#define HAVE_CEIL 1
#define HAVE_FLOOR 1
#define HAVE_FMOD 1
#define HAVE_FREE 1
#define HAVE_LRINT 1
#define HAVE_LRINTF 1
#define HAVE_MALLOC 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define STDC_HEADERS 1

#define PACKAGE "libsamplerate"
#define PACKAGE_BUGREPORT "erikd@mega-nerd.com"
#define PACKAGE_NAME "libsamplerate"
#define PACKAGE_STRING "libsamplerate 0.2.2"
#define PACKAGE_TARNAME "libsamplerate"
#define PACKAGE_URL "https://github.com/libsndfile/libsamplerate/"
#define PACKAGE_VERSION "0.2.2"
#define VERSION "0.2.2"

#define SIZEOF_DOUBLE 8
#define SIZEOF_FLOAT 4
#define SIZEOF_INT 4
#define SIZEOF_LONG 4
