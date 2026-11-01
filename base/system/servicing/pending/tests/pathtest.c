/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Unprivileged test: servicing-root resolution from \SystemRoot
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

/*
 * Regression for Dwight's online run 4: the boot applier hardcoded
 * \SystemRoot\LiberNT\Servicing, but \SystemRoot names the Windows
 * DIRECTORY ("\??\C:\Windows" or its ARC form), so the plan was searched
 * under the Windows directory, never found, and the applier silently
 * exited 0 — the plan sat unconsumed across every boot. The resolution
 * logic is pure string handling; this test pins it for every link shape.
 *
 * Build (from this directory, MinGW/Clang on PATH):
 *   clang --target=x86_64-w64-mingw32 -O1 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE -I../../../system/servicing/pending -I<shim dir> pathtest.c ../bundle-not-needed... (see S1-E2E.md pattern)
 *   clang --target=x86_64-w64-mingw32 -O1 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE -I../../../system/servicing/pending -I"C:/Users/Nicolas/HarnessAgents/hive/shared/leeway/libernt-update/s1-e2e" pathtest.c "../../../system/servicing/pending/s1core.c" -mconsole -o pathtest.exe -lmsvcrt-os -lkernel32 -lntdll
 *   ./pathtest.exe
 */

#define S1_HOST_APPLIER 1
#include "s1host_shim.h"

WCHAR g_FakeRoot[MAX_PATH];
WCHAR g_ServicingRoot[MAX_PATH];
WCHAR g_S1PlanPath[MAX_PATH * 2];
WCHAR g_S1AttemptPath[MAX_PATH * 2];
WCHAR g_S1JournalPath[MAX_PATH * 2];
WCHAR g_S1BackupPath[MAX_PATH * 2];
WCHAR g_VolumeRoot[MAX_PATH * 2];

#include "../pending.c"

#include <stdio.h>

static int Failures;

static void
Case(const WCHAR *LinkTarget, const WCHAR *Want)
{
    WCHAR Got[MAX_PATH * 2];
    BOOL Ok;

    Ok = S1ServicingRootFromSystemRoot(LinkTarget, Got, RTL_NUMBER_OF(Got));
    if (Ok && wcscmp(Got, Want) == 0)
        printf("ok:   %ls -> %ls\n", LinkTarget, Got);
    else
    {
        printf("FAIL: %ls -> %ls (want %ls)\n", LinkTarget, Ok ? Got : L"<rejected>",
               Want);
        Failures++;
    }
}

static void
CaseRejected(const WCHAR *LinkTarget)
{
    WCHAR Got[MAX_PATH * 2];

    if (!S1ServicingRootFromSystemRoot(LinkTarget, Got, RTL_NUMBER_OF(Got)))
        printf("ok:   %ls -> rejected\n", LinkTarget);
    else
    {
        printf("FAIL: %ls -> %ls (want rejected)\n", LinkTarget, Got);
        Failures++;
    }
}

int
main(void)
{
    WCHAR WinDir[MAX_PATH], Synthesized[MAX_PATH * 2];

    Case(L"\\??\\C:\\Windows", L"\\??\\C:\\LiberNT\\Servicing");
    Case(L"\\??\\C:\\ReactOS", L"\\??\\C:\\LiberNT\\Servicing");
    Case(L"\\??\\C:\\", L"\\??\\C:\\LiberNT\\Servicing");
    Case(L"\\??\\C:\\LiberNT\\Windows", L"\\??\\C:\\LiberNT\\Servicing");
    Case(L"\\??\\C:\\a\\b\\Windows", L"\\??\\C:\\LiberNT\\Servicing");
    Case(L"\\??\\C:\\Win(1)", L"\\??\\C:\\LiberNT\\Servicing");
    Case(L"\\??\\C:\\HarddiskTools\\Windows", L"\\??\\C:\\LiberNT\\Servicing");
    Case(L"\\ArcName\\multi(0)disk(0)rdisk(0)partition(1)\\ReactOS",
         L"\\ArcName\\multi(0)disk(0)rdisk(0)partition(1)\\LiberNT\\Servicing");
    Case(L"\\ArcName\\multi(0)disk(0)rdisk(0)partition(1)\\Windows\\",
         L"\\ArcName\\multi(0)disk(0)rdisk(0)partition(1)\\LiberNT\\Servicing");
    Case(L"\\ArcName\\multi(0)disk(0)rdisk(0)partition(1)\\a\\b\\ReactOS",
         L"\\ArcName\\multi(0)disk(0)rdisk(0)partition(1)\\LiberNT\\Servicing");
    Case(L"\\Device\\HarddiskVolume1",
         L"\\Device\\HarddiskVolume1\\LiberNT\\Servicing");
    Case(L"\\Device\\HarddiskVolume1\\",
         L"\\Device\\HarddiskVolume1\\LiberNT\\Servicing");
    Case(L"\\Device\\HarddiskVolume1\\Windows",
         L"\\Device\\HarddiskVolume1\\LiberNT\\Servicing");
    Case(L"\\Device\\HarddiskVolume1\\nested\\deeper\\Windows",
         L"\\Device\\HarddiskVolume1\\LiberNT\\Servicing");
    Case(L"\\Device\\Harddisk0\\Partition1\\Windows",
         L"\\Device\\Harddisk0\\Partition1\\LiberNT\\Servicing");
    Case(L"\\Device\\CdRom0\\Windows",
         L"\\Device\\CdRom0\\LiberNT\\Servicing");
    CaseRejected(L"\\garbage\\path");
    CaseRejected(L"\\??\\NoColon");
    CaseRejected(L"\\Device\\RamDisk0\\Windows");
    CaseRejected(L"ArcName\\without\\leading\\backslash");

    /* the live host shape: synthesize the link target from GetWindowsDirectoryW */
    if (GetWindowsDirectoryW(WinDir, RTL_NUMBER_OF(WinDir)) && WinDir[1] == L':')
    {
        swprintf(Synthesized, RTL_NUMBER_OF(Synthesized), L"\\??\\%s", WinDir);
        swprintf(WinDir, RTL_NUMBER_OF(WinDir), L"\\??\\%c:\\LiberNT\\Servicing",
                 Synthesized[4]);
        Case(Synthesized, WinDir);
    }
    else
    {
        printf("skip: cannot synthesize the live host shape\n");
    }

    printf("%s\n", Failures ? "PATH RESOLUTION TEST FAILED" : "PATH RESOLUTION TEST PASSED");
    return Failures ? 1 : 0;
}
