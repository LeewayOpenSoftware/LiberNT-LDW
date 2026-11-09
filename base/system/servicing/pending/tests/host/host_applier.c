/*
 * PROJECT:     LiberNT Update S1 end-to-end harness
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Run the real first-boot applier (pending.c) on the host over a fake target root
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

WCHAR g_FakeRoot[MAX_PATH];
WCHAR g_ServicingRoot[MAX_PATH];
WCHAR g_VolumeRoot[MAX_PATH * 2];
WCHAR g_S1PlanPath[MAX_PATH * 2];
WCHAR g_S1AttemptPath[MAX_PATH * 2];
WCHAR g_S1JournalPath[MAX_PATH * 2];
WCHAR g_S1BackupPath[MAX_PATH * 2];

void ShimSetFakeRoot(const wchar_t *Root)
{
    wchar_t Full[MAX_PATH * 2];

    if (!GetFullPathNameW(Root, MAX_PATH * 2, Full, NULL))
        wcsncpy(Full, Root, MAX_PATH * 2);
    Full[MAX_PATH * 2 - 1] = 0;

    wcsncpy(g_FakeRoot, Full, MAX_PATH);
    g_FakeRoot[MAX_PATH - 1] = 0;

    swprintf(g_ServicingRoot, MAX_PATH * 2, L"%s\\LiberNT\\Servicing", g_FakeRoot);
    swprintf(g_S1PlanPath, MAX_PATH * 2, L"%s\\pending\\plan.json", g_ServicingRoot);
    swprintf(g_S1AttemptPath, MAX_PATH * 2, L"%s\\pending\\attempt.json", g_ServicingRoot);
    swprintf(g_S1JournalPath, MAX_PATH * 2, L"%s\\journal\\transactions.jsonl", g_ServicingRoot);
    swprintf(g_S1BackupPath, MAX_PATH * 2, L"%s\\backup", g_ServicingRoot);
    wcsncpy(g_VolumeRoot, g_FakeRoot, MAX_PATH * 2 - 1);
    g_VolumeRoot[MAX_PATH * 2 - 1] = 0;
}

#define S1_HOST_APPLIER 1
#include "s1host_shim.h"

/* Boot-class dirty check on the fake volume (B1 review M4): the applier must ask BEFORE it modifies anything.
 * S1_FAKE_DIRTY=1 makes the fake volume dirty so the swap is refused. */
static NTSTATUS ShimQueryVolumeDirty(VOID)
{
    wprintf(L"[host_applier] dirty query after %lu mutating operations\n", (unsigned long)g_ShimMutations);
    if (g_ShimMutations != 0)
    {
        wprintf(L"[host_applier] FAIL: the applier wrote to the volume before the dirty check\n");
        fflush(stdout);
        ExitProcess(3);                     /* any harness (run-e2e, test-swap-host) sees a non-zero exit */
    }
    return getenv("S1_FAKE_DIRTY") ? (NTSTATUS)0xE0000002L : STATUS_SUCCESS;
}

#include "pending.c"

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        fwprintf(stderr, L"usage: host_applier.exe <fake-root>\n");
        return 1;
    }
    ShimSetFakeRoot(argv[1]);
    wprintf(L"[host_applier] fake root: %s\n", g_FakeRoot);
    NtProcessStartup(NULL);
    return 0;
}
