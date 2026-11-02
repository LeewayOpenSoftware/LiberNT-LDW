/*
 * PROJECT:     LiberNT Event Log dump tool
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Read a classic event log with OpenEventLog/ReadEventLog and print
 *              every record: number, time, type, event ID, source, computer,
 *              insertion strings and raw data. Used to check what the Event Log
 *              service really stored, independently of any viewer.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <psapi.h>

static const WCHAR *
TypeName(WORD Type)
{
    switch (Type)
    {
        case EVENTLOG_SUCCESS:          return L"Success";
        case EVENTLOG_ERROR_TYPE:       return L"Error";
        case EVENTLOG_WARNING_TYPE:     return L"Warning";
        case EVENTLOG_INFORMATION_TYPE: return L"Information";
        case EVENTLOG_AUDIT_SUCCESS:    return L"AuditSuccess";
        case EVENTLOG_AUDIT_FAILURE:    return L"AuditFailure";
    }
    return L"Unknown";
}

static void
PrintRecord(const EVENTLOGRECORD *Rec)
{
    const BYTE *Base = (const BYTE *)Rec;
    const WCHAR *Source = (const WCHAR *)(Base + sizeof(EVENTLOGRECORD));
    const WCHAR *Computer = Source + wcslen(Source) + 1;
    const WCHAR *Str = (const WCHAR *)(Base + Rec->StringOffset);
    FILETIME Ft;
    SYSTEMTIME St;
    ULONGLONG T;
    WORD i;
    DWORD d;

    /* seconds since 1970 -> FILETIME (100 ns since 1601) */
    T = ((ULONGLONG)Rec->TimeGenerated + 11644473600ULL) * 10000000ULL;
    Ft.dwLowDateTime = (DWORD)T;
    Ft.dwHighDateTime = (DWORD)(T >> 32);
    FileTimeToSystemTime(&Ft, &St);

    wprintf(L"Record %lu  %04u-%02u-%02u %02u:%02u:%02u UTC  %ls  EventID=%lu (0x%08lx)  Category=%u\n",
            Rec->RecordNumber, St.wYear, St.wMonth, St.wDay, St.wHour, St.wMinute, St.wSecond,
            TypeName(Rec->EventType), Rec->EventID & 0xFFFF, Rec->EventID, Rec->EventCategory);
    wprintf(L"  Source=%ls  Computer=%ls\n", Source, Computer);
    for (i = 0; i < Rec->NumStrings; i++)
    {
        wprintf(L"  String[%u]=%ls\n", i, Str);
        Str += wcslen(Str) + 1;
    }
    if (Rec->DataLength)
    {
        const BYTE *Data = Base + Rec->DataOffset;
        wprintf(L"  Data(%lu):", Rec->DataLength);
        for (d = 0; d < Rec->DataLength && d < 64; d++)
            wprintf(L" %02x", Data[d]);
        wprintf(Rec->DataLength > 64 ? L" ...\n" : L"\n");
    }
}

/* elfdump /storm N: report N events from this process as fast as possible (write-rate check) */
static int
Storm(DWORD N, DWORD DataSize)
{
    HANDLE h = RegisterEventSourceW(NULL, L"ObsAStorm");
    DWORD i, ok = 0, failed = 0, lastErr = 0;
    WCHAR Text[32];
    const WCHAR *Strs[1];
    BYTE *Data = NULL;

    if (DataSize != 0)
        Data = calloc(1, DataSize);
    if (DataSize != 0 && !Data)
        return 1;
    if (!h)
    {
        wprintf(L"RegisterEventSource failed: %lu\n", GetLastError());
        return 1;
    }
    Strs[0] = Text;
    for (i = 0; i < N; i++)
    {
        swprintf(Text, 32, L"storm %lu", i);
        if (ReportEventW(h, EVENTLOG_INFORMATION_TYPE, 0, 888, NULL, 1, DataSize, Strs, Data))
            ok++;
        else
        {
            failed++;
            lastErr = GetLastError();
        }
    }
    wprintf(L"Storm: %lu calls, %lu succeeded, %lu failed (last error %lu)\n", N, ok, failed, lastErr);
    DeregisterEventSource(h);
    free(Data);
    return 0;
}

/*
 * elfdump /bounds: reports events at and over the limits of the service and prints what came back.
 * Expected: the small ones succeed (the 16-byte data event can be read back with its data),
 * the ones with more than 256 strings or more than 256 KiB of data are refused.
 */
static int
Bounds(void)
{
    HANDLE h = RegisterEventSourceW(NULL, L"ObsABounds");
    static const BYTE Data16[16] = {0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const WCHAR *Strs[300];
    BYTE *Big;
    BOOL ok;
    DWORD i;

    if (!h)
    {
        wprintf(L"RegisterEventSource failed: %lu\n", GetLastError());
        return 1;
    }
    for (i = 0; i < 300; i++)
        Strs[i] = L"s";

    ok = ReportEventW(h, EVENTLOG_INFORMATION_TYPE, 0, 901, NULL, 1, 16, Strs, (PVOID)Data16);
    wprintf(L"data16 (1 string, 16 data bytes): %ls (error %lu)\n", ok ? L"ACCEPTED" : L"REFUSED", ok ? 0 : GetLastError());
    ok = ReportEventW(h, EVENTLOG_INFORMATION_TYPE, 0, 902, NULL, 256, 0, Strs, NULL);
    wprintf(L"256 strings: %ls (error %lu)\n", ok ? L"ACCEPTED" : L"REFUSED", ok ? 0 : GetLastError());
    ok = ReportEventW(h, EVENTLOG_INFORMATION_TYPE, 0, 903, NULL, 300, 0, Strs, NULL);
    wprintf(L"300 strings: %ls (error %lu)\n", ok ? L"ACCEPTED" : L"REFUSED", ok ? 0 : GetLastError());
    Big = calloc(1, 300 * 1024);
    if (Big)
    {
        ok = ReportEventW(h, EVENTLOG_INFORMATION_TYPE, 0, 904, NULL, 0, 200 * 1024, NULL, Big);
        wprintf(L"200 KiB data: %ls (error %lu)\n", ok ? L"ACCEPTED" : L"REFUSED", ok ? 0 : GetLastError());
        ok = ReportEventW(h, EVENTLOG_INFORMATION_TYPE, 0, 905, NULL, 0, 300 * 1024, NULL, Big);
        wprintf(L"300 KiB data: %ls (error %lu)\n", ok ? L"ACCEPTED" : L"REFUSED", ok ? 0 : GetLastError());
        free(Big);
    }
    DeregisterEventSource(h);
    return 0;
}

/* Process counters of the service host, for the overhead numbers */
typedef struct _ELFPROBE
{
    ULONGLONG CpuMs;        /* kernel + user */
    ULONGLONG WriteBytes;   /* bytes written by the process (IO counters) */
    ULONGLONG WriteOps;
    SIZE_T Private;         /* PrivateUsage */
    SIZE_T Working;
    ULONGLONG LogSize;      /* file size of the log being written */
} ELFPROBE;

static BOOL
ProbeSample(HANDLE Proc, const WCHAR *LogPath, ELFPROBE *P)
{
    FILETIME c, e, k, u;
    IO_COUNTERS io;
    PROCESS_MEMORY_COUNTERS_EX pmc;
    WIN32_FILE_ATTRIBUTE_DATA fad;

    memset(P, 0, sizeof(*P));
    if (!GetProcessTimes(Proc, &c, &e, &k, &u))
        return FALSE;
    P->CpuMs = (((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) +
                ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 10000;
    if (GetProcessIoCounters(Proc, &io))
    {
        P->WriteBytes = io.WriteTransferCount;
        P->WriteOps = io.WriteOperationCount;
    }
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(Proc, (PPROCESS_MEMORY_COUNTERS)&pmc, sizeof(pmc)))
    {
        P->Private = pmc.PrivateUsage;
        P->Working = pmc.WorkingSetSize;
    }
    if (GetFileAttributesExW(LogPath, GetFileExInfoStandard, &fad))
        P->LogSize = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    return TRUE;
}

static VOID
ProbePrint(const WCHAR *Tag, const ELFPROBE *B, const ELFPROBE *A, DWORD Ok)
{
    wprintf(L"[%ls] cpu_ms=%I64u write_bytes=%I64u write_ops=%I64u private=%I64u -> %I64u working=%I64u -> %I64u logfile=%I64u -> %I64u\n",
            Tag, A->CpuMs - B->CpuMs, A->WriteBytes - B->WriteBytes, A->WriteOps - B->WriteOps,
            (ULONGLONG)B->Private, (ULONGLONG)A->Private, (ULONGLONG)B->Working, (ULONGLONG)A->Working,
            B->LogSize, A->LogSize);
    if (Ok)
        wprintf(L"[%ls] per 1000 ok events: cpu_ms=%I64u write_bytes=%I64u write_ops=%I64u\n", Tag,
                (A->CpuMs - B->CpuMs) * 1000 / Ok, (A->WriteBytes - B->WriteBytes) * 1000 / Ok,
                (A->WriteOps - B->WriteOps) * 1000 / Ok);
}

/*
 * elfdump /probe: overhead of the event log service host.
 * 1. finds the host PID of "eventlog" (QueryServiceStatusEx),
 * 2. 60 s idle, 3. a burst of 400 reports (the rate limiter refuses part), 4. 1000 reports paced at 40/s.
 * Prints CPU ms, bytes/ops written (process IO counters), PrivateUsage, working set and the size of
 * AppEvent.Evt before/after each phase. Numbers are deltas, nothing is simulated.
 */
static int
Probe(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS_PROCESS ssp;
    DWORD needed, i, ok, bad;
    HANDLE proc, h;
    ELFPROBE b, a;
    WCHAR Text[32], Path[MAX_PATH];
    const WCHAR *Strs[1];

    /* the service host runs as SYSTEM: an administrator needs SeDebugPrivilege to open it */
    {
        HANDLE tok;
        TOKEN_PRIVILEGES tp;

        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &tp.Privileges[0].Luid))
                AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
            wprintf(L"SeDebugPrivilege: %ls (%lu)\n", GetLastError() == ERROR_SUCCESS ? L"enabled" : L"not held", GetLastError());
            CloseHandle(tok);
        }
    }

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    svc = scm ? OpenServiceW(scm, L"eventlog", SERVICE_QUERY_STATUS) : NULL;
    if (!svc || !QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed))
    {
        wprintf(L"cannot query the eventlog service: %lu\n", GetLastError());
        return 1;
    }
    wprintf(L"eventlog host PID = %lu\n", ssp.dwProcessId);
    proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, ssp.dwProcessId);
    if (!proc)
        proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ssp.dwProcessId);
    if (!proc)
    {
        wprintf(L"OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }
    GetSystemDirectoryW(Path, MAX_PATH);
    wcscat(Path, L"\\config\\AppEvent.Evt");

    ProbeSample(proc, Path, &b);
    Sleep(60000);
    ProbeSample(proc, Path, &a);
    wprintf(L"[idle 60 s] cpu_ms=%I64u write_bytes=%I64u private=%I64u -> %I64u\n", a.CpuMs - b.CpuMs,
            a.WriteBytes - b.WriteBytes, (ULONGLONG)b.Private, (ULONGLONG)a.Private);

    h = RegisterEventSourceW(NULL, L"ObsAProbe");
    if (!h)
        return 1;
    Strs[0] = Text;

    ok = bad = 0;
    ProbeSample(proc, Path, &b);
    for (i = 0; i < 400; i++)
    {
        swprintf(Text, 32, L"probe burst %lu", i);
        if (ReportEventW(h, EVENTLOG_INFORMATION_TYPE, 0, 889, NULL, 1, 0, Strs, NULL))
            ok++;
        else
            bad++;
    }
    ProbeSample(proc, Path, &a);
    wprintf(L"[burst] 400 calls, %lu ok, %lu refused\n", ok, bad);
    ProbePrint(L"burst", &b, &a, ok);

    Sleep(5000);
    ok = bad = 0;
    ProbeSample(proc, Path, &b);
    for (i = 0; i < 1000; i++)
    {
        swprintf(Text, 32, L"probe paced %lu", i);
        if (ReportEventW(h, EVENTLOG_INFORMATION_TYPE, 0, 890, NULL, 1, 0, Strs, NULL))
            ok++;
        else
            bad++;
        Sleep(25);
    }
    ProbeSample(proc, Path, &a);
    wprintf(L"[paced 40/s] 1000 calls, %lu ok, %lu refused\n", ok, bad);
    ProbePrint(L"paced", &b, &a, ok);

    DeregisterEventSource(h);
    CloseHandle(proc);
    return 0;
}

/* elfdump /hdr <file>: raw header of an .evt file (read with full sharing while the service has it open) */
static int
Hdr(const WCHAR *Path)
{
    HANDLE f = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    DWORD h[12], got = 0, sizelo, sizehi;

    if (f == INVALID_HANDLE_VALUE)
    {
        wprintf(L"cannot open %ls: %lu\n", Path, GetLastError());
        return 1;
    }
    sizelo = GetFileSize(f, &sizehi);
    if (!ReadFile(f, h, sizeof(h), &got, NULL) || got < sizeof(h))
    {
        wprintf(L"cannot read the header: %lu\n", GetLastError());
        CloseHandle(f);
        return 1;
    }
    wprintf(L"file size=%lu header: HeaderSize=%lu Signature=%08lx Major=%lu Minor=%lu StartOffset=%lu EndOffset=%lu "
            L"CurrentRecord=%lu OldestRecord=%lu MaxSize=%lu Flags=%lx Retention=%lu\n",
            sizelo, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9], h[10]);
    CloseHandle(f);
    return 0;
}

static int Dump(int argc, WCHAR **argv);

int wmain(int argc, WCHAR **argv)
{
    if (argc > 2 && !wcscmp(argv[1], L"/storm"))
        return Storm((DWORD)_wtoi(argv[2]), (argc > 3) ? (DWORD)_wtoi(argv[3]) : 0);
    if (argc > 1 && !wcscmp(argv[1], L"/bounds"))
        return Bounds();
    if (argc > 1 && !wcscmp(argv[1], L"/probe"))
        return Probe();
    if (argc > 2 && !wcscmp(argv[1], L"/hdr"))
        return Hdr(argv[2]);

    return Dump(argc, argv);
}

static int
Dump(int argc, WCHAR **argv)
{
    const WCHAR *LogName = (argc > 1) ? argv[1] : L"System";
    DWORD Max = (argc > 2) ? (DWORD)_wtoi(argv[2]) : 0xFFFFFFFF;
    DWORD Count = 0, Total = 0, Read, Needed, Size = 0x10000;
    BYTE *Buf;
    HANDLE h;

    h = OpenEventLogW(NULL, LogName);
    if (!h)
    {
        wprintf(L"OpenEventLog(%ls) failed: %lu\n", LogName, GetLastError());
        return 1;
    }
    GetNumberOfEventLogRecords(h, &Total);
    wprintf(L"Log %ls: %lu records\n", LogName, Total);

    Buf = malloc(Size);
    if (!Buf)
        return 2;

    /* newest first, like a viewer */
    while (Count < Max &&
           ReadEventLogW(h, EVENTLOG_SEQUENTIAL_READ | EVENTLOG_BACKWARDS_READ, 0, Buf, Size, &Read, &Needed))
    {
        DWORD Off = 0;
        while (Off < Read && Count < Max)
        {
            const EVENTLOGRECORD *Rec = (const EVENTLOGRECORD *)(Buf + Off);
            if (Rec->Length == 0 || Off + Rec->Length > Read)
                break;
            PrintRecord(Rec);
            Off += Rec->Length;
            Count++;
        }
    }
    if (GetLastError() != ERROR_HANDLE_EOF && Count < Max && Count < Total)
        wprintf(L"ReadEventLog stopped: %lu\n", GetLastError());

    wprintf(L"Printed %lu records\n", Count);
    free(Buf);
    CloseEventLog(h);
    return 0;
}
