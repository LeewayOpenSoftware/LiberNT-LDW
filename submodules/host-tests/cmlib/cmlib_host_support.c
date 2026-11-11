/*
 * PROJECT:     LiberNT host-native tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Host support for the cmlib tests: the real Rtl bitmap implementation
 *              plus the small host shims cmlib links against (string, bit-scan and
 *              allocator/flush routines), the same set mkhive provides for host use.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdlib.h>
#include <stdio.h>

/* cmlib.h brings the host SAL set the cmlib sources themselves compile with, so
 * including it before bitmap.c defines _In_/_Out_ without pulling the <sal.h>
 * chain (sdk/include/psdk/specstrings.h -> sal.h), which is absent on a Linux
 * host. __drv_aliasesMem is the only driver annotation bitmap.c uses. */
#include <typedefs.h>
#include <cmlib.h>
#ifndef __drv_aliasesMem
#define __drv_aliasesMem
#endif

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* Signatures match bitmap.c's callers (BITMAP_INDEX is 32-bit; the 64-bit wrappers
 * write an `unsigned long`), so this is correct on both LLP64 and LP64. */
unsigned char BitScanForward(unsigned int *Index, unsigned int Mask)
{
    if (!Mask) return 0;
    *Index = (unsigned int)__builtin_ctz(Mask);
    return 1;
}
unsigned char BitScanReverse(unsigned int *Index, unsigned int Mask)
{
    if (!Mask) return 0;
    *Index = 31u - (unsigned int)__builtin_clz(Mask);
    return 1;
}
unsigned char BitScanForward64(unsigned int *Index, unsigned long long Mask)
{
    if (!Mask) return 0;
    *Index = (unsigned int)__builtin_ctzll(Mask);
    return 1;
}
unsigned char BitScanReverse64(unsigned int *Index, unsigned long long Mask)
{
    if (!Mask) return 0;
    *Index = 63u - (unsigned int)__builtin_clzll(Mask);
    return 1;
}

VOID NTAPI RtlFillMemoryUlong(PVOID Destination, SIZE_T Length, ULONG Pattern)
{
    PULONG p = (PULONG)Destination;
    SIZE_T n = Length / sizeof(ULONG);
    while (n--) *p++ = Pattern;
}
VOID NTAPI RtlFillMemoryUlonglong(PVOID Destination, SIZE_T Length, ULONGLONG Pattern)
{
    ULONGLONG *p = (ULONGLONG *)Destination;
    SIZE_T n = Length / sizeof(ULONGLONG);
    while (n--) *p++ = Pattern;
}

/* The real kernel bitmap routines (RtlInitializeBitMap, RtlSetBits, RtlClearBits,
 * RtlFindSetBits, RtlClearAllBits, RtlCheckBit). */
#include <bitmap.c>

VOID NTAPI RtlInitUnicodeString(PUNICODE_STRING Dest, PCWSTR Source)
{
    if (Source)
    {
        SIZE_T n = 0;
        while (Source[n]) n++;
        Dest->Length = (USHORT)(n * sizeof(WCHAR));
        Dest->MaximumLength = (USHORT)(Dest->Length + sizeof(WCHAR));
    }
    else
    {
        Dest->Length = 0;
        Dest->MaximumLength = 0;
    }
    Dest->Buffer = (PWCHAR)Source;
}

WCHAR NTAPI RtlUpcaseUnicodeChar(WCHAR Source)
{
    if (Source >= 'a' && Source <= 'z')
        return (WCHAR)(Source - ('a' - 'A'));
    return Source;
}

LONG NTAPI RtlCompareUnicodeString(PCUNICODE_STRING String1, PCUNICODE_STRING String2, BOOLEAN CaseInSensitive)
{
    USHORT i;
    WCHAR c1, c2;
    for (i = 0; i <= String1->Length / sizeof(WCHAR) && i <= String2->Length / sizeof(WCHAR); i++)
    {
        c1 = CaseInSensitive ? RtlUpcaseUnicodeChar(String1->Buffer[i]) : String1->Buffer[i];
        c2 = CaseInSensitive ? RtlUpcaseUnicodeChar(String2->Buffer[i]) : String2->Buffer[i];
        if (c1 < c2) return -1;
        if (c1 > c2) return 1;
    }
    return 0;
}

VOID NTAPI KeQuerySystemTime(PLARGE_INTEGER CurrentTime)
{
    CurrentTime->QuadPart = 0;
}

VOID NTAPI KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4)
{
    fprintf(stderr, "KeBugCheckEx 0x%lx (%p %p %p %p)\n", (unsigned long)Code,
            (void *)P1, (void *)P2, (void *)P3, (void *)P4);
    abort();
}

PVOID NTAPI CmpAllocate(SIZE_T Size, BOOLEAN Paged, ULONG Tag)
{
    (void)Paged; (void)Tag;
    return malloc((size_t)Size);
}
VOID NTAPI CmpFree(PVOID Ptr, ULONG Quota)
{
    (void)Quota;
    free(Ptr);
}
BOOLEAN CMAPI CmpFileFlush(PHHIVE RegistryHive, ULONG FileType, PLARGE_INTEGER FileOffset, ULONG Length)
{
    (void)RegistryHive; (void)FileType; (void)FileOffset; (void)Length;
    return TRUE;
}
