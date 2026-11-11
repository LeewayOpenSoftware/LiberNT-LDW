/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test for QueryPerformanceCounter and QueryPerformanceFrequency
 */

#include "precomp.h"
#include <ndk/kefuncs.h>

#define SAMPLES 1000
#define TIMING_CALLS 100000

START_TEST(QueryPerformanceCounter)
{
    LARGE_INTEGER Frequency, SyscallFrequency, Counter, Previous, Before, After, Syscall;
    LARGE_INTEGER Start, End;
    ULONGLONG Tick, Elapsed;
    NTSTATUS Status;
    ULONG i;
    BOOL Ret;

    /* Frequency: valid and the same as the system call reports */
    Frequency.QuadPart = 0;
    Ret = QueryPerformanceFrequency(&Frequency);
    ok_int(Ret, TRUE);
    ok(Frequency.QuadPart > 0, "frequency is %I64d\n", Frequency.QuadPart);

    Status = NtQueryPerformanceCounter(&Syscall, &SyscallFrequency);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok(SyscallFrequency.QuadPart == Frequency.QuadPart,
       "syscall frequency %I64d, user mode %I64d\n",
       SyscallFrequency.QuadPart, Frequency.QuadPart);

    /* Monotonic */
    Ret = QueryPerformanceCounter(&Previous);
    ok_int(Ret, TRUE);
    for (i = 0; i < SAMPLES; i++)
    {
        Ret = QueryPerformanceCounter(&Counter);
        if (!Ret || Counter.QuadPart < Previous.QuadPart)
        {
            ok(0, "counter went backwards or failed at %lu: %I64d after %I64d\n",
               i, Counter.QuadPart, Previous.QuadPart);
            break;
        }
        Previous = Counter;
    }

    /* The user-mode value and the system call share one time base: a system
     * call read between two user-mode reads lies between them. */
    for (i = 0; i < 50; i++)
    {
        QueryPerformanceCounter(&Before);
        NtQueryPerformanceCounter(&Syscall, NULL);
        QueryPerformanceCounter(&After);
        if (Syscall.QuadPart < Before.QuadPart || Syscall.QuadPart > After.QuadPart)
        {
            ok(0, "syscall %I64d not between user-mode %I64d and %I64d (round %lu)\n",
               Syscall.QuadPart, Before.QuadPart, After.QuadPart, i);
            break;
        }
    }

    /* The rate matches the system clock: 100 ms of sleep is 100 ms of counts, within a wide margin */
    QueryPerformanceCounter(&Start);
    Tick = GetTickCount64();
    Sleep(100);
    QueryPerformanceCounter(&End);
    Tick = GetTickCount64() - Tick;
    Elapsed = (ULONGLONG)(End.QuadPart - Start.QuadPart) * 1000 / (ULONGLONG)Frequency.QuadPart;
    ok(Elapsed >= 80 && Elapsed < 1000, "100 ms sleep measured as %I64u ms (tick count %I64u ms)\n", Elapsed, Tick);

    /* Cost per call, for the log only */
    QueryPerformanceCounter(&Start);
    for (i = 0; i < TIMING_CALLS; i++)
        QueryPerformanceCounter(&Counter);
    QueryPerformanceCounter(&End);
    Elapsed = (ULONGLONG)(End.QuadPart - Start.QuadPart) * 1000000000ULL / (ULONGLONG)Frequency.QuadPart / TIMING_CALLS;
    trace("QueryPerformanceCounter: %I64u ns per call, frequency %I64d Hz\n", Elapsed, Frequency.QuadPart);

    QueryPerformanceCounter(&Start);
    for (i = 0; i < TIMING_CALLS; i++)
        NtQueryPerformanceCounter(&Counter, NULL);
    QueryPerformanceCounter(&End);
    Elapsed = (ULONGLONG)(End.QuadPart - Start.QuadPart) * 1000000000ULL / (ULONGLONG)Frequency.QuadPart / TIMING_CALLS;
    trace("NtQueryPerformanceCounter: %I64u ns per call\n", Elapsed);
}
