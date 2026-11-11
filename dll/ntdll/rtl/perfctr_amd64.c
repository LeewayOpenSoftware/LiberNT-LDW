/*
 * PROJECT:     LiberNT NT Library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     AMD64 user-mode read of the performance counter
 */

#include <ntdll.h>
#include <intrin.h>

#define RTL_AMD64_QPC_BYPASS_ENABLED 0x01

/* The kernel publishes the configuration in KUSER_SHARED_DATA: QpcBypassEnabled
 * is written last, QpcShift and QpcBias before it. The counter is
 * (TSC >> QpcShift) + QpcBias, which is exactly what the kernel returns. A
 * change of the enable byte while reading sends the caller to the system call. */
BOOLEAN
RtlpArchQueryPerformanceCounter(
    _Out_ PLARGE_INTEGER Counter)
{
    ULONGLONG Bias, Value;
    UCHAR Shift;

    if (!(*(volatile UCHAR *)&SharedUserData->QpcBypassEnabled & RTL_AMD64_QPC_BYPASS_ENABLED))
        return FALSE;

    Shift = *(volatile UCHAR *)&SharedUserData->QpcShift;
    Bias = *(volatile ULONGLONG *)&SharedUserData->QpcBias;
    _mm_lfence();
    Value = __rdtsc();

    if (!(*(volatile UCHAR *)&SharedUserData->QpcBypassEnabled & RTL_AMD64_QPC_BYPASS_ENABLED))
        return FALSE;

    Counter->QuadPart = (LONGLONG)((Value >> Shift) + Bias);
    return TRUE;
}

BOOLEAN
RtlpArchQueryPerformanceFrequency(
    _Out_ PLARGE_INTEGER Frequency)
{
    ULONGLONG Value;

    if (!(*(volatile UCHAR *)&SharedUserData->QpcBypassEnabled & RTL_AMD64_QPC_BYPASS_ENABLED))
        return FALSE;

    Value = *(volatile ULONGLONG *)&SharedUserData->QpcFrequency;
    if (!Value ||
        !(*(volatile UCHAR *)&SharedUserData->QpcBypassEnabled & RTL_AMD64_QPC_BYPASS_ENABLED))
        return FALSE;

    Frequency->QuadPart = (LONGLONG)Value;
    return TRUE;
}
