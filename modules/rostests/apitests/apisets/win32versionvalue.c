/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     API-set resolution must not depend on the image's Win32VersionValue (LBI WS-D test T-08)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * A PE image can carry a nonzero Win32VersionValue (optional header, offset 0x34). The system
 * then reports the overridden version to that process through RtlGetVersion/GetVersionEx (PEB),
 * but the API-set schema stays the system's. The test copies its own executable, patches the
 * field of the copy to 6.1.7601 and runs the copy as a child; the child reports
 *   - the version it sees (the override must still be honoured: nothing removed)
 *   - IsApiSetImplemented() for a fixed list of contracts
 * through a file named by an environment variable. The parent compares with its own answers.
 */

#include <ndk/umtypes.h>
#include <ndk/rtlfuncs.h>
#include <apitest.h>
#include <stdio.h>

#define WVV_ENV     L"APISET_WVV_OUT"
#define WVV_VALUE   0x1DB10106UL /* 6.1 build 7601, platform 2: major=v&0xFF minor=(v>>8)&0xFF build=(v>>16)&0x3FFF */

typedef BOOL (WINAPI *PISAPISETIMPLEMENTED)(PCSTR);

/* Present in the Win10 schema table and, for most, absent from the Win7 table. */
static const PCSTR g_Contracts[] =
{
    "api-ms-win-core-heap-l1-2-0",
    "api-ms-win-core-file-l1-2-0",
    "api-ms-win-core-processthreads-l1-1-2",
    "api-ms-win-core-synch-l1-2-0",
    "api-ms-win-core-libraryloader-l1-2-0",
    "api-ms-win-core-registry-l1-1-0",
    "api-ms-win-core-memory-l1-1-1",
    "api-ms-win-core-sysinfo-l1-2-0",
    "api-ms-win-core-errorhandling-l1-1-1",
    "api-ms-win-core-interlocked-l1-1-1",
    "api-ms-win-core-path-l1-1-0",
    "api-ms-win-core-psapi-l1-1-0",
    "api-ms-win-core-timezone-l1-1-0",
    "api-ms-win-core-comm-l1-1-1",
    "api-ms-win-core-sysinfo-l1-2-4",
    "api-ms-win-core-processthreads-l1-1-3",
};

static PISAPISETIMPLEMENTED
get_implemented(void)
{
    PISAPISETIMPLEMENTED pfn = NULL;
    HMODULE hMod = GetModuleHandleW(L"kernelbase.dll");

    if (hMod)
        pfn = (PISAPISETIMPLEMENTED)GetProcAddress(hMod, "IsApiSetImplemented");
    if (!pfn)
    {
        hMod = GetModuleHandleW(L"kernel32.dll");
        if (hMod)
            pfn = (PISAPISETIMPLEMENTED)GetProcAddress(hMod, "IsApiSetImplemented");
    }
    return pfn;
}

/* Bit i set = contract i implemented. */
static ULONG
query_bits(PISAPISETIMPLEMENTED pfn)
{
    ULONG i, bits = 0;

    for (i = 0; i < RTL_NUMBER_OF(g_Contracts); i++)
    {
        if (pfn(g_Contracts[i]))
            bits |= 1UL << i;
    }
    return bits;
}

/* Child entry (test name "win32versionvalue_child"): write "bits major minor" to the file in WVV_ENV. */
void func_win32versionvalue_child(void)
{
    WCHAR Path[MAX_PATH];
    RTL_OSVERSIONINFOW Ver = { sizeof(Ver) };
    PISAPISETIMPLEMENTED pfn = get_implemented();
    FILE *f;

    if (!GetEnvironmentVariableW(WVV_ENV, Path, RTL_NUMBER_OF(Path)))
    {
        skip("not started by the win32versionvalue test\n");
        return;
    }
    RtlGetVersion(&Ver);
    f = _wfopen(Path, L"w");
    if (!f)
    {
        ok(0, "cannot open %ls\n", Path);
        return;
    }
    fprintf(f, "%lu %lu %lu %lu\n", pfn ? query_bits(pfn) : 0xFFFFFFFFUL, Ver.dwMajorVersion, Ver.dwMinorVersion,
            pfn ? 1UL : 0UL);
    fclose(f);
}

static BOOL
make_patched_copy(PCWSTR Src, PCWSTR Dst)
{
    HANDLE hFile;
    DWORD Size, Read, Written;
    PBYTE Buf;
    ULONG PeOffset;
    ULONG Value = WVV_VALUE;
    BOOL Ok = FALSE;

    hFile = CreateFileW(Src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    Size = GetFileSize(hFile, NULL);
    Buf = HeapAlloc(GetProcessHeap(), 0, Size);
    if (Buf && ReadFile(hFile, Buf, Size, &Read, NULL) && Read == Size && Size > 0x100 &&
        Buf[0] == 'M' && Buf[1] == 'Z')
    {
        PeOffset = *(PULONG)(Buf + 0x3C);
        if (PeOffset + 24 + 0x38 <= Size && !memcmp(Buf + PeOffset, "PE\0\0", 4))
        {
            /* IMAGE_OPTIONAL_HEADER.Win32VersionValue is at +0x34 for PE32 and PE32+. */
            memcpy(Buf + PeOffset + 24 + 0x34, &Value, sizeof(Value));
            CloseHandle(hFile);
            hFile = CreateFileW(Dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE)
            {
                Ok = WriteFile(hFile, Buf, Size, &Written, NULL) && Written == Size;
                CloseHandle(hFile);
                hFile = INVALID_HANDLE_VALUE;
            }
        }
    }
    if (Buf)
        HeapFree(GetProcessHeap(), 0, Buf);
    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);
    return Ok;
}

void func_win32versionvalue(void)
{
    WCHAR Self[MAX_PATH], Dir[MAX_PATH], Copy[MAX_PATH], Out[MAX_PATH], Cmd[2 * MAX_PATH + 64];
    PISAPISETIMPLEMENTED pfn = get_implemented();
    STARTUPINFOW Si = { sizeof(Si) };
    PROCESS_INFORMATION Pi;
    ULONG ParentBits, ChildBits = 0, ChildMajor = 0, ChildMinor = 0, ChildHasApi = 0;
    DWORD Exit = 0, Wait;
    FILE *f;
    ULONG i;
    int Count;

    if (!pfn)
    {
        skip("IsApiSetImplemented is not exported\n");
        return;
    }
    ParentBits = query_bits(pfn);
    ok(ParentBits != 0, "no contract of the list is implemented in the parent, test is meaningless\n");

    GetModuleFileNameW(NULL, Self, RTL_NUMBER_OF(Self));
    GetTempPathW(RTL_NUMBER_OF(Dir), Dir);
    _snwprintf(Copy, RTL_NUMBER_OF(Copy), L"%lsapisets_wvv_%lu.exe", Dir, GetCurrentProcessId());
    _snwprintf(Out, RTL_NUMBER_OF(Out), L"%lsapisets_wvv_%lu.txt", Dir, GetCurrentProcessId());
    DeleteFileW(Out);

    if (!make_patched_copy(Self, Copy))
    {
        skip("cannot create the patched copy %ls (%lu)\n", Copy, GetLastError());
        return;
    }

    SetEnvironmentVariableW(WVV_ENV, Out);
    _snwprintf(Cmd, RTL_NUMBER_OF(Cmd), L"\"%ls\" win32versionvalue_child", Copy);
    if (!CreateProcessW(Copy, Cmd, NULL, NULL, FALSE, 0, NULL, NULL, &Si, &Pi))
    {
        skip("cannot start the patched copy (%lu)\n", GetLastError());
        SetEnvironmentVariableW(WVV_ENV, NULL);
        DeleteFileW(Copy);
        return;
    }
    Wait = WaitForSingleObject(Pi.hProcess, 60000);
    ok(Wait == WAIT_OBJECT_0, "child did not finish: %lu\n", Wait);
    GetExitCodeProcess(Pi.hProcess, &Exit);
    CloseHandle(Pi.hThread);
    CloseHandle(Pi.hProcess);
    SetEnvironmentVariableW(WVV_ENV, NULL);

    f = _wfopen(Out, L"r");
    if (!f)
    {
        ok(0, "the child produced no result file (exit %lu)\n", Exit);
        DeleteFileW(Copy);
        return;
    }
    Count = fscanf(f, "%lu %lu %lu %lu", &ChildBits, &ChildMajor, &ChildMinor, &ChildHasApi);
    fclose(f);
    DeleteFileW(Out);
    DeleteFileW(Copy);
    ok(Count == 4, "bad result file, %d fields\n", Count);

    /* The override is still honoured: the child sees 6.1 through RtlGetVersion (PEB). */
    ok(ChildMajor == 6 && ChildMinor == 1, "child reports %lu.%lu, expected the 6.1 override\n", ChildMajor, ChildMinor);
    ok(ChildHasApi == 1, "IsApiSetImplemented missing in the child\n");

    /* The API-set answers must not follow it. */
    for (i = 0; i < RTL_NUMBER_OF(g_Contracts); i++)
    {
        ok(((ParentBits >> i) & 1) == ((ChildBits >> i) & 1), "%s: parent %lu, child with Win32VersionValue 6.1: %lu\n",
           g_Contracts[i], (ParentBits >> i) & 1, (ChildBits >> i) & 1);
    }
}
