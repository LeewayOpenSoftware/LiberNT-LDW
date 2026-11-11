/*
 * PROJECT:     LiberNT Update - boot-class lab fixtures
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     A driver that loads cleanly, to exercise the good driver-swap lane (D1)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * TEST FIXTURE, not a shipping binary. The "good" counterpart of lnfailstub_driver: its DriverEntry
 * returns STATUS_SUCCESS so the driver loads and the confirm step finds it with the new identity. Stands
 * in for a working new boot-start / system-start driver in a boot-class package (lane D1).
 */
#include <ntddk.h>

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    /* loads cleanly: the good driver-swap confirms with the new identity */
    return STATUS_SUCCESS;
}
