/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NTFS filesystem driver
 * COPYRIGHT:   Copyright 2024 Carl Bialorucki <carl.bialorucki@reactos.org>
 *              Copyright 2024 Justin Miller <justin.miller@reactos.org>
 */

#include "ntfslib_new.h"
#include "ntfslib_new_internal.h"

/*
 * Reads the $UpCase table into the Volume cache. The table is immutable, so
 * it is read once, normally at mount. It is read into a private buffer and
 * published only once complete (NtfsPublishOnce): before, the pointer was
 * set by the read itself, so a second reader could see the table with a
 * length of 0 and compare names case-sensitively, and a failed read freed a
 * table another reader already held.
 */
NTSTATUS
Volume::LoadUpcaseTable()
{
    NTSTATUS Status;
    PFileRecord UpCaseFile;
    PAttribute UpCaseData;
    PWSTR Table = NULL;
    ULONG UpCaseDataLength = 0;

    // Get $UpCase file
    Status = MFT->GetFileAttributeFromFileRecordNumber(TypeData,
                                                       NULL,
                                                       _UpCase,
                                                       &UpCaseFile,
                                                       &UpCaseData);
    if (!NT_SUCCESS(Status))
        return Status;

    // Get $DATA attribute contents from $UpCase
    Status = UpCaseFile->ReadAttributeAlloc(UpCaseData,
                                            (PUCHAR*)&Table,
                                            &UpCaseDataLength);
    delete UpCaseFile;
    if (NT_SUCCESS(Status) &&
        (UpCaseDataLength == 0 ||
         (UpCaseDataLength % sizeof(WCHAR)) != 0))
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
    }
    if (!NT_SUCCESS(Status))
    {
        delete[] Table;
        return Status;
    }

    /* Every loader stores the same length; the compare-exchange orders it
     * before the pointer. */
    UpcaseTableLength = UpCaseDataLength / sizeof(WCHAR);
    if (NtfsPublishOnce(reinterpret_cast<PVOID volatile*>(&UpcaseTable), Table) != Table)
        delete[] Table;
    return STATUS_SUCCESS;
}

NTSTATUS
Volume::GetUpcase(_Out_ PWSTR* Table,
                  _Out_ PULONG Length)
{
    PWSTR Published;
    NTSTATUS Status;

    Published = static_cast<PWSTR>(
        NtfsLoadPublished(reinterpret_cast<PVOID const volatile*>(&UpcaseTable)));
    if (!Published)
    {
        Status = LoadUpcaseTable();
        if (!NT_SUCCESS(Status))
            return Status;
        Published = static_cast<PWSTR>(
            NtfsLoadPublished(reinterpret_cast<PVOID const volatile*>(&UpcaseTable)));
    }
    *Table = Published;
    *Length = UpcaseTableLength;
    return STATUS_SUCCESS;
}

NTSTATUS
Volume::UpcaseWideString(_Inout_ PWSTR WideString,
                         _In_    ULONG Length)
{
    PWSTR Table;
    ULONG TableLength;
    NTSTATUS Status;

    Status = GetUpcase(&Table, &TableLength);
    if (!NT_SUCCESS(Status))
        return Status;

    // Update each character according to its entry in the $UpCase table
    for (ULONG i = 0; i < Length; i++)
    {
        if (WideString[i] < TableLength)
            WideString[i] = Table[WideString[i]];
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Volume::CompareFileNames(_In_  PUNICODE_STRING Left,
                         _In_  PUNICODE_STRING Right,
                         _Out_ LONG* Result)
{
    NTSTATUS Status;
    PWSTR Table;
    ULONG TableLength;
    ULONG LeftLength;
    ULONG RightLength;
    ULONG CommonLength;

    if (!Left || !Right || !Result ||
        (Left->Length != 0 && !Left->Buffer) ||
        (Right->Length != 0 && !Right->Buffer) ||
        (Left->Length & (sizeof(WCHAR) - 1)) != 0 ||
        (Right->Length & (sizeof(WCHAR) - 1)) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Status = GetUpcase(&Table, &TableLength);
    if (!NT_SUCCESS(Status))
        return Status;

    LeftLength = Left->Length / sizeof(WCHAR);
    RightLength = Right->Length / sizeof(WCHAR);
    CommonLength = min(LeftLength, RightLength);

    for (ULONG Index = 0; Index < CommonLength; Index++)
    {
        WCHAR LeftCharacter = Left->Buffer[Index];
        WCHAR RightCharacter = Right->Buffer[Index];

        if (LeftCharacter < TableLength)
            LeftCharacter = Table[LeftCharacter];
        if (RightCharacter < TableLength)
            RightCharacter = Table[RightCharacter];

        if (LeftCharacter != RightCharacter)
        {
            *Result = LeftCharacter < RightCharacter ? -1 : 1;
            return STATUS_SUCCESS;
        }
    }

    *Result = LeftLength == RightLength
        ? 0
        : (LeftLength < RightLength ? -1 : 1);
    return STATUS_SUCCESS;
}

NTSTATUS
Volume::CollateFileNames(_In_  PUNICODE_STRING Left,
                         _In_  PUNICODE_STRING Right,
                         _In_  BOOLEAN CaseSensitive,
                         _Out_ LONG* Result)
{
    NTSTATUS Status;

    Status = CompareFileNames(Left, Right, Result);
    if (!NT_SUCCESS(Status) || *Result != 0 || !CaseSensitive)
        return Status;

    for (ULONG Index = 0; Index < Left->Length / sizeof(WCHAR); Index++)
    {
        if (Left->Buffer[Index] != Right->Buffer[Index])
        {
            *Result = Left->Buffer[Index] < Right->Buffer[Index] ? -1 : 1;
            break;
        }
    }
    return STATUS_SUCCESS;
}
