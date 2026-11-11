/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NTFS filesystem driver
 * COPYRIGHT:   Copyright 2024 Carl Bialorucki <carl.bialorucki@reactos.org>
 *              Copyright 2024 Justin Miller <justin.miller@reactos.org>
 */

#include "ntfspch.h"

#define InvalidMftZoneReservation(Num) Num < 0 || Num > 4
#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))
#define KEYVAL_BUFFER_SIZE ROUND_UP(sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG), 0x10)

BOOLEAN gAllowExtChar8dot3;
BOOLEAN gShowVersionInfo;
BOOLEAN gBugCheckOnCorrupt;
BOOLEAN gDisable8dot3NameCreation;
INT gDisableLastAccessUpdate;
BOOLEAN gDisableLfsDowngrade;
BOOLEAN gDisableLfsUpgrade;
INT gMftZoneReservation;
BOOLEAN gTracePagingWrites;

HANDLE
OpenRegistryKey()
{
    NTSTATUS Status;
    HANDLE hRegistryKey;
    UNICODE_STRING RegistryPath;
    OBJECT_ATTRIBUTES Attributes;

    // Set up registry path
    RtlInitUnicodeString(&RegistryPath,
                         L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\FileSystem");

    InitializeObjectAttributes(&Attributes,
                               &RegistryPath,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    Status = ZwOpenKey(&hRegistryKey, KEY_READ, &Attributes);
    if (!NT_SUCCESS(Status))
    {
        return NULL;
    }

    // Caller must close this registry key.
    return hRegistryKey;
}

INT
QueryDwordRegistryValueEx(_In_ HANDLE RegistryKey,
                          _In_ PWCHAR Name,
                          _In_ INT Default)
{
    NTSTATUS Status;
    UNICODE_STRING ValueName;
    UCHAR Buffer[KEYVAL_BUFFER_SIZE];
    ULONG DataLength;

    if (!RegistryKey)
        return Default;

    // Set up registry value
    RtlInitUnicodeString(&ValueName, Name);

    Status = ZwQueryValueKey(RegistryKey,
                             &ValueName,
                             KeyValuePartialInformation,
                             Buffer,
                             KEYVAL_BUFFER_SIZE,
                             &DataLength);

    if (!NT_SUCCESS(Status))
    {
        return Default;
    }

    return *((INT*)(((PKEY_VALUE_PARTIAL_INFORMATION)Buffer)->Data));
}

INT
QueryDwordRegistryValue(_In_ PWCHAR Name,
                        _In_ INT Default)
{
    HANDLE RegistryKey;

    RegistryKey = OpenRegistryKey();
    if (!RegistryKey)
        return Default;
    return QueryDwordRegistryValueEx(RegistryKey, Name, Default);
}

BOOLEAN
QueryBooleanRegistryValueEx(_In_ HANDLE RegistryKey,
                            _In_ PWCHAR Name,
                            _In_ BOOLEAN Default)
{
    return !!QueryDwordRegistryValueEx(RegistryKey, Name, Default ? 1 : 0);
}

BOOLEAN
QueryBooleanRegistryValue(_In_ PWCHAR Name,
                          _In_ BOOLEAN Default)
{
    return !!QueryDwordRegistryValue(Name, Default ? 1 : 0);
}

NTSTATUS
SetDwordRegistryValue(_In_ HANDLE RegistryKey,
                      _In_ PWCHAR Name,
                      _In_ INT Data)
{
    UNICODE_STRING ValueName;

    if (!RegistryKey)
        return STATUS_INVALID_PARAMETER;

    // Set up registry value
    RtlInitUnicodeString(&ValueName, Name);

    return ZwSetValueKey(RegistryKey,
                         &ValueName,
                         0,
                         REG_DWORD,
                         &Data,
                         sizeof(INT));
}

NTSTATUS
SetBooleanRegistryValue(_In_ HANDLE RegistryKey,
                        _In_ PWCHAR Name,
                        _In_ BOOLEAN Data)
{
    return SetDwordRegistryValue(RegistryKey, Name, Data ? 1 : 0);
}

VOID
GetGlobalSettingsFromRegistry()
{
    HANDLE RegistryKey;

    RegistryKey = OpenRegistryKey();

    /* *** Inherited Options *** */

    /* Allows extended characters, including diacritics, for 8.3 compliant
     * file names. Default is OFF (0).
     */
    gAllowExtChar8dot3 = QueryBooleanRegistryValueEx(RegistryKey,
                                                     L"NtfsAllowExtendedCharacter8dot3Rename",
                                                     FALSE);

    /* Initiates a Bug Check when file corruption is detected, instead of
     * trying to repair it. Default is OFF (0).
     */
    gBugCheckOnCorrupt = QueryBooleanRegistryValueEx(RegistryKey,
                                                     L"NtfsBugCheckOnCorrupt",
                                                     FALSE);

    /* Disables generating an 8.3 compliant name when a file has a
     * non-compliant name. This feature has a significant performance impact,
     * but allows older Windows programs to access these files.
     * Default is OFF (0).
     */
    gDisable8dot3NameCreation = QueryBooleanRegistryValueEx(RegistryKey,
                                                            L"NtfsDisable8dot3NameCreation",
                                                            FALSE);
    NtfsSetGenerate8dot3Names(!gDisable8dot3NameCreation);

    /* Enables or disables the automatic last-access time update on all files
     * and all volumes, with the Windows 10 1803+ meaning. The low two bits are
     * the value fsutil behavior shows; Windows itself stores the System
     * Managed forms with bit 31 set (0x80000002 / 0x80000003), and both
     * spellings are accepted here.
     *     0 - User Managed, Enabled
     *     1 - User Managed, Disabled
     *     2 - System Managed, Enabled (default)
     *     3 - System Managed, Disabled
     * System Managed is decided per volume: enabled only below 128 GB
     * (NtfsLastAccessUpdateEnabled). The policy is consulted where a read
     * would stamp the time; an explicit SetFileInformation still writes it.
     */
    {
        ULONG RawValue = (ULONG)QueryDwordRegistryValueEx(RegistryKey,
                                                          L"NtfsDisableLastAccessUpdate",
                                                          NTFS_LAST_ACCESS_SYSTEM_ENABLED);

        if ((RawValue & ~(NTFS_LAST_ACCESS_SYSTEM_MANAGED_BIT | 3UL)) == 0 &&
            (!(RawValue & NTFS_LAST_ACCESS_SYSTEM_MANAGED_BIT) || (RawValue & 2)))
        {
            gDisableLastAccessUpdate = (INT)(RawValue & 3);
        }
        else
        {
            // Out of range: use the default, and try to repair the value.
            SetDwordRegistryValue(RegistryKey,
                                  L"NtfsDisableLastAccessUpdate",
                                  NTFS_LAST_ACCESS_SYSTEM_ENABLED);
            gDisableLastAccessUpdate = NTFS_LAST_ACCESS_SYSTEM_ENABLED;
        }
    }

    /* Disables LFS downgrade when a volume is cleanly unmounted. The LFS
     * downgrade is required for interoperability with older versions of
     * Windows. Default is OFF (0).
     *
     * Note: This option currently has no effect.
     */
    gDisableLfsDowngrade = QueryBooleanRegistryValueEx(RegistryKey,
                                                       L"NtfsDisableLfsDowngrade",
                                                       FALSE);

    /* Disables LFS upgrade when a volume is mounted. The LFS upgrade can cause
     * compatibility issues with older versions of Windows when the volume is
     * not cleanly shut down or dismounted. Default is OFF (0).
     *
     * Note: This option currently has no effect.
     */
    gDisableLfsUpgrade = QueryBooleanRegistryValueEx(RegistryKey,
                                                     L"NtfsDisableLfsUpgrade",
                                                     FALSE);

    /* Changes the amount of space reserved for the Master File Table (MFT)
     * by default.
     *
     * Options:
     *     For Windows 7 and earlier:
     *
     *     0 - 12.5% of the disk (same as 1)
     *     1 - 12.5% of the disk (default)
     *     2 - 25.0% of the disk
     *     3 - 37.5% of the disk
     *     4 - 50.0% of the disk
     *
     *     For Windows 8+:
     *     MFT space reserved = 200 MB * max(gMftZoneReservation, 1)
     *
     * > I think we should follow the Windows 7 and earlier behavior because it
     * > seems to work better on spinning disk drives and is better documented.
     * > - Carl Bialorucki
     */
    gMftZoneReservation = QueryDwordRegistryValueEx(RegistryKey,
                                                    L"NtfsMftZoneReservation",
                                                    1);

    if (InvalidMftZoneReservation(gMftZoneReservation))
    {
        // We don't care if this fails or not, just give it a try.
        SetDwordRegistryValue(RegistryKey, L"NtfsMftZoneReservation", 2);
        gMftZoneReservation = 1;
    }

    /* *** ReactOS Extended Options *** */

    /* Shows the super hidden NTFS metadata files, including $MFT and $LogFile.
     * Default is OFF (0).
     */
    NtfsSetShowMetadataFiles(QueryBooleanRegistryValueEx(RegistryKey,
                                                         L"NtfsShowMetadataFiles",
                                                         FALSE));

    /* Shows the NTFS version information, including the LFS Client version,
     * in the properties window for each NTFS volume. Default is OFF (0).
     */
    gShowVersionInfo = QueryBooleanRegistryValueEx(RegistryKey,
                                                   L"NtfsShowVersionInfo",
                                                   FALSE);

    /* Prints one debug line per paging write (checked builds only), for the
     * power-cut probes that read a file's write-back timeline off the serial
     * port. The line costs more than the write at 115200 baud and throttles
     * every paging write, so it is off unless a lab turns it on.
     * Default is OFF (0). Read once; takes effect at the next boot.
     */
    gTracePagingWrites = QueryBooleanRegistryValueEx(RegistryKey,
                                                     L"NtfsTracePagingWrites",
                                                     FALSE);

    CloseRegistryKey(RegistryKey);
}
