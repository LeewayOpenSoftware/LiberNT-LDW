/*
 * PROJECT:     LiberNT Update - boot-class lab fixtures
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     A native process that terminates itself with a failure code, to exercise the smss fallback
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * TEST FIXTURE, not a shipping binary. Native subsystem (it runs before csrss, like the real session manager),
 * built and signed with the DEVELOPMENT key. It stands in for smss.exe in a boot-class package: its entry point
 * terminates the process with a failure status at once, so the new set has no working session manager and the
 * boot does not reach the desktop. That is the condition the fallback boot (kernel loads smss.old) and the
 * rollback recover. No corruption, no fault: a signed component that cleanly declines to run.
 */
#include <ntndk.h>

VOID
NTAPI
NtProcessStartup(_In_ PVOID Peb)
{
    UNREFERENCED_PARAMETER(Peb);

    /* no working session manager -> the boot cannot complete; the fallback boot starts smss.old */
    NtTerminateProcess(NtCurrentProcess(), STATUS_UNSUCCESSFUL);
}
