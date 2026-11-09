/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtQuerySystemInformation
 * COPYRIGHT:   Copyright 2019 Thomas Faber (thomas.faber@reactos.org)
 */

#include "precomp.h"

/* PE CheckSum and TimeDateStamp of \SystemRoot\System32\<Name>, from the file's headers */
static
BOOLEAN
ReadFilePeIdentity(
    _In_ PCSTR Name,
    _Out_ PULONG CheckSum,
    _Out_ PULONG TimeDateStamp)
{
    WCHAR Path[MAX_PATH];
    UNICODE_STRING PathU;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;
    HANDLE File;
    UCHAR Header[4096];
    LARGE_INTEGER Offset;
    PIMAGE_DOS_HEADER Dos = (PIMAGE_DOS_HEADER)Header;
    PIMAGE_NT_HEADERS Nt;
    NTSTATUS Status;

    if (_snwprintf(Path, RTL_NUMBER_OF(Path), L"\\SystemRoot\\System32\\%hs", Name) < 0)
        return FALSE;
    Path[RTL_NUMBER_OF(Path) - 1] = UNICODE_NULL;
    RtlInitUnicodeString(&PathU, Path);
    InitializeObjectAttributes(&Attributes, &PathU, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&File, FILE_READ_DATA | SYNCHRONIZE, &Attributes, &IoStatus,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status))
        return FALSE;
    Offset.QuadPart = 0;
    Status = NtReadFile(File, NULL, NULL, NULL, &IoStatus, Header, sizeof(Header), &Offset, NULL);
    NtClose(File);
    if (!NT_SUCCESS(Status) || IoStatus.Information < sizeof(IMAGE_DOS_HEADER) ||
        Dos->e_magic != IMAGE_DOS_SIGNATURE || Dos->e_lfanew <= 0 ||
        (ULONG)Dos->e_lfanew + sizeof(IMAGE_NT_HEADERS) > IoStatus.Information)
    {
        return FALSE;
    }
    Nt = (PIMAGE_NT_HEADERS)(Header + Dos->e_lfanew);
    if (Nt->Signature != IMAGE_NT_SIGNATURE)
        return FALSE;
    *CheckSum = Nt->OptionalHeader.CheckSum;
    *TimeDateStamp = Nt->FileHeader.TimeDateStamp;
    return TRUE;
}

/* Class 77: the same modules, in the same order, as class 11, plus checksum,
 * time stamp and preferred base, chained by NextOffset. */
static
VOID
TestModuleInformationEx(VOID)
{
    NTSTATUS Status;
    ULONG Length = 0, ExLength = 0, Returned = 0, Count, i;
    PRTL_PROCESS_MODULES Modules;
    PUCHAR ExBuffer;
    PRTL_PROCESS_MODULE_INFORMATION_EX Entry;
    /* ULONG-aligned (the buffer is probed for ULONG alignment), smaller than one entry */
    ULONG Small[(sizeof(RTL_PROCESS_MODULE_INFORMATION_EX) - 1) / sizeof(ULONG)];

    Status = NtQuerySystemInformation(SystemModuleInformationEx, NULL, 0, &ExLength);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok(ExLength >= sizeof(RTL_PROCESS_MODULE_INFORMATION_EX), "Extended module length %lu\n", ExLength);
    ok(ExLength % sizeof(RTL_PROCESS_MODULE_INFORMATION_EX) == 0,
       "Extended module length %lu is not a whole number of entries\n", ExLength);

    Status = NtQuerySystemInformation(SystemModuleInformationEx, Small, sizeof(Small), &Returned);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok(Returned == ExLength, "Short buffer: required %lu, expected %lu\n", Returned, ExLength);
    if (ExLength < sizeof(RTL_PROCESS_MODULE_INFORMATION_EX))
        return;

    Status = NtQuerySystemInformation(SystemModuleInformation, NULL, 0, &Length);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    Modules = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length + 4096);
    ExBuffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, ExLength + 4096);
    if (!Modules || !ExBuffer)
    {
        skip("Could not allocate module information\n");
        goto Cleanup;
    }

    /* One entry less than needed: refused, and the required size is reported */
    Returned = 0;
    Status = NtQuerySystemInformation(SystemModuleInformationEx, ExBuffer,
                                      ExLength - sizeof(RTL_PROCESS_MODULE_INFORMATION_EX), &Returned);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok(Returned == ExLength, "One entry short: required %lu, expected %lu\n", Returned, ExLength);

    Status = NtQuerySystemInformation(SystemModuleInformation, Modules, Length + 4096, &Length);
    ok_hex(Status, STATUS_SUCCESS);
    Returned = 0;
    Status = NtQuerySystemInformation(SystemModuleInformationEx, ExBuffer, ExLength + 4096, &Returned);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    ok(Returned % sizeof(RTL_PROCESS_MODULE_INFORMATION_EX) == 0, "Returned length %lu\n", Returned);

    /* Walk the NextOffset chain */
    Count = 0;
    Entry = (PRTL_PROCESS_MODULE_INFORMATION_EX)ExBuffer;
    for (;;)
    {
        Count++;
        if (Entry->NextOffset == 0)
            break;
        ok(Entry->NextOffset == sizeof(RTL_PROCESS_MODULE_INFORMATION_EX),
           "Entry %lu: NextOffset %lu\n", Count - 1, Entry->NextOffset);
        if ((PUCHAR)Entry + Entry->NextOffset + sizeof(*Entry) > ExBuffer + Returned)
        {
            ok(FALSE, "Entry %lu: NextOffset runs past the returned data\n", Count - 1);
            break;
        }
        Entry = (PRTL_PROCESS_MODULE_INFORMATION_EX)((PUCHAR)Entry + Entry->NextOffset);
    }
    ok(Count == Returned / sizeof(RTL_PROCESS_MODULE_INFORMATION_EX),
       "Chain has %lu entries, length says %lu\n", Count, Returned / (ULONG)sizeof(RTL_PROCESS_MODULE_INFORMATION_EX));
    ok(Count == Modules->NumberOfModules, "Class 77 has %lu modules, class 11 has %lu\n",
       Count, Modules->NumberOfModules);

    /* Same order, base, size and name as class 11 */
    Entry = (PRTL_PROCESS_MODULE_INFORMATION_EX)ExBuffer;
    for (i = 0; i < Count && i < Modules->NumberOfModules; i++)
    {
        PRTL_PROCESS_MODULE_INFORMATION Base = &Modules->Modules[i];

        ok(Entry->BaseInfo.LoadOrderIndex == i, "Entry %lu: LoadOrderIndex %u\n", i, Entry->BaseInfo.LoadOrderIndex);
        ok(Entry->BaseInfo.ImageBase == Base->ImageBase, "Entry %lu: ImageBase %p, class 11 %p\n",
           i, Entry->BaseInfo.ImageBase, Base->ImageBase);
        ok(Entry->BaseInfo.ImageSize == Base->ImageSize, "Entry %lu: ImageSize 0x%lx, class 11 0x%lx\n",
           i, Entry->BaseInfo.ImageSize, Base->ImageSize);
        ok(strcmp((PCSTR)Entry->BaseInfo.FullPathName, (PCSTR)Base->FullPathName) == 0,
           "Entry %lu: '%s', class 11 '%s'\n", i, Entry->BaseInfo.FullPathName, Base->FullPathName);
        ok(Entry->BaseInfo.OffsetToFileName == Base->OffsetToFileName, "Entry %lu: OffsetToFileName %u, class 11 %u\n",
           i, Entry->BaseInfo.OffsetToFileName, Base->OffsetToFileName);
        if (Entry->NextOffset == 0)
            break;
        Entry = (PRTL_PROCESS_MODULE_INFORMATION_EX)((PUCHAR)Entry + Entry->NextOffset);
    }

    /* The kernel (first entry): time stamp and checksum are those of its image file */
    Entry = (PRTL_PROCESS_MODULE_INFORMATION_EX)ExBuffer;
    ok(Entry->TimeDateStamp != 0, "Kernel TimeDateStamp is 0\n");
    ok(Entry->DefaultBase != NULL, "Kernel DefaultBase is NULL\n");
    {
        ULONG FileCheckSum = 0, FileTimeDateStamp = 0;
        PCSTR Name = (PCSTR)Entry->BaseInfo.FullPathName + Entry->BaseInfo.OffsetToFileName;

        if (ReadFilePeIdentity(Name, &FileCheckSum, &FileTimeDateStamp))
        {
            ok(Entry->TimeDateStamp == FileTimeDateStamp, "Kernel TimeDateStamp 0x%lx, file '%s' 0x%lx\n",
               Entry->TimeDateStamp, Name, FileTimeDateStamp);
            ok(Entry->ImageCheckSum == FileCheckSum, "Kernel CheckSum 0x%lx, file '%s' 0x%lx\n",
               Entry->ImageCheckSum, Name, FileCheckSum);
            if (FileCheckSum == 0)
                trace("The kernel image '%s' has no PE checksum; only the match was checked\n", Name);
        }
        else
        {
            skip("Cannot read the PE headers of '%s'\n", Name);
        }
    }

Cleanup:
    if (ExBuffer)
        RtlFreeHeap(RtlGetProcessHeap(), 0, ExBuffer);
    if (Modules)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Modules);
}

START_TEST(NtQuerySystemInformation)
{
    NTSTATUS Status;
    ULONG Length = 0;
    PRTL_PROCESS_MODULES Modules;
    SYSTEM_BASIC_INFORMATION Basic;

    Status = NtQuerySystemInformation(0, NULL, 0, NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    Status = NtQuerySystemInformation(0x80000000, NULL, 0, NULL);
    ok_hex(Status, STATUS_INVALID_INFO_CLASS);

    RtlZeroMemory(&Basic, sizeof(Basic));
    Status = NtQuerySystemInformation(SystemBasicInformation, &Basic, sizeof(Basic), &Length);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(Basic.NumberOfPhysicalPages != 0, "No physical memory reported\n");
        ok(SharedUserData->NumberOfPhysicalPages == Basic.NumberOfPhysicalPages,
           "Shared physical pages %lu, system information %lu\n",
           SharedUserData->NumberOfPhysicalPages, Basic.NumberOfPhysicalPages);
        trace("Physical pages: shared %lu, queried %lu\n",
              SharedUserData->NumberOfPhysicalPages, Basic.NumberOfPhysicalPages);
    }

    /* Both loaded-image lists must be initialized even when no user images
     * have been registered with the kernel's module list. */
    Status = NtQuerySystemInformation(SystemModuleInformation, NULL, 0, &Length);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok(Length >= sizeof(*Modules), "Module information length %lu\n", Length);
    Modules = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length + 4096);
    if (!Modules)
    {
        skip("Could not allocate module information\n");
        return;
    }
    Status = NtQuerySystemInformation(SystemModuleInformation, Modules, Length + 4096, &Length);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ok(Modules->NumberOfModules != 0, "No kernel modules reported\n");
    RtlFreeHeap(RtlGetProcessHeap(), 0, Modules);

    TestModuleInformationEx();
}
