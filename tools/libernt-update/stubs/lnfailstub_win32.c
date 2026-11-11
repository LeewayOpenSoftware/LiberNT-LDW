/*
 * PROJECT:     LiberNT Update - boot-class lab fixtures
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     A Win32 process that logs and exits non-zero, to exercise the group-3 fallback
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * TEST FIXTURE, not a shipping binary. Built and signed with the DEVELOPMENT key, it stands in for a Win32
 * critical process (csrss, wininit, winlogon, services, lsass) in a boot-class package. It writes one marker
 * line to COM1 and returns a non-zero exit code at once. A critical process that exits that way makes the new
 * set fail to come up (no working subsystem / no logon), which is exactly the condition the fallback boot and
 * the two-attempt rollback are there to recover. Nothing is corrupted; the file is simply a signed component
 * that does not do the job, so only the .old pre-image brings the box back.
 */
#include <windows.h>

int
main(void)
{
    static const char Marker[] =
        "LNFAILSTUB: boot-class process fixture exiting non-zero to exercise the fallback\r\n";
    HANDLE Com = CreateFileW(L"\\\\.\\COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

    if (Com != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;

        WriteFile(Com, Marker, (DWORD)(sizeof(Marker) - 1), &written, NULL);
        CloseHandle(Com);
    }
    return 1;
}
