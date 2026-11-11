/*
 * PROJECT:     ReactOS Windows-Compatible Session Manager
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     LiberNT Update fallback boot: start a swapped critical process from its .old pre-image
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * A boot-class update swaps csrss, wininit or winlogon on disk and keeps the previous file beside it as
 * <stem>.old. On a fallback boot (the load options name a *.old kernel, HAL or bootvid, as the manual
 * LiberNT_Fallback entry and the boot-try loader do) the files listed in the updater's
 * LiberNT\Servicing\pending\bootstate ("ARMED", then one "old <path>" line each) are started from that
 * pre-image, so a process that killed the previous boot does not run again before the boot applier rolls
 * the swap back. The kernel does the same for ntdll, smss and win32k. S1-GROUP3-DESIGN.md.
 */

#include "smss.h"

#define NDEBUG
#include <debug.h>

#define SMP_S1_MAX_OLD  16
#define SMP_S1_NAME_CCH 64
#define SMP_S1_STATE_MAX 4096

static BOOLEAN SmpS1Checked;
static CHAR SmpS1Old[SMP_S1_MAX_OLD][SMP_S1_NAME_CCH];
static ULONG SmpS1OldCount;

/* TRUE when /KEY=value in the option string has a value ending in ".old" */
static BOOLEAN
SmpS1OptionNamesOld(IN PCWSTR Options, IN PCWSTR Key)
{
    SIZE_T KeyLength = wcslen(Key);
    PCWSTR p = Options;

    while (*p)
    {
        if ((p == Options || p[-1] == L' ' || p[-1] == L'\t') && _wcsnicmp(p, Key, KeyLength) == 0)
        {
            PCWSTR Value = p + KeyLength;
            SIZE_T Length = 0;

            while (Value[Length] && Value[Length] != L' ' && Value[Length] != L'\t')
                Length++;
            if (Length > 4 && _wcsnicmp(Value + Length - 4, L".old", 4) == 0)
                return TRUE;
        }
        p++;
    }
    return FALSE;
}

/* SystemStartOptions: the option string the loader handed the kernel */
static BOOLEAN
SmpS1IsFallbackBoot(VOID)
{
    static const PCWSTR Keys[] =
    {
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Control",
        L"\\Registry\\Machine\\System\\ControlSet001\\Control"
    };
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(Keys); i++)
    {
        UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 1024];
        PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
        OBJECT_ATTRIBUTES Attr;
        UNICODE_STRING Name, Value;
        HANDLE Key;
        ULONG Length;
        NTSTATUS Status;
        PWCHAR Options;

        RtlInitUnicodeString(&Name, Keys[i]);
        InitializeObjectAttributes(&Attr, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
        if (!NT_SUCCESS(NtOpenKey(&Key, KEY_QUERY_VALUE, &Attr)))
            continue;
        RtlInitUnicodeString(&Value, L"SystemStartOptions");
        Status = NtQueryValueKey(Key, &Value, KeyValuePartialInformation, Buffer, sizeof(Buffer) - sizeof(WCHAR), &Length);
        NtClose(Key);
        if (!NT_SUCCESS(Status) || Info->Type != REG_SZ)
            continue;
        Options = (PWCHAR)Info->Data;
        Options[Info->DataLength / sizeof(WCHAR)] = UNICODE_NULL;
        return SmpS1OptionNamesOld(Options, L"/KERNEL=") || SmpS1OptionNamesOld(Options, L"/HAL=") ||
               SmpS1OptionNamesOld(Options, L"/BOOTVID=");
    }
    return FALSE;
}

/* \??\<system drive>\LiberNT\Servicing\pending\bootstate: "ARMED" then "old <target>" lines */
static VOID
SmpS1ReadList(VOID)
{
    WCHAR PathBuffer[80];
    UNICODE_STRING Path;
    OBJECT_ATTRIBUTES Attr;
    IO_STATUS_BLOCK IoStatus;
    HANDLE File;
    PCHAR Buffer, Line, Next;
    SIZE_T Count;
    NTSTATUS Status;

    SmpS1OldCount = 0;
    if (!SharedUserData->NtSystemRoot[0] || SharedUserData->NtSystemRoot[1] != L':')
        return;
    _swprintf(PathBuffer, L"\\??\\%c:\\LiberNT\\Servicing\\pending\\bootstate", SharedUserData->NtSystemRoot[0]);
    RtlInitUnicodeString(&Path, PathBuffer);
    InitializeObjectAttributes(&Attr, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&File, FILE_GENERIC_READ, &Attr, &IoStatus, FILE_SHARE_READ,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status))
        return;                                 /* no state file: not armed */
    Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, SMP_S1_STATE_MAX + 1);
    if (!Buffer)
    {
        NtClose(File);
        return;
    }
    Status = NtReadFile(File, NULL, NULL, NULL, &IoStatus, Buffer, SMP_S1_STATE_MAX, NULL, NULL);
    NtClose(File);
    Count = NT_SUCCESS(Status) ? IoStatus.Information : 0;
    if (Count > SMP_S1_STATE_MAX)
        Count = SMP_S1_STATE_MAX;
    Buffer[Count] = ANSI_NULL;
    if (Count >= 5 && memcmp(Buffer, "ARMED", 5) == 0)
    {
        for (Line = Buffer; Line && *Line; Line = Next)
        {
            PCSTR Base;
            PCHAR p;

            Next = strchr(Line, '\n');
            if (Next)
                *Next++ = ANSI_NULL;
            for (p = Line + strlen(Line); p > Line && (p[-1] == '\r' || p[-1] == ' '); p--)
                p[-1] = ANSI_NULL;
            if (_strnicmp(Line, "old ", 4) != 0)
                continue;
            Base = Line + 4;
            for (p = Line + 4; *p; p++)
            {
                if (*p == '\\' || *p == '/')
                    Base = p + 1;
            }
            if (!*Base || strlen(Base) >= SMP_S1_NAME_CCH || SmpS1OldCount >= SMP_S1_MAX_OLD)
                continue;
            strcpy(SmpS1Old[SmpS1OldCount++], Base);
        }
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
    if (SmpS1OldCount)
        DPRINT1("SMSS: fallback boot, %lu swapped file(s) start from their .old pre-image\n", SmpS1OldCount);
}

/*
 * FileName is the image a process is about to be started from. On a fallback boot, when its base name is
 * listed and <stem>.old exists, OldName (heap buffer, RtlFreeHeap) gets that path and *Substituted is TRUE.
 */
NTSTATUS
NTAPI
SmpS1OldImageName(IN PUNICODE_STRING FileName,
                  OUT PUNICODE_STRING OldName,
                  OUT PBOOLEAN Substituted)
{
    ULONG Chars, i, BaseChars, StemChars;
    PWCHAR Base, Dot;
    OBJECT_ATTRIBUTES Attr;
    IO_STATUS_BLOCK IoStatus;
    HANDLE File;
    NTSTATUS Status;

    *Substituted = FALSE;
    RtlInitEmptyUnicodeString(OldName, NULL, 0);
    if (!SmpS1Checked)
    {
        SmpS1Checked = TRUE;
        if (SmpS1IsFallbackBoot())
            SmpS1ReadList();
    }
    if (SmpS1OldCount == 0)
        return STATUS_SUCCESS;

    Chars = FileName->Length / sizeof(WCHAR);
    Base = FileName->Buffer;
    Dot = NULL;
    for (i = 0; i < Chars; i++)
    {
        if (FileName->Buffer[i] == L'\\')
        {
            Base = &FileName->Buffer[i + 1];
            Dot = NULL;
        }
        else if (FileName->Buffer[i] == L'.')
            Dot = &FileName->Buffer[i];
    }
    BaseChars = (ULONG)(FileName->Buffer + Chars - Base);
    if (BaseChars == 0 || BaseChars >= SMP_S1_NAME_CCH)
        return STATUS_SUCCESS;
    for (i = 0; i < SmpS1OldCount; i++)
    {
        ULONG k;

        for (k = 0; k < BaseChars && SmpS1Old[i][k]; k++)
        {
            if (RtlUpcaseUnicodeChar(Base[k]) != RtlUpcaseUnicodeChar((WCHAR)(UCHAR)SmpS1Old[i][k]))
                break;
        }
        if (k == BaseChars && !SmpS1Old[i][k])
            break;
    }
    if (i == SmpS1OldCount)
        return STATUS_SUCCESS;

    StemChars = Dot ? (ULONG)(Dot - FileName->Buffer) : Chars;
    OldName->MaximumLength = (USHORT)((StemChars + 5) * sizeof(WCHAR));
    OldName->Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, OldName->MaximumLength);
    if (!OldName->Buffer)
        return STATUS_NO_MEMORY;
    RtlCopyMemory(OldName->Buffer, FileName->Buffer, StemChars * sizeof(WCHAR));
    RtlCopyMemory(OldName->Buffer + StemChars, L".old", 5 * sizeof(WCHAR));
    OldName->Length = (USHORT)((StemChars + 4) * sizeof(WCHAR));

    InitializeObjectAttributes(&Attr, OldName, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&File, FILE_READ_ATTRIBUTES, &Attr, &IoStatus,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SMSS: fallback boot, '%wZ' has no .old pre-image, starting it as named\n", FileName);
        RtlFreeHeap(RtlGetProcessHeap(), 0, OldName->Buffer);
        RtlInitEmptyUnicodeString(OldName, NULL, 0);
        return STATUS_SUCCESS;
    }
    NtClose(File);
    DPRINT1("SMSS: fallback boot, starting '%wZ' instead of '%wZ'\n", OldName, FileName);
    *Substituted = TRUE;
    return STATUS_SUCCESS;
}
