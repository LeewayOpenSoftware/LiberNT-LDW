/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Network store (NSI) provider and table access test, declarations
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#ifndef _KMTEST_NSI_STORE_H_
#define _KMTEST_NSI_STORE_H_

#define IOCTL_NSISTORE_RUN 1
#define IOCTL_NSISTORE_NOTIFY 2

#define NSISTORE_TABLE 7
#define NSISTORE_SCRATCH_TABLE 8
#define NSISTORE_ROWS 2

#define NSISTORE_MODULE_GUID { 0x5b2f3c1a, 0x9d4e, 0x4a7b, { 0x8c, 0x11, 0x6e, 0x2d, 0x4f, 0x90, 0xa1, 0x53 } }

typedef struct _NSISTORE_RW
{
    ULONG A;
    ULONG B;
} NSISTORE_RW;

typedef struct _NSISTORE_DYNAMIC
{
    ULONG64 Counter;
    ULONG X;
    ULONG Y;
} NSISTORE_DYNAMIC;

typedef struct _NSISTORE_STATIC
{
    ULONG S[2];
} NSISTORE_STATIC;

#define NSISTORE_RW_A(Key) ((Key) * 10 + 1)
#define NSISTORE_RW_B(Key) ((Key) * 10 + 2)
#define NSISTORE_COUNTER(Key) ((ULONG64)(Key) * 1000)
#define NSISTORE_X(Key) ((Key) * 10 + 3)
#define NSISTORE_Y(Key) ((Key) * 10 + 4)
#define NSISTORE_S0(Key) ((Key) * 10 + 5)
#define NSISTORE_S1(Key) ((Key) * 10 + 6)

#endif
