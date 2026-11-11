/*
 * PROJECT:     LiberNT Update - boot-class lab fixtures
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     A driver that fails its own init, to exercise the boot-class fallback (LAB-S1-BOOTCLASS-JOB.md)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * This is a TEST FIXTURE, not a shipping driver. Built and signed with the DEVELOPMENT key, it stands in for a
 * boot-start / system-start driver (D2/D3) or win32k (group 3) in a boot-class package. It does nothing but
 * return a failure status from DriverEntry, which is the ordinary, documented way a driver reports it cannot
 * start. On the first boot of the new set the loader brings it in, DriverEntry fails, and a boot-start driver
 * whose ErrorControl is critical stops the boot before smss; the fallback boot then loads the <stem>.old
 * pre-image and the applier rolls the swap back. No corruption and no crash: a clean, signed "does not work".
 */
#include <ntddk.h>

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    /* COM1 (kernel debugger output) so the lab can see which file came up as the new set */
    DbgPrint("LNFAILSTUB: boot-class driver fixture returning failure to exercise the fallback\n");
    return STATUS_DRIVER_UNABLE_TO_LOAD;
}
