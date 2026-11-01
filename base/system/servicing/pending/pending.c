/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     First-boot pending operations applier (bootstrap-stable)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * LiberNTUpdatePending.exe runs from the SMSS BootExecute list before the
 * service control manager starts. It applies the pending plan of LiberNT
 * Update per S1-CONTRACTS.md: the plan itself is NOT trusted (it is a local
 * view); the trust root is the package manifest signature made on the host
 * (manifest.sig, RSA-2048 PKCS#1 v1.5 over SHA-256, development key). This
 * binary re-verifies the manifest with the portable s1core (no wintrust),
 * checks every operation against the signed manifest, applies file
 * replacements with per-operation backups and atomic renames, verifies the
 * final on-disk hashes before declaring the transaction applied, and keeps
 * the hash-chained journal and the attempt counter on disk before any next
 * step is visible.
 *
 * Rollback: two boot attempts without a health confirmation revert the
 * whole transaction from the backup, distinguishing a verifiable failure
 * (failureRecorded) from an unexpected reboot (lastResult "started" without
 * a closing entry). The process always exits 0 so a bad package can never
 * wedge the boot; recovery is this rule or the offline rescue CLI. The one
 * nonzero exit is an unwritable journal: the failure itself cannot be
 * recorded, and a silently skipped entry is worse than a loud boot.
 *
 * Native only: ntdll plus the portable s1core. Never the kernel (stage 1).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* The servicing tree lives at the root of the system volume. The native
 * build resolves it at runtime from the \SystemRoot link, which names the
 * Windows directory ("\??\C:\Windows" or its ARC form) — NOT the volume
 * root. A compile-time \SystemRoot\LiberNT\Servicing made the applier look
 * for the plan under the Windows directory, find nothing, and silently exit
 * (found by Dwight's online run 4). The host shim provides the same g_*
 * globals via ShimSetFakeRoot. */
#ifdef S1_HOST_APPLIER
#include "s1host_shim.h"
#else
#include <ntndk.h>

WCHAR g_ServicingRoot[MAX_PATH * 2];
WCHAR g_VolumeRoot[MAX_PATH * 2];
WCHAR g_S1PlanPath[MAX_PATH * 2];
WCHAR g_S1AttemptPath[MAX_PATH * 2];
WCHAR g_S1JournalPath[MAX_PATH * 2];
WCHAR g_S1BackupPath[MAX_PATH * 2];

#define S1_ROOT            g_ServicingRoot
#define S1_PLAN_PATH       g_S1PlanPath
#define S1_ATTEMPT_PATH    g_S1AttemptPath
#define S1_JOURNAL_PATH    g_S1JournalPath
#define S1_BACKUP_PATH     g_S1BackupPath
#endif

#ifndef _In_
#define _In_
#define _Out_
#define _Inout_
#define _In_opt_
#define _Out_opt_
#endif

#include "s1core.h"

/* Plan paths are ASCII by contract (S1-CONTRACTS.md), so the widening copy
 * of every target this binary applies is exact. */
static void
S1Widen(
    _Out_ PWSTR Dst,
    _In_ const char *Src,
    _In_ size_t DstCch)
{
    size_t i;
    for (i = 0; i + 1 < DstCch && Src[i]; i++)
        Dst[i] = (WCHAR)(unsigned char)Src[i];
    Dst[i] = 0;
}

/* Development public key (the matching private key stays outside the
 * repository and the test systems; S1-CONTRACTS.md section 1b pins this in
 * the protected store as well). */
static const uint8_t S1DevPublicKey[S1_RSA2048_SIZE] = {
0xb9,0x0b,0xa9,0xbc,0xc2,0x0c,0x37,0x11,0x12,0xf6,0x46,0x48,0xdc,0x9e,0x77,0x0e,
0x82,0x59,0xc0,0x06,0x84,0x81,0x0a,0xbf,0xb7,0x39,0x7c,0xdc,0x5c,0x40,0xe7,0x94,
0xac,0x76,0xee,0x29,0x42,0x07,0x91,0x62,0x95,0x01,0x08,0x68,0xf2,0xd9,0xf2,0x4f,
0x8c,0x26,0x97,0x00,0x8e,0x3a,0x44,0xef,0x60,0xea,0x5c,0x7a,0x21,0x4b,0x20,0x2b,
0x70,0x5c,0xea,0xa5,0xf0,0x1d,0xd8,0xa7,0x4b,0xdf,0xd6,0x7d,0xc3,0xf6,0x91,0x85,
0x40,0x04,0xac,0x79,0xca,0x25,0xe8,0xb5,0x58,0x62,0xde,0xc0,0xc2,0xfb,0x05,0xf7,
0xa9,0xce,0x81,0xe4,0x7e,0x87,0xd3,0xa4,0x5a,0x74,0xc2,0x54,0xf3,0x9a,0xb3,0x89,
0x1b,0x88,0x1f,0xd6,0x6d,0x9c,0xb0,0x81,0x9d,0xd5,0x24,0xfb,0x96,0x08,0xdf,0x46,
0x92,0x86,0x91,0xc2,0x64,0x58,0x58,0x2a,0xea,0x31,0x37,0x53,0x27,0x23,0xd1,0xc5,
0x5d,0x2b,0x8d,0xb1,0xeb,0xea,0xe7,0x1f,0x46,0xee,0xa4,0x6a,0xaf,0x03,0x48,0xe2,
0x75,0x02,0x94,0x7d,0x70,0x5a,0xbf,0xb0,0xa7,0xde,0xb2,0xbb,0x81,0x6f,0x5a,0x63,
0xe4,0xff,0x7f,0x58,0x21,0x8c,0x0b,0xf8,0x99,0x3b,0x00,0x58,0xdb,0x25,0xac,0xe6,
0x68,0xc9,0x3e,0xda,0xdc,0xfc,0xf1,0x1b,0x7a,0x88,0xd6,0x9d,0xef,0x13,0x57,0x78,
0x24,0xc3,0x2e,0xdb,0xdb,0xda,0x47,0xd3,0x1a,0x46,0x82,0x75,0x9a,0xfd,0x9e,0x31,
0xa6,0xb6,0xe6,0x69,0xe4,0x0e,0x70,0x31,0x52,0xef,0x3b,0x9d,0x6d,0x65,0xc5,0x22,
0xc8,0xd7,0x6f,0x49,0x18,0x66,0x30,0xb5,0x2c,0x17,0x50,0x6f,0xea,0x27,0xeb,0x65,
};

/* No ntstatus value names a content-hash mismatch; this is a private,
 * customer-bit code the journal can record precisely. */
#ifndef STATUS_S1_HASH_MISMATCH
#define STATUS_S1_HASH_MISMATCH ((NTSTATUS)0xE0000001L)
#endif

/* ------------------------------------------------------------------ native IO */

static NTSTATUS
S1NtOpen(
    _In_ PCWSTR Path,
    _In_ ACCESS_MASK Access,
    _In_ ULONG CreateDisposition,
    _Out_ PHANDLE Handle)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attr;
    IO_STATUS_BLOCK IoStatus;
    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attr, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NtCreateFile(Handle, Access | SYNCHRONIZE, &Attr, &IoStatus, NULL,
                        FILE_ATTRIBUTE_NORMAL,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        CreateDisposition, FILE_SYNCHRONOUS_IO_NONALERT |
                        ((CreateDisposition == FILE_CREATE || CreateDisposition == FILE_OPEN_IF)
                         ? FILE_NON_DIRECTORY_FILE : 0), NULL, 0);
}

static NTSTATUS
S1ReadWholeFile(
    _In_ PCWSTR Path,
    _Out_ PVOID *Buffer,
    _Out_ PULONG Size)
{
    HANDLE Handle;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    FILE_STANDARD_INFORMATION Info;
    PVOID Data;

    *Buffer = NULL;
    *Size = 0;
    Status = S1NtOpen(Path, FILE_READ_DATA, FILE_OPEN, &Handle);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = NtQueryInformationFile(Handle, &IoStatus, &Info, sizeof(Info),
                                    FileStandardInformation);
    if (!NT_SUCCESS(Status))
    {
        NtClose(Handle);
        return Status;
    }
    Data = RtlAllocateHeap(RtlGetProcessHeap(), 0, (SIZE_T)Info.EndOfFile.QuadPart + 1);
    if (!Data)
    {
        NtClose(Handle);
        return STATUS_NO_MEMORY;
    }
    Status = NtReadFile(Handle, NULL, NULL, NULL, &IoStatus, Data,
                        (ULONG)Info.EndOfFile.QuadPart, NULL, NULL);
    NtClose(Handle);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Data);
        return Status;
    }
    ((PUCHAR)Data)[IoStatus.Information] = 0;
    *Buffer = Data;
    *Size = (ULONG)IoStatus.Information;
    return STATUS_SUCCESS;
}

/* A real-time scan of a small file resolves in milliseconds but can exceed
 * a sub-second budget; waiting seconds is cheap next to a lost audit
 * entry, so retried opens wait generously. */
#define S1_JOURNAL_APPEND_ATTEMPTS 25

/* Whole-file rewrite in place: open (create if missing), truncate, write,
 * flush. The boot writer used to go through a temp file + rename, but the
 * rename needs directory rights (DELETE on the temp, add-file on the
 * parent) that the BootExecute context does not reliably hold at smss
 * time: LiberNT denied the well-formed rename with STATUS_ACCESS_DENIED
 * (Dwight's run 6). A whole-file rewrite needs only write access to the
 * file, which the protected servicing DACL always grants the writer; a
 * crash mid-write leaves the file torn but recoverable (the plan stays
 * pending and the two-attempt rule reverts from the backup). */
static VOID S1Log(_In_ PCWSTR Msg);

/* Name the failing step of a whole-file rewrite on the debug channel: the
 * caller only sees the last status, and a boot-time failure on a real
 * volume has otherwise no trace of which call refused. */
static VOID
S1LogWriteStep(
    _In_ PCWSTR Step,
    _In_ NTSTATUS Status)
{
    WCHAR Line[96];

    swprintf(Line, RTL_NUMBER_OF(Line), L"S1: file rewrite step '%ls' failed 0x%08lx",
             Step, (unsigned long)Status);
    S1Log(Line);
}

/* Write Data at offset 0 of an open handle, trim the file to Size only if it
 * was longer, and flush. The old order set the end of file to 0 first and
 * wrote afterwards. On the dx2400 the second write of a file (every journal
 * anchor after the first) failed with STATUS_INFO_LENGTH_MISMATCH
 * (0xC0000004) while the first, on an empty file, worked; the trace did not
 * say which call refused. This order never truncates to 0 and never shrinks
 * a file unless the new content is shorter, and every step now logs its own
 * status so a repeat on the box names the call. */
static NTSTATUS
S1WriteHandleFrom0(
    _In_ HANDLE Handle,
    _In_ PVOID Data,
    _In_ ULONG Size,
    _Out_ PBOOLEAN TrimFailed)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset;
    FILE_STANDARD_INFORMATION Info;
    FILE_END_OF_FILE_INFORMATION Eof;

    *TrimFailed = FALSE;
    Offset.QuadPart = 0;
    Status = NtWriteFile(Handle, NULL, NULL, NULL, &IoStatus, Data, Size, &Offset, NULL);
    if (!NT_SUCCESS(Status))
    {
        S1LogWriteStep(L"write", Status);
        return Status;
    }
    Status = NtQueryInformationFile(Handle, &IoStatus, &Info, sizeof(Info),
                                    FileStandardInformation);
    if (NT_SUCCESS(Status) && Info.EndOfFile.QuadPart > (LONGLONG)Size)
    {
        Eof.EndOfFile.QuadPart = (LONGLONG)Size;
        Status = NtSetInformationFile(Handle, &IoStatus, &Eof, sizeof(Eof),
                                      FileEndOfFileInformation);
        if (!NT_SUCCESS(Status))
        {
            S1LogWriteStep(L"trim", Status);
            *TrimFailed = TRUE;
            return Status;
        }
    }
    else if (!NT_SUCCESS(Status))
    {
        S1LogWriteStep(L"query-size", Status);
        return Status;
    }
    Status = NtFlushBuffersFile(Handle, &IoStatus);
    if (!NT_SUCCESS(Status))
        S1LogWriteStep(L"flush", Status);
    return Status;
}

static NTSTATUS
S1WriteFileDirect(
    _In_ PCWSTR Path,
    _In_ PVOID Data,
    _In_ ULONG Size)
{
    HANDLE Handle;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    BOOLEAN TrimFailed;
    ULONG Try;

    for (Try = 0; ; Try++)
    {
        Status = S1NtOpen(Path, FILE_WRITE_DATA, FILE_OPEN_IF, &Handle);
        if (NT_SUCCESS(Status))
            break;
        if ((Status != STATUS_SHARING_VIOLATION && Status != STATUS_LOCK_NOT_GRANTED) ||
            Try + 1 >= S1_JOURNAL_APPEND_ATTEMPTS)
        {
            S1LogWriteStep(L"open", Status);
            return Status;
        }
        {
            LARGE_INTEGER Delay;

            Delay.QuadPart = -1000000; /* 100 ms */
            NtDelayExecution(FALSE, &Delay);
        }
    }
    Status = S1WriteHandleFrom0(Handle, Data, Size, &TrimFailed);
    NtClose(Handle);
    if (!TrimFailed)
        return Status;

    /* The file could not be shortened in place: replace it. Deleting needs
     * only DELETE on the file, as the plan removal already relies on. */
    Status = S1NtOpen(Path, DELETE, FILE_OPEN, &Handle);
    if (NT_SUCCESS(Status))
    {
        FILE_DISPOSITION_INFORMATION DeleteInfo;

        DeleteInfo.DeleteFile = TRUE;
        Status = NtSetInformationFile(Handle, &IoStatus, &DeleteInfo, sizeof(DeleteInfo),
                                      FileDispositionInformation);
        NtClose(Handle);
    }
    if (!NT_SUCCESS(Status))
    {
        S1LogWriteStep(L"delete", Status);
        return Status;
    }
    Status = S1NtOpen(Path, FILE_WRITE_DATA, FILE_CREATE, &Handle);
    if (!NT_SUCCESS(Status))
    {
        S1LogWriteStep(L"recreate", Status);
        return Status;
    }
    Status = S1WriteHandleFrom0(Handle, Data, Size, &TrimFailed);
    NtClose(Handle);
    return Status;
}

static NTSTATUS
S1MoveFileEx(
    _In_ PCWSTR Source,
    _In_ PCWSTR Target,
    _In_ BOOLEAN ReplaceIfExists)
{
    HANDLE Handle;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    FILE_RENAME_INFORMATION *Rename;
    UNICODE_STRING NewName;
    SIZE_T BufferSize;

    /* Rename-by-handle requires DELETE access on the handle (Windows and
     * LiberNT), so the open must grant it and share FILE_SHARE_DELETE. */
    Status = S1NtOpen(Source, FILE_READ_DATA | FILE_WRITE_DATA | DELETE,
                      FILE_OPEN, &Handle);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlInitUnicodeString(&NewName, Target);
    BufferSize = sizeof(FILE_RENAME_INFORMATION) + NewName.Length;
    Rename = RtlAllocateHeap(RtlGetProcessHeap(), 0, BufferSize);
    if (!Rename)
    {
        NtClose(Handle);
        return STATUS_NO_MEMORY;
    }
    Rename->ReplaceIfExists = ReplaceIfExists;
    Rename->RootDirectory = NULL;
    Rename->FileNameLength = NewName.Length;
    memcpy(Rename->FileName, NewName.Buffer, NewName.Length);
    Status = NtSetInformationFile(Handle, &IoStatus, Rename, (ULONG)BufferSize,
                                  FileRenameInformation);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Rename);
    NtClose(Handle);
    if (Status == STATUS_ACCESS_DENIED)
    {
        /* The BootExecute context can hold file rights without directory
         * rights, and the rename walks the parent (Dwight's run 6): fall
         * back to a whole-file copy plus a best-effort delete of the
         * source. A crash between the two leaves both files present,
         * which every reader of this pair already tolerates. */
        PVOID Data;
        ULONG Size;

        Status = S1ReadWholeFile(Source, &Data, &Size);
        if (!NT_SUCCESS(Status))
            return Status;
        Status = S1WriteFileDirect(Target, Data, Size);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Data);
        if (NT_SUCCESS(Status) &&
            NT_SUCCESS(S1NtOpen(Source, DELETE, FILE_OPEN, &Handle)))
        {
            FILE_DISPOSITION_INFORMATION DeleteInfo;

            DeleteInfo.DeleteFile = TRUE;
            NtSetInformationFile(Handle, &IoStatus, &DeleteInfo, sizeof(DeleteInfo),
                                 FileDispositionInformation);
            NtClose(Handle);
        }
    }
    return Status;
}

static VOID S1Log(_In_ PCWSTR Msg);

/* A hash step that fails must say which one and why: the journal used to
 * record every hashing problem as STATUS_S1_HASH_MISMATCH, so a read that
 * failed in the native boot environment looked like a changed file. */
static VOID
S1LogHashFailure(
    _In_ PCWSTR Step,
    _In_ NTSTATUS Status,
    _In_ PCWSTR Path)
{
    WCHAR Line[200];
    SIZE_T Len = wcslen(Path);

    swprintf(Line, RTL_NUMBER_OF(Line), L"S1: %ls: cannot hash 0x%08lx ...%ls",
             Step, (unsigned long)Status, Len > 90 ? Path + Len - 90 : Path);
    S1Log(Line);
}

static VOID
S1LogHashMismatch(
    _In_ PCWSTR Step,
    _In_ PCWSTR Path,
    _In_reads_(S1_SHA256_SIZE) const uint8_t *Actual,
    _In_reads_(S1_SHA256_SIZE) const uint8_t *Expected)
{
    WCHAR Line[220];
    SIZE_T Len = wcslen(Path);

    swprintf(Line, RTL_NUMBER_OF(Line),
             L"S1: %ls: hash differs actual %02x%02x%02x%02x.. expected %02x%02x%02x%02x.. ...%ls",
             Step, Actual[0], Actual[1], Actual[2], Actual[3],
             Expected[0], Expected[1], Expected[2], Expected[3],
             Len > 80 ? Path + Len - 80 : Path);
    S1Log(Line);
}

static NTSTATUS
S1HashFile(
    _In_ PCWSTR Path,
    _Out_ uint8_t Digest[S1_SHA256_SIZE])
{
    HANDLE Handle;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    uint8_t Buffer[65536];
    S1_SHA256_CTX Ctx;

    Status = S1NtOpen(Path, FILE_READ_DATA, FILE_OPEN, &Handle);
    if (!NT_SUCCESS(Status))
        return Status;
    S1Sha256Init(&Ctx);
    for (;;)
    {
        Status = NtReadFile(Handle, NULL, NULL, NULL, &IoStatus, Buffer,
                            sizeof(Buffer), NULL, NULL);
        if (!NT_SUCCESS(Status) || IoStatus.Information == 0)
            break;
        S1Sha256Update(&Ctx, Buffer, IoStatus.Information);
    }
    NtClose(Handle);
    S1Sha256Final(&Ctx, Digest);
    return NT_SUCCESS(Status) || Status == STATUS_END_OF_FILE ? STATUS_SUCCESS : Status;
}

/* --------------------------------------------------------------- paths */

/* Case-insensitive prefix/equality without the CRT's locale tables; the
 * kernel-fixed link names are pure ASCII here. */
static WCHAR
S1Upcase(
    _In_ WCHAR C)
{
    if (C >= L'a' && C <= L'z')
        return C - (L'a' - L'A');
    return C;
}

static BOOL
S1CiEqual(
    _In_ PCWSTR A,
    _In_ PCWSTR B)
{
    while (*A && S1Upcase(*A) == S1Upcase(*B))
    {
        A++;
        B++;
    }
    return *A == 0 && *B == 0;
}

static BOOL
S1CiPrefix(
    _In_ PCWSTR Str,
    _In_ PCWSTR Prefix)
{
    while (*Prefix && S1Upcase(*Str) == S1Upcase(*Prefix))
    {
        Str++;
        Prefix++;
    }
    return *Prefix == 0;
}

/* Compose the servicing root from a \SystemRoot link target, which names
 * Compose the servicing root from a \SystemRoot link target, which names
 * the Windows directory, not the volume root. The volume is derived from
 * its designator — "\??\<X>:", "\ArcName\...partition(n)", "\Device\
 * HarddiskVolumeN" / "Harddisk<n>\Partition<p>" / "CdRom<n>" / "Floppy<n>"
 * — never by guessing the last component, so a Windows directory nested
 * deeper than one level or oddly named ("Win(1)", "HarddiskTools") still
 * resolves to the volume, and an unrecognized shape fails closed (Pam's
 * review of 970c9a4e9c). Pure string logic, separated so host tests pin
 * it. */
static BOOL
S1ServicingRootFromSystemRoot(
    _In_ PCWSTR LinkTarget,
    _Out_ PWSTR Out,
    _In_ size_t OutCch)
{
    WCHAR Comp[64] = { 0 };
    WCHAR Built[MAX_PATH * 2];
    PCWSTR p = LinkTarget;
    size_t Used = 0;

    Built[0] = 0;
    if (*p != L'\\')
        return FALSE;

#define S1_NEXT_COMPONENT() \
    do { \
        size_t N_ = 0; \
        while (*p == L'\\') \
            p++; \
        while (*p && *p != L'\\' && N_ < RTL_NUMBER_OF(Comp) - 1) \
            Comp[N_++] = *p++; \
        Comp[N_] = 0; \
    } while (0)

#define S1_KEEP_COMPONENT() \
    do { \
        if (Used) \
            wcscat(Built, L"\\"); \
        wcscat(Built, Comp); \
        Used++; \
    } while (0)

    S1_NEXT_COMPONENT();              /* "??", "ArcName", "Device", ... */

    if (S1CiEqual(Comp, L"??"))
    {
        /* "\??\C:" — the drive designator is the volume */
        S1_KEEP_COMPONENT();          /* keeps "??" */
        S1_NEXT_COMPONENT();
        if (wcslen(Comp) < 2 || Comp[1] != L':')
            return FALSE;
        S1_KEEP_COMPONENT();          /* keeps "C:" */
    }
    else if (S1CiEqual(Comp, L"ArcName"))
    {
        /* "\ArcName\<multi...>\partition(n)" — the partition component
         * closes the volume designator */
        BOOL Found = FALSE;

        S1_KEEP_COMPONENT();
        while (*p)
        {
            S1_NEXT_COMPONENT();
            S1_KEEP_COMPONENT();
            if (wcsstr(Comp, L"partition("))
            {
                Found = TRUE;
                break;
            }
        }
        if (!Found)
            return FALSE;
    }
    else if (S1CiEqual(Comp, L"Device"))
    {
        S1_KEEP_COMPONENT();
        S1_NEXT_COMPONENT();          /* HarddiskVolumeN | Harddisk<n> | CdRom<n> | Floppy<n> */
        if (S1CiPrefix(Comp, L"HarddiskVolume") ||
            S1CiPrefix(Comp, L"CdRom") ||
            S1CiPrefix(Comp, L"Floppy"))
        {
            S1_KEEP_COMPONENT();
        }
        else if (S1CiPrefix(Comp, L"Harddisk"))
        {
            S1_KEEP_COMPONENT();
            S1_NEXT_COMPONENT();      /* Partition<p> */
            if (!S1CiPrefix(Comp, L"Partition"))
                return FALSE;
            S1_KEEP_COMPONENT();
        }
        else
        {
            return FALSE;
        }
    }
    else
    {
        return FALSE;
    }
#undef S1_NEXT_COMPONENT
#undef S1_KEEP_COMPONENT

    swprintf(Out, OutCch, L"\\%s\\LiberNT\\Servicing", Built);
    return TRUE;
}

#ifndef S1_HOST_APPLIER
static NTSTATUS
S1ResolveServicingRoot(VOID)
{
    UNICODE_STRING Name, Target;
    OBJECT_ATTRIBUTES Attr;
    HANDLE Link;
    WCHAR Buf[MAX_PATH * 2];
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, L"\\SystemRoot");
    InitializeObjectAttributes(&Attr, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenSymbolicLinkObject(&Link, SYMBOLIC_LINK_QUERY, &Attr);
    if (!NT_SUCCESS(Status))
        return Status;
    /* leave room for the terminating WCHAR the query may not write */
    RtlInitEmptyUnicodeString(&Target, Buf, sizeof(Buf) - sizeof(WCHAR));
    Status = NtQuerySymbolicLinkObject(Link, &Target, NULL);
    NtClose(Link);
    if (!NT_SUCCESS(Status))
        return Status;
    Buf[Target.Length / sizeof(WCHAR)] = 0;
    if (!S1ServicingRootFromSystemRoot(Buf, g_ServicingRoot, RTL_NUMBER_OF(g_ServicingRoot)))
        return STATUS_OBJECT_PATH_INVALID;
    {
        static const WCHAR ServicingSuffix[] = L"\\LiberNT\\Servicing";
        size_t RootLen = wcslen(g_ServicingRoot);
        size_t SuffixLen = RTL_NUMBER_OF(ServicingSuffix) - 1;

        if (RootLen <= SuffixLen)
            return STATUS_OBJECT_PATH_INVALID;
        memcpy(g_VolumeRoot, g_ServicingRoot, (RootLen - SuffixLen) * sizeof(WCHAR));
        g_VolumeRoot[RootLen - SuffixLen] = 0;
    }
    swprintf(g_S1PlanPath, RTL_NUMBER_OF(g_S1PlanPath), L"%s\\pending\\plan.json", g_ServicingRoot);
    swprintf(g_S1AttemptPath, RTL_NUMBER_OF(g_S1AttemptPath), L"%s\\pending\\attempt.json", g_ServicingRoot);
    swprintf(g_S1JournalPath, RTL_NUMBER_OF(g_S1JournalPath), L"%s\\journal\\transactions.jsonl", g_ServicingRoot);
    swprintf(g_S1BackupPath, RTL_NUMBER_OF(g_S1BackupPath), L"%s\\backup", g_ServicingRoot);
    return STATUS_SUCCESS;
}
#endif

/* --------------------------------------------------------------- journal */

typedef struct _S1_JOURNAL_WRITER
{
    uint8_t PrevHash[S1_SHA256_SIZE];
    uint64_t Sequence;
} S1_JOURNAL_WRITER;

/* A real-time scan of a small file resolves in milliseconds but can exceed
 * a sub-second budget; waiting seconds is cheap next to a lost audit
 * entry, so the open is retried generously (S1_JOURNAL_APPEND_ATTEMPTS,
 * defined above with the file writers). */

static VOID S1Log(_In_ PCWSTR Msg);

static NTSTATUS
S1JournalWriterAppend(
    _Inout_ S1_JOURNAL_WRITER *Writer,
    _In_ const char *TransactionId,
    _In_ const char *Phase,
    _In_ int Operation,
    _In_ const char *Result,
    _In_ const char *Error)
{
    /* Append through a growable in-memory view is not possible natively;
     * each entry is formatted with s1core and written at the file end. */
    char Entry[S1_JOURNAL_MAX_ENTRY];
    char PrevHex[65];
    int Len;
    S1_SHA256_CTX Ctx;
    HANDLE Handle;
    NTSTATUS Status;
    ULONG Try;
    IO_STATUS_BLOCK IoStatus;
    FILE_POSITION_INFORMATION PosInfo;
    FILE_STANDARD_INFORMATION StdInfo;
    uint8_t NewHash[S1_SHA256_SIZE];

    S1HexEncode(Writer->PrevHash, S1_SHA256_SIZE, PrevHex);
    if (Operation >= 0)
        Len = snprintf(Entry, sizeof(Entry),
                       "{\"seq\":%llu,\"prev\":\"%s\",\"ts\":\"boot\",\"tx\":\"%s\",\"phase\":\"%s\",\"op\":%d,\"result\":\"%s\",\"error\":%s%s%s}\n",
                       (unsigned long long)(Writer->Sequence + 1), PrevHex,
                       TransactionId, Phase, Operation, Result,
                       Error ? "\"" : "null", Error ? Error : "", Error ? "\"" : "");
    else
        Len = snprintf(Entry, sizeof(Entry),
                       "{\"seq\":%llu,\"prev\":\"%s\",\"ts\":\"boot\",\"tx\":\"%s\",\"phase\":\"%s\",\"op\":null,\"result\":\"%s\",\"error\":%s%s%s}\n",
                       (unsigned long long)(Writer->Sequence + 1), PrevHex,
                       TransactionId, Phase, Result,
                       Error ? "\"" : "null", Error ? Error : "", Error ? "\"" : "");
    if (Len <= 0 || (size_t)Len >= sizeof(Entry))
        return STATUS_BUFFER_OVERFLOW;

    /* Real-time scanners briefly open the journal with restrictive
     * sharing; a boot-time writer must out-wait that instead of losing
     * the entry. */
    for (Try = 0; ; Try++)
    {
        Status = S1NtOpen(S1_JOURNAL_PATH, FILE_WRITE_DATA, FILE_OPEN_IF, &Handle);
        if (NT_SUCCESS(Status))
            break;
        if ((Status != STATUS_SHARING_VIOLATION && Status != STATUS_LOCK_NOT_GRANTED) ||
            Try + 1 >= S1_JOURNAL_APPEND_ATTEMPTS)
            return Status;
        {
            LARGE_INTEGER Delay;

            Delay.QuadPart = -1000000; /* 100 ms; 24 pauses, about 2.4 s */
            NtDelayExecution(FALSE, &Delay);
        }
    }
    Status = NtQueryInformationFile(Handle, &IoStatus, &StdInfo, sizeof(StdInfo),
                                    FileStandardInformation);
    if (!NT_SUCCESS(Status))
    {
        NtClose(Handle);
        return Status;
    }
    PosInfo.CurrentByteOffset = StdInfo.EndOfFile;
    NtSetInformationFile(Handle, &IoStatus, &PosInfo, sizeof(PosInfo),
                         FilePositionInformation);
    Status = NtWriteFile(Handle, NULL, NULL, NULL, &IoStatus, Entry, (ULONG)Len, NULL, NULL);
    if (NT_SUCCESS(Status))
        Status = NtFlushBuffersFile(Handle, &IoStatus);
    NtClose(Handle);
    if (!NT_SUCCESS(Status))
        return Status;

    S1Sha256Init(&Ctx);
    S1Sha256Update(&Ctx, Entry, (size_t)Len - 1);
    S1Sha256Final(&Ctx, NewHash);
    memcpy(Writer->PrevHash, NewHash, S1_SHA256_SIZE);
    Writer->Sequence++;
    /* libservicing keeps journal\anchor.json (seq and hash of the tail
     * entry) no more than one entry behind the journal; a boot writer that
     * appends more than one entry must maintain it too, or the next read
     * reports the journal corrupt. */
    {
        WCHAR AnchorPath[MAX_PATH * 2];
        char Anchor[256], Hex[65];
        int AnchorLen;

        swprintf(AnchorPath, MAX_PATH * 2, L"%ls\\journal\\anchor.json", S1_ROOT);
        S1HexEncode(Writer->PrevHash, S1_SHA256_SIZE, Hex);
        AnchorLen = snprintf(Anchor, sizeof(Anchor),
                             "{\"schema\":1,\"seq\":%llu,\"hash\":\"%s\"}\n",
                             (unsigned long long)Writer->Sequence, Hex);
        if (AnchorLen <= 0 || (size_t)AnchorLen >= sizeof(Anchor))
            return STATUS_BUFFER_OVERFLOW;
        {
            NTSTATUS AnchorSt = S1WriteFileDirect(AnchorPath, Anchor, (ULONG)AnchorLen);

            if (!NT_SUCCESS(AnchorSt))
            {
                WCHAR Line[112];

                swprintf(Line, RTL_NUMBER_OF(Line),
                         L"S1: anchor write failed 0x%08lx (root %ls)",
                         (unsigned long)AnchorSt, S1_ROOT);
                S1Log(Line);
            }
            return AnchorSt;
        }
    }
}

/* A phase transition the journal cannot record is a broken audit chain,
 * and the failure cannot be journaled either; the only honest signal
 * left is a nonzero exit. Re-entering after a late halt is safe:
 * S1RevertOperation is a no-op once its backup is consumed (the replace
 * path returns success when the backup is gone and never touches the
 * target, the add path re-deletes nothing), so a repeated rollback or a
 * rollback that reaches an op never applied converges to the same state. */
#ifndef S1_HOST_APPLIER
/* BootExecute processes have no console and NtDisplayString only reaches
 * the boot video (invisible under the graphical splash and absent from
 * the KD serial); DbgPrint lands in the kernel debugger on debug builds,
 * which is the reliable channel for a native applier (Dwight's diag run). */
static VOID
S1Log(
    _In_ PCWSTR Msg)
{
    CHAR Narrow[512];
    SIZE_T i;

    for (i = 0; Msg[i] && i < RTL_NUMBER_OF(Narrow) - 1; i++)
        Narrow[i] = (CHAR)Msg[i];
    Narrow[i] = 0;
    DbgPrint("%s\r\n", Narrow);
}
#else
static VOID
S1Log(_In_ PCWSTR Msg)
{
    (void)Msg;
}
#endif

#define S1JournalOrHalt(j, tx, ph, op, res, err) \
    do { \
        NTSTATUS S1HaltSt_ = S1JournalWriterAppend((j), (tx), (ph), (op), (res), (err)); \
        if (!NT_SUCCESS(S1HaltSt_)) \
        { \
            WCHAR S1HaltLine_[96]; \
            swprintf(S1HaltLine_, RTL_NUMBER_OF(S1HaltLine_), \
                     L"S1: journal append '%hs' failed 0x%08lx, halting", (ph), \
                     (unsigned long)S1HaltSt_); \
            S1Log(S1HaltLine_); \
            NtTerminateProcess(NtCurrentProcess(), 1); \
        } \
    } while (0)

/* The chain is verified on read: load the journal, verify every link, and
 * seed the writer with the last intact entry. A broken chain turns the
 * transaction state into a verifiable failure for the rescue path. */
static NTSTATUS
S1JournalLoad(
    _Out_ S1_JOURNAL_WRITER *Writer,
    _Out_ S1Bool *ChainIntact)
{
    PVOID Data;
    ULONG Size;
    NTSTATUS Status;

    memset(Writer, 0, sizeof(*Writer));
    *ChainIntact = S1_TRUE;
    Status = S1ReadWholeFile(S1_JOURNAL_PATH, &Data, &Size);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
        return STATUS_SUCCESS;
    if (!NT_SUCCESS(Status))
        return Status;
    Writer->Sequence = S1JournalVerifyChain((const char *)Data, Size, ChainIntact);
    if (*ChainIntact && Writer->Sequence > 0)
    {
        /* recover the last hash by re-walking to the final entry */
        const char *Cursor = (const char *)Data;
        const char *End = Cursor + Size;
        while (Cursor < End && *Cursor)
        {
            const char *LineEnd = memchr(Cursor, '\n', (size_t)(End - Cursor));
            if (LineEnd)
            {
                S1_SHA256_CTX Ctx;
                S1Sha256Init(&Ctx);
                S1Sha256Update(&Ctx, Cursor, (size_t)(LineEnd - Cursor));
                S1Sha256Final(&Ctx, Writer->PrevHash);
                Cursor = LineEnd + 1;
            }
            else
                break;
        }
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, Data);
    return STATUS_SUCCESS;
}

/* --------------------------------------------------------------- manifest */

static S1Bool
S1ManifestPathForPlan(
    _In_ const S1_PLAN *Plan,
    _Out_ PWSTR ManifestPath,
    _In_ size_t ManifestPathCch,
    _Out_ PWSTR SignaturePath,
    _In_ size_t SignaturePathCch)
{
    if (swprintf(ManifestPath, (ULONG)ManifestPathCch,
                 L"%ls\\store\\manifests\\%S\\%S\\manifest.json",
                 S1_ROOT, Plan->PackageId, Plan->Version) < 0)
        return S1_FALSE;
    if (swprintf(SignaturePath, (ULONG)SignaturePathCch,
                 L"%ls\\store\\manifests\\%S\\%S\\manifest.sig",
                 S1_ROOT, Plan->PackageId, Plan->Version) < 0)
        return S1_FALSE;
    return S1_TRUE;
}

/* Verify the signed manifest and that every plan operation is listed in it
 * with the same target hash (S1-CONTRACTS.md section 1b). */
static NTSTATUS
S1VerifyPlanAgainstManifest(
    _In_ const S1_PLAN *Plan)
{
    WCHAR ManifestPath[MAX_PATH * 2], SignaturePath[MAX_PATH * 2];
    PVOID ManifestData, SignatureData;
    ULONG ManifestSize, SignatureSize;
    uint8_t ManifestDigest[S1_SHA256_SIZE], DigestHex[S1_SHA256_SIZE * 2 + 1];
    S1_SHA256_CTX Ctx;
    S1_JSON *Root;
    size_t FileCount, i;
    NTSTATUS Status;

    if (!S1ManifestPathForPlan(Plan, ManifestPath, MAX_PATH * 2,
                               SignaturePath, MAX_PATH * 2))
        return STATUS_INVALID_PARAMETER;

    Status = S1ReadWholeFile(ManifestPath, &ManifestData, &ManifestSize);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = S1ReadWholeFile(SignaturePath, &SignatureData, &SignatureSize);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, ManifestData);
        return Status;
    }
    if (SignatureSize != S1_RSA2048_SIZE)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, ManifestData);
        RtlFreeHeap(RtlGetProcessHeap(), 0, SignatureData);
        return STATUS_INVALID_SIGNATURE;
    }

    if (!S1Rsa2048Sha256Verify(S1DevPublicKey, 65537, ManifestData, ManifestSize,
                               SignatureData))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, ManifestData);
        RtlFreeHeap(RtlGetProcessHeap(), 0, SignatureData);
        return STATUS_INVALID_SIGNATURE;
    }

    S1Sha256Init(&Ctx);
    S1Sha256Update(&Ctx, ManifestData, ManifestSize);
    S1Sha256Final(&Ctx, ManifestDigest);
    if (memcmp(ManifestDigest, Plan->ManifestDigest, S1_SHA256_SIZE) != 0)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, ManifestData);
        RtlFreeHeap(RtlGetProcessHeap(), 0, SignatureData);
        return STATUS_INVALID_SIGNATURE;
    }

    if (!S1JsonParse((const char *)ManifestData, ManifestSize, &Root))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, ManifestData);
        RtlFreeHeap(RtlGetProcessHeap(), 0, SignatureData);
        return STATUS_INVALID_PARAMETER;
    }

    /* Every plan operation must appear in the signed manifest with the same
     * target path and expected hash. The manifest schema lists the payloads
     * it ships under "payloads" with the target the bytes install to. */
    FileCount = S1JsonGetArrayCount(Root, "payloads");
    for (i = 0; i < Plan->OperationCount; i++)
    {
        const S1_PLAN_OP *Op = &Plan->Operations[i];
        const char *PlanTarget = Op->Target;
        char Path[64];
        const char *ManifestPathA, *ManifestSha;
        size_t PathLen, ShaLen;
        size_t f;
        S1Bool Found = S1_FALSE;
        uint8_t Expected[S1_SHA256_SIZE];

        if (PlanTarget[0] == '\\')
            PlanTarget++;
        for (f = 0; f < FileCount; f++)
        {
            snprintf(Path, sizeof(Path), "payloads.%zu.target", f);
            if (!S1JsonGetString(Root, Path, &ManifestPathA, &PathLen))
                continue;
            snprintf(Path, sizeof(Path), "payloads.%zu.sha256", f);
            if (!S1JsonGetString(Root, Path, &ManifestSha, &ShaLen))
                continue;
            if (PathLen == strlen(PlanTarget) && memcmp(ManifestPathA, PlanTarget, PathLen) == 0 &&
                ShaLen == S1_SHA256_SIZE * 2 && S1HexDecode(ManifestSha, Expected, S1_SHA256_SIZE) &&
                memcmp(Expected, Op->ExpectedSha256, S1_SHA256_SIZE) == 0)
            {
                Found = S1_TRUE;
                break;
            }
        }
        if (!Found)
        {
            RtlFreeHeap(RtlGetProcessHeap(), 0, ManifestData);
            RtlFreeHeap(RtlGetProcessHeap(), 0, SignatureData);
            return STATUS_INVALID_SIGNATURE;
        }
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, ManifestData);
    RtlFreeHeap(RtlGetProcessHeap(), 0, SignatureData);
    S1HexEncode(ManifestDigest, S1_SHA256_SIZE, (char *)DigestHex);
    return STATUS_SUCCESS;
}

/* --------------------------------------------------------------- attempt */

static NTSTATUS
S1AttemptLoad(
    _In_ const char *TransactionId,
    _Out_ S1_ATTEMPT *Attempt)
{
    PVOID Data;
    ULONG Size;
    NTSTATUS Status;

    memset(Attempt, 0, sizeof(*Attempt));
    Attempt->Schema = 1;
    snprintf(Attempt->TransactionId, sizeof(Attempt->TransactionId), "%s", TransactionId);
    Attempt->Attempt = 0;
    Attempt->HealthConfirmed = S1_FALSE;
    Attempt->FailureRecorded = S1_FALSE;
    snprintf(Attempt->LastResult, sizeof(Attempt->LastResult), "%s", "none");

    Status = S1ReadWholeFile(S1_ATTEMPT_PATH, &Data, &Size);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
        return STATUS_SUCCESS;
    if (!NT_SUCCESS(Status))
        return Status;
    if (!S1AttemptParse((const char *)Data, Size, Attempt) ||
        strcmp(Attempt->TransactionId, TransactionId) != 0)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Data);
        return STATUS_INVALID_PARAMETER;
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, Data);
    return STATUS_SUCCESS;
}

static NTSTATUS
S1AttemptSave(
    _In_ const S1_ATTEMPT *Attempt)
{
    char Buffer[512];
    size_t Len;

    Len = S1AttemptSerialize(Attempt, Buffer, sizeof(Buffer));
    if (Len == 0)
        return STATUS_BUFFER_OVERFLOW;
    return S1WriteFileDirect(S1_ATTEMPT_PATH, Buffer, (ULONG)Len);
}

/* --------------------------------------------------------------- apply/rollback */

static NTSTATUS
S1BuildObjectPath(
    _In_ const S1_PLAN_OP *Op,
    _Out_ PWSTR ObjectPath,
    _In_ size_t ObjectPathCch)
{
    char ShaHex[S1_SHA256_SIZE * 2 + 1];

    S1HexEncode(Op->ExpectedSha256, S1_SHA256_SIZE, ShaHex);
    if (swprintf(ObjectPath, (ULONG)ObjectPathCch,
                 L"%ls\\store\\objects\\%S", S1_ROOT, ShaHex) < 0)
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

static NTSTATUS
S1BuildBackupPath(
    _In_ const S1_PLAN *Plan,
    _In_ const S1_PLAN_OP *Op,
    _Out_ PWSTR BackupPath,
    _In_ size_t BackupPathCch)
{
    if (swprintf(BackupPath, (ULONG)BackupPathCch,
                 L"%ls\\%S\\%04lu", S1_BACKUP_PATH, Plan->TransactionId,
                 (unsigned long)Op->Seq) < 0)
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

static NTSTATUS
S1ApplyOperation(
    _In_ const S1_PLAN *Plan,
    _In_ const S1_PLAN_OP *Op)
{
    WCHAR ObjectPath[MAX_PATH * 2], BackupPath[MAX_PATH * 2], TargetW[MAX_PATH * 2];
    NTSTATUS Status;
    uint8_t Actual[S1_SHA256_SIZE];
    HANDLE Probe;

    S1Widen(TargetW, Op->Target, MAX_PATH * 2);
    {
        /* Plan targets are volume-root-relative ("\Windows\System32\...").
         * Both builds prefix the volume root — the real one resolved from
         * \SystemRoot at boot, the fake root under the host shim — so the
         * host E2E composes paths exactly like the guest (the shim used to
         * hide this, Dwight's run 7). */
        WCHAR FullTarget[MAX_PATH * 4];

        if (swprintf(FullTarget, RTL_NUMBER_OF(FullTarget), L"%s%s",
                     g_VolumeRoot, TargetW) > 0)
            wcscpy(TargetW, FullTarget);
    }
    Status = S1BuildObjectPath(Op, ObjectPath, MAX_PATH * 2);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = S1BuildBackupPath(Plan, Op, BackupPath, MAX_PATH * 2);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Backup the current target if it exists (replace-file); add-file has no
     * backup and a revert deletes the target. The engine stages a verified
     * backup before it defers an operation to the boot plan, so an existing
     * backup is kept as-is. */
    if (Op->Type == S1_OP_REPLACE_FILE)
    {
        Status = S1NtOpen(TargetW, FILE_READ_DATA, FILE_OPEN, &Probe);
        if (NT_SUCCESS(Status))
        {
            HANDLE BackupProbe;
            uint8_t Current[S1_SHA256_SIZE];

            NtClose(Probe);
            /* The target must still be what the plan recorded: a drifted
             * target means the backup would capture the wrong bytes and
             * the later originalSha256 restore check would fail. */
            if (Op->HasOriginal == S1_TRUE)
            {
                NTSTATUS HashSt = S1HashFile(TargetW, Current);

                if (!NT_SUCCESS(HashSt))
                {
                    /* a hashing failure is reported as itself, not as a changed file */
                    S1LogHashFailure(L"target before replace", HashSt, TargetW);
                    return HashSt;
                }
                if (memcmp(Current, Op->OriginalSha256, S1_SHA256_SIZE) != 0)
                {
                    S1LogHashMismatch(L"target before replace", TargetW, Current, Op->OriginalSha256);
                    return STATUS_S1_HASH_MISMATCH;
                }
            }
            Status = S1NtOpen(BackupPath, FILE_READ_DATA, FILE_OPEN, &BackupProbe);
            if (NT_SUCCESS(Status))
                NtClose(BackupProbe);
            else
            {
                Status = S1MoveFileEx(TargetW, BackupPath, FALSE);
                if (!NT_SUCCESS(Status))
                    return Status;
            }
        }
    }

    /* Whole-file replace/add from the store object (direct write, as the
     * boot context cannot rely on rename rights). The object stays in the
     * store: confirm re-verifies the store against the pinned certificate,
     * and immutable objects are shared with later transactions. */
    {
        PVOID ObjectData;
        ULONG ObjectSize;

        Status = S1ReadWholeFile(ObjectPath, &ObjectData, &ObjectSize);
        if (!NT_SUCCESS(Status))
        {
            if (Op->Type == S1_OP_REPLACE_FILE)
                S1MoveFileEx(BackupPath, TargetW, TRUE);
            return Status;
        }
        Status = S1WriteFileDirect(TargetW, ObjectData, ObjectSize);
        RtlFreeHeap(RtlGetProcessHeap(), 0, ObjectData);
        if (!NT_SUCCESS(Status))
        {
            if (Op->Type == S1_OP_REPLACE_FILE)
                S1MoveFileEx(BackupPath, TargetW, TRUE);
            return Status;
        }
    }

    /* The component is not installed until its final hash matches. */
    Status = S1HashFile(TargetW, Actual);
    if (!NT_SUCCESS(Status))
        S1LogHashFailure(L"target after replace", Status, TargetW);
    else if (memcmp(Actual, Op->ExpectedSha256, S1_SHA256_SIZE) != 0)
        S1LogHashMismatch(L"target after replace", TargetW, Actual, Op->ExpectedSha256);
    if (!NT_SUCCESS(Status) || memcmp(Actual, Op->ExpectedSha256, S1_SHA256_SIZE) != 0)
    {
        /* revert this operation */
        {
            HANDLE Handle;
            FILE_DISPOSITION_INFORMATION DeleteInfo;
            IO_STATUS_BLOCK IoStatus;
            if (NT_SUCCESS(S1NtOpen(TargetW, DELETE, FILE_OPEN, &Handle)))
            {
                DeleteInfo.DeleteFile = TRUE;
                NtSetInformationFile(Handle, &IoStatus, &DeleteInfo,
                                     sizeof(DeleteInfo), FileDispositionInformation);
                NtClose(Handle);
            }
        }
        if (Op->Type == S1_OP_REPLACE_FILE)
            S1MoveFileEx(BackupPath, TargetW, TRUE);
        return STATUS_S1_HASH_MISMATCH;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
S1RevertOperation(
    _In_ const S1_PLAN *Plan,
    _In_ const S1_PLAN_OP *Op)
{
    WCHAR BackupPath[MAX_PATH * 2], TargetW[MAX_PATH * 2];
    NTSTATUS Status;
    HANDLE Probe;

    S1Widen(TargetW, Op->Target, MAX_PATH * 2);
    {
        WCHAR FullTarget[MAX_PATH * 4];

        if (swprintf(FullTarget, RTL_NUMBER_OF(FullTarget), L"%s%s",
                     g_VolumeRoot, TargetW) > 0)
            wcscpy(TargetW, FullTarget);
    }
    Status = S1BuildBackupPath(Plan, Op, BackupPath, MAX_PATH * 2);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Op->Type == S1_OP_REPLACE_FILE)
    {
        Status = S1NtOpen(BackupPath, FILE_READ_DATA, FILE_OPEN, &Probe);
        if (NT_SUCCESS(Status))
        {
            uint8_t BackupHash[S1_SHA256_SIZE];

            NtClose(Probe);
            /* Fail closed (owner decision): never restore a backup that
             * does not hash to the original content recorded in the plan;
             * a corrupt or swapped backup means the undo is untrusted. */
            {
                NTSTATUS HashSt = S1HashFile(BackupPath, BackupHash);

                if (!NT_SUCCESS(HashSt))
                {
                    S1LogHashFailure(L"backup before restore", HashSt, BackupPath);
                    return HashSt;
                }
                if (memcmp(BackupHash, Op->OriginalSha256, S1_SHA256_SIZE) != 0)
                {
                    S1LogHashMismatch(L"backup before restore", BackupPath, BackupHash, Op->OriginalSha256);
                    return STATUS_S1_HASH_MISMATCH;
                }
            }
            return S1MoveFileEx(BackupPath, TargetW, TRUE);
        }
        return STATUS_SUCCESS; /* nothing was backed up */
    }

    /* add-file: a revert removes the projected file. */
    Status = S1NtOpen(TargetW, DELETE, FILE_OPEN, &Probe);
    if (NT_SUCCESS(Status))
    {
        FILE_DISPOSITION_INFORMATION DeleteInfo;
        IO_STATUS_BLOCK IoStatus;
        DeleteInfo.DeleteFile = TRUE;
        NtSetInformationFile(Probe, &IoStatus, &DeleteInfo, sizeof(DeleteInfo),
                             FileDispositionInformation);
        NtClose(Probe);
    }
    return STATUS_SUCCESS;
}

/* --------------------------------------------------------------- main */

VOID
NTAPI
NtProcessStartup(
    _In_ PRTL_USER_PROCESS_PARAMETERS ProcessParameters)
{
    S1_JOURNAL_WRITER Journal;
    S1_ATTEMPT Attempt;
    S1_PLAN Plan;
    PVOID PlanData, BackupDirProbe;
    ULONG PlanSize, i;
    S1Bool ChainIntact;
    NTSTATUS Status, OpStatus;

    (void)ProcessParameters;

#ifndef S1_HOST_APPLIER
    /* Find the servicing tree first: everything below reads and writes
     * under it. Failing here means the applier cannot do anything
     * meaningful at all, so exit loudly (smss survives a BootExecute
     * failure; sminit.c discards the status). */
    Status = S1ResolveServicingRoot();
    if (!NT_SUCCESS(Status))
    {
        S1Log(L"S1: cannot resolve the servicing root");
        NtTerminateProcess(NtCurrentProcess(), 1);
    }
    S1Log(g_ServicingRoot);
#endif

    /* 1. No plan is a successful no-op — but say which no-op it is, so a
     * vanished servicing tree does not look identical to a healthy idle
     * boot (Pam's review: the failure modes must be distinguishable). */
    Status = S1ReadWholeFile(S1_PLAN_PATH, &PlanData, &PlanSize);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
    {
#ifndef S1_HOST_APPLIER
        HANDLE RootProbe;

        if (!NT_SUCCESS(S1NtOpen(S1_ROOT, FILE_READ_DATA, FILE_OPEN, &RootProbe)))
            S1Log(L"S1: servicing tree is not installed, exit 0");
        else
        {
            NtClose(RootProbe);
            S1Log(L"S1: no plan, exit 0");
        }
#else
        S1Log(L"S1: no plan, exit 0");
#endif
        NtTerminateProcess(NtCurrentProcess(), 0);
    }
    if (!NT_SUCCESS(Status))
    {
        S1Log(L"S1: plan read failed, exit 0");
        NtTerminateProcess(NtCurrentProcess(), 0);
    }
    S1Log(L"S1: plan found");

    /* 2. Parse and validate the plan shape. */
    if (!S1PlanParse((const char *)PlanData, PlanSize, &Plan) ||
        !S1PlanCheckTransactionId(&Plan))
    {
        S1JournalLoad(&Journal, &ChainIntact);
        S1JournalOrHalt(&Journal, "unknown", "verify", -1, "failed", "0xC0000001");
        RtlFreeHeap(RtlGetProcessHeap(), 0, PlanData);
        NtTerminateProcess(NtCurrentProcess(), 0);
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, PlanData);

    S1JournalLoad(&Journal, &ChainIntact);

    /* 3. Trust root: the signed package manifest (never the plan). */
    Status = S1VerifyPlanAgainstManifest(&Plan);
    if (!NT_SUCCESS(Status))
    {
        S1Log(L"S1: manifest verify failed");
        S1JournalOrHalt(&Journal, Plan.TransactionId, "verify", -1, "failed",
                        "0xC0000020");
        Attempt.Attempt = 1;
        Attempt.FailureRecorded = S1_TRUE;
        Attempt.HealthConfirmed = S1_FALSE;
        snprintf(Attempt.LastResult, sizeof(Attempt.LastResult), "%s", "failed");
        S1AttemptLoad(Plan.TransactionId, &Attempt);
        Attempt.Attempt++;
        Attempt.FailureRecorded = S1_TRUE;
        snprintf(Attempt.LastResult, sizeof(Attempt.LastResult), "%s", "failed");
        S1AttemptSave(&Attempt);
        NtTerminateProcess(NtCurrentProcess(), 0);
    }
    S1JournalOrHalt(&Journal, Plan.TransactionId, "verified", -1, "ok", NULL);
    S1Log(L"S1: plan verified against manifest");

    /* 4. Boot attempt accounting, written and flushed before any work
     * (reconciliation with Meredith: a mid-run power cut must still count). */
    {
        NTSTATUS LoadSt = S1AttemptLoad(Plan.TransactionId, &Attempt);
        WCHAR Line[96];

        swprintf(Line, RTL_NUMBER_OF(Line), L"S1: attempt load 0x%08lx attempt=%lu",
                 (unsigned long)LoadSt, (unsigned long)Attempt.Attempt);
        S1Log(Line);
    }
    {
        S1_ATTEMPT Started = Attempt;
        NTSTATUS SaveSt;

        Started.Attempt = Attempt.Attempt + 1;
        Started.HealthConfirmed = S1_FALSE;
        Started.FailureRecorded = S1_FALSE;
        snprintf(Started.LastResult, sizeof(Started.LastResult), "%s", "started");
        SaveSt = S1AttemptSave(&Started);
        {
            WCHAR Line[96];

            swprintf(Line, RTL_NUMBER_OF(Line), L"S1: attempt save 0x%08lx attempt=%lu",
                     (unsigned long)SaveSt, (unsigned long)Started.Attempt);
            S1Log(Line);
        }
        Attempt = Started;
    }

    /* 5. Two attempts without a health confirmation revert. */
    if (S1ShouldRollback(Attempt.Attempt, Attempt.HealthConfirmed))
    {
        const char *Reason = (Attempt.FailureRecorded || strcmp(Attempt.LastResult, "started") == 0)
                            ? (Attempt.FailureRecorded ? "verifiable-failure" : "unexpected-reboot")
                            : "unexpected-reboot";
        S1Log(L"S1: two attempts without health, rolling back");
        S1JournalOrHalt(&Journal, Plan.TransactionId, "rollback-prepared", -1,
                        "started", Reason);
        {
            /* Best-effort revert (Pam review R2): continue after a failure
             * so one corrupt backup cannot block restoring the remaining
             * operations; the first failure is journaled and exits 1 after
             * the loop instead of halting mid-rollback. */
            NTSTATUS RevertErr = STATUS_SUCCESS;
            ULONG FailedOp = 0;

            for (i = Plan.OperationCount; i > 0; i--)
            {
                NTSTATUS OneErr = S1RevertOperation(&Plan, &Plan.Operations[i - 1]);

                if (!NT_SUCCESS(OneErr) && NT_SUCCESS(RevertErr))
                {
                    RevertErr = OneErr;
                    FailedOp = i;
                }
            }
            if (!NT_SUCCESS(RevertErr))
            {
                WCHAR Line[112];

                swprintf(Line, RTL_NUMBER_OF(Line),
                         L"S1: revert op %lu failed 0x%08lx, halting",
                         (unsigned long)FailedOp, (unsigned long)RevertErr);
                S1Log(Line);
                S1JournalOrHalt(&Journal, Plan.TransactionId, "failed", -1,
                                "failed", "0xE0000001");
                NtTerminateProcess(NtCurrentProcess(), 1);
            }
        }
        S1JournalOrHalt(&Journal, Plan.TransactionId, "rolled-back", -1, "ok", NULL);
        Attempt.Attempt = 0;
        Attempt.FailureRecorded = S1_FALSE;
        snprintf(Attempt.LastResult, sizeof(Attempt.LastResult), "%s", "none");
        S1AttemptSave(&Attempt);
        /* The transaction is over: a consumed plan must not reapply on the
         * next boot. The engine closes a transaction by removing the plan
         * and the attempt state together. */
        {
            HANDLE PlanHandle;
            FILE_DISPOSITION_INFORMATION DeleteInfo;
            IO_STATUS_BLOCK IoStatus;
            static const struct
            {
                PCWSTR Path;
            } ToDelete[] =
            {
                { S1_PLAN_PATH },
                { S1_ATTEMPT_PATH },
            };
            size_t d;

            for (d = 0; d < sizeof(ToDelete) / sizeof(ToDelete[0]); d++)
            {
                if (NT_SUCCESS(S1NtOpen(ToDelete[d].Path, DELETE, FILE_OPEN, &PlanHandle)))
                {
                    DeleteInfo.DeleteFile = TRUE;
                    NtSetInformationFile(PlanHandle, &IoStatus, &DeleteInfo,
                                         sizeof(DeleteInfo), FileDispositionInformation);
                    NtClose(PlanHandle);
                }
            }
        }
        NtTerminateProcess(NtCurrentProcess(), 0);
    }

    /* 6. Apply the plan; journal each step before the next. */
    S1JournalOrHalt(&Journal, Plan.TransactionId, "applying", -1, "started", NULL);
    S1Log(L"S1: applying plan");
    for (i = 0; i < Plan.OperationCount; i++)
    {
        OpStatus = S1ApplyOperation(&Plan, &Plan.Operations[i]);
        if (!NT_SUCCESS(OpStatus))
        {
            WCHAR Line[96];

            swprintf(Line, RTL_NUMBER_OF(Line), L"S1: op %lu failed 0x%08lx, rolling back",
                     (unsigned long)i, (unsigned long)OpStatus);
            S1Log(Line);
            /* Immediate rollback of the operations already applied
             * (best-effort, Pam review R1/R2): keep reverting after a
             * failure so one bad backup cannot block the rest; the first
             * failure is journaled and exits 1 below. */
            {
                NTSTATUS RevertErr = STATUS_SUCCESS;
                ULONG FailedOp = 0;

                while (i > 0)
                {
                    NTSTATUS OneErr;

                    i--;
                    OneErr = S1RevertOperation(&Plan, &Plan.Operations[i]);
                    if (!NT_SUCCESS(OneErr) && NT_SUCCESS(RevertErr))
                    {
                        RevertErr = OneErr;
                        FailedOp = i + 1;
                    }
                }
                if (!NT_SUCCESS(RevertErr))
                {
                    WCHAR Line[112];

                    swprintf(Line, RTL_NUMBER_OF(Line),
                             L"S1: revert op %lu failed 0x%08lx during failure rollback",
                             (unsigned long)FailedOp, (unsigned long)RevertErr);
                    S1Log(Line);
                    S1JournalOrHalt(&Journal, Plan.TransactionId, "failed", -1,
                                    "failed", "0xE0000001");
                    NtTerminateProcess(NtCurrentProcess(), 1);
                }
            }
            S1JournalOrHalt(&Journal, Plan.TransactionId, "failed", -1,
                            "failed", "0xE0000001");
            Attempt.FailureRecorded = S1_TRUE;
            snprintf(Attempt.LastResult, sizeof(Attempt.LastResult), "%s", "failed");
            S1AttemptSave(&Attempt);
            NtTerminateProcess(NtCurrentProcess(), 0);
        }
        S1JournalOrHalt(&Journal, Plan.TransactionId, "applying",
                        (int)Plan.Operations[i].Seq, "ok", NULL);
    }

    /* 7. Applied, not confirmed: confirmation requires the service health
     * check after the boot. The next boot without it consumes the attempt
     * already written in step 4. */
    S1JournalOrHalt(&Journal, Plan.TransactionId, "applied", -1, "ok", NULL);
    S1Log(L"S1: applied");
    snprintf(Attempt.LastResult, sizeof(Attempt.LastResult), "%s", "applied");
    S1AttemptSave(&Attempt);
    (void)BackupDirProbe;

    S1Log(L"S1: done, exit 0");
    NtTerminateProcess(NtCurrentProcess(), 0);
}
