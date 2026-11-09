/*
 * PROJECT:     LiberNT Update S1 end-to-end harness
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Host shim that lets the real first-boot applier (pending.c) run on a fake target root
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#ifndef S1HOST_SHIM_H
#define S1HOST_SHIM_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>

typedef LONG NTSTATUS;
typedef ULONG ACCESS_MASK;

#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS               ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
#endif
#ifndef STATUS_OBJECT_PATH_NOT_FOUND
#define STATUS_OBJECT_PATH_NOT_FOUND ((NTSTATUS)0xC000003AL)
#endif
#ifndef STATUS_OBJECT_NAME_COLLISION
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035L)
#endif
#ifndef STATUS_ACCESS_DENIED
#define STATUS_ACCESS_DENIED         ((NTSTATUS)0xC0000022L)
#endif
#ifndef STATUS_NO_MEMORY
#define STATUS_NO_MEMORY             ((NTSTATUS)0xC0000017L)
#endif
#ifndef STATUS_END_OF_FILE
#define STATUS_END_OF_FILE           ((NTSTATUS)0xC0000011L)
#endif
#ifndef STATUS_BUFFER_OVERFLOW
#define STATUS_BUFFER_OVERFLOW       ((NTSTATUS)0x80000005L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER     ((NTSTATUS)0xC000000DL)
#endif
#ifndef STATUS_INVALID_SIGNATURE
#define STATUS_INVALID_SIGNATURE     ((NTSTATUS)0xC000A000L)
#endif
#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH  ((NTSTATUS)0xC0000004L)
#endif
#ifndef STATUS_SHARING_VIOLATION
#define STATUS_SHARING_VIOLATION     ((NTSTATUS)0xC0000043L)
#endif
#ifndef STATUS_LOCK_NOT_GRANTED
#define STATUS_LOCK_NOT_GRANTED      ((NTSTATUS)0xC0000055L)
#endif

#ifndef FILE_READ_DATA
#define FILE_READ_DATA        0x0001
#define FILE_WRITE_DATA       0x0002
#endif
#ifndef SYNCHRONIZE
#define SYNCHRONIZE           0x00100000L
#endif
#ifndef DELETE
#define DELETE                0x00010000L
#endif
#ifndef FILE_SHARE_READ
#define FILE_SHARE_READ       1
#endif
#ifndef FILE_ATTRIBUTE_NORMAL
#define FILE_ATTRIBUTE_NORMAL 0x80
#endif
#ifndef FILE_OPEN
#define FILE_OPEN             1
#define FILE_CREATE           2
#define FILE_OPEN_IF          3
#endif
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#define FILE_NON_DIRECTORY_FILE      0x00000040

#define FileStandardInformation    5
#define FilePositionInformation    14
#define FileRenameInformation      10
#define FileEndOfFileInformation   20
#define FileDispositionInformation 13

typedef struct _S1_UNICODE_STRING
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} S1_UNICODE_STRING;

typedef S1_UNICODE_STRING UNICODE_STRING, *PUNICODE_STRING;
typedef const UNICODE_STRING *PCUNICODE_STRING;
typedef PUNICODE_STRING PCWSTR_NATIVE;

typedef struct _S1_OBJECT_ATTRIBUTES
{
    ULONG Length;
    HANDLE RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} S1_OBJECT_ATTRIBUTES;

typedef S1_OBJECT_ATTRIBUTES OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

typedef struct _S1_IO_STATUS_BLOCK
{
    union
    {
        NTSTATUS Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
} S1_IO_STATUS_BLOCK;

typedef S1_IO_STATUS_BLOCK IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef struct _S1_FILE_STANDARD_INFORMATION
{
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG NumberOfLinks;
    BOOLEAN DeletePending;
    BOOLEAN Directory;
} S1_FILE_STANDARD_INFORMATION;

typedef S1_FILE_STANDARD_INFORMATION FILE_STANDARD_INFORMATION, *PFILE_STANDARD_INFORMATION;

typedef struct _S1_FILE_POSITION_INFORMATION
{
    LARGE_INTEGER CurrentByteOffset;
} S1_FILE_POSITION_INFORMATION;

typedef S1_FILE_POSITION_INFORMATION FILE_POSITION_INFORMATION, *PFILE_POSITION_INFORMATION;

typedef struct _S1_FILE_END_OF_FILE_INFORMATION
{
    LARGE_INTEGER EndOfFile;
} S1_FILE_END_OF_FILE_INFORMATION;

typedef S1_FILE_END_OF_FILE_INFORMATION FILE_END_OF_FILE_INFORMATION, *PFILE_END_OF_FILE_INFORMATION;

typedef struct _S1_FILE_DISPOSITION_INFORMATION
{
    BOOLEAN DeleteFile;
} S1_FILE_DISPOSITION_INFORMATION;

typedef S1_FILE_DISPOSITION_INFORMATION FILE_DISPOSITION_INFORMATION, *PFILE_DISPOSITION_INFORMATION;

typedef struct _S1_FILE_RENAME_INFORMATION
{
    BOOLEAN ReplaceIfExists;
    HANDLE RootDirectory;
    ULONG FileNameLength;
    WCHAR FileName[1];
} S1_FILE_RENAME_INFORMATION;

typedef S1_FILE_RENAME_INFORMATION FILE_RENAME_INFORMATION, *PFILE_RENAME_INFORMATION;

#define OBJ_CASE_INSENSITIVE 0x40
#ifndef NTAPI
#define NTAPI __stdcall
#endif
#ifndef VOID
#define VOID void
#endif

#define InitializeObjectAttributes(p, n, a, r, s) \
    do { \
        (p)->Length = sizeof(OBJECT_ATTRIBUTES); \
        (p)->RootDirectory = (r); \
        (p)->ObjectName = (n); \
        (p)->Attributes = (a); \
        (p)->SecurityDescriptor = (s); \
        (p)->SecurityQualityOfService = NULL; \
    } while (0)

typedef struct _S1_RTL_USER_PROCESS_PARAMETERS
{
    ULONG Length;
    ULONG MaximumLength;
} S1_RTL_USER_PROCESS_PARAMETERS;

typedef S1_RTL_USER_PROCESS_PARAMETERS RTL_USER_PROCESS_PARAMETERS,
    *PRTL_USER_PROCESS_PARAMETERS;

/* The fake target root: \Windows\... in the applier maps under here. */
extern WCHAR g_FakeRoot[MAX_PATH];
extern WCHAR g_ServicingRoot[MAX_PATH];
/* The volume root the op targets are composed against: the real volume
 * resolved from \SystemRoot at boot, the fake root under the shim. Both
 * builds of the applier prefix it, so the host E2E composes target paths
 * exactly like the guest. */
extern WCHAR g_VolumeRoot[MAX_PATH * 2];

#define S1_ROOT         g_ServicingRoot
#define S1_PLAN_PATH    g_S1PlanPath
#define S1_ATTEMPT_PATH g_S1AttemptPath
#define S1_JOURNAL_PATH g_S1JournalPath
#define S1_BACKUP_PATH  g_S1BackupPath

extern WCHAR g_S1PlanPath[MAX_PATH * 2];
extern WCHAR g_S1AttemptPath[MAX_PATH * 2];
extern WCHAR g_S1JournalPath[MAX_PATH * 2];
extern WCHAR g_S1BackupPath[MAX_PATH * 2];

void ShimSetFakeRoot(const wchar_t *Root);

static __inline void RtlInitUnicodeString(PUNICODE_STRING Dst, PCWSTR Src)
{
    size_t Len = wcslen(Src);

    Dst->Buffer = (PWSTR)Src;
    Dst->Length = (USHORT)(Len * sizeof(WCHAR));
    Dst->MaximumLength = Dst->Length + sizeof(WCHAR);
}

static __inline PVOID RtlGetProcessHeap(VOID)
{
    return (PVOID)(ULONG_PTR)1;
}

static __inline PVOID RtlAllocateHeap(PVOID Heap, ULONG Flags, SIZE_T Size)
{
    (void)Heap;
    (void)Flags;
    return malloc(Size);
}

static __inline BOOL RtlFreeHeap(PVOID Heap, ULONG Flags, PVOID Mem)
{
    (void)Heap;
    (void)Flags;
    free(Mem);
    return TRUE;
}

static __inline HANDLE NtCurrentProcess(VOID)
{
    return (HANDLE)(LONG_PTR)-1;
}

/* Map an applier path (\Windows\... or an absolute host path) to the host
 * file, and prove the normalized result stays under the fake root: a
 * "\Windows\..\..\X" must never escape (Pam review E3). */
static __inline NTSTATUS ShimMapPath(PCWSTR Name, WCHAR Out[MAX_PATH * 3])
{
    WCHAR Tmp[MAX_PATH * 3];
    WCHAR Full[MAX_PATH * 3];
    DWORD Len;
    size_t RootLen = wcslen(g_FakeRoot);

    if (Name[0] == L'\\' || Name[0] == L'/')
    {
        if (Name[1] == L'?' || Name[1] == L'.') /* \??\ and \.\ are not target paths */
            return STATUS_ACCESS_DENIED;
        swprintf(Tmp, MAX_PATH * 3, L"%s%ls", g_FakeRoot, Name);
    }
    else if (Name[0] && Name[1] == L':') /* drive-absolute: only inside the fake root */
    {
        if (_wcsnicmp(Name, g_FakeRoot, (DWORD)RootLen) != 0 || Name[RootLen] != L'\\')
            return STATUS_ACCESS_DENIED;
        wcscpy(Tmp, Name);
    }
    else if (wcsncmp(Name, g_FakeRoot, RootLen) == 0 && Name[RootLen] == L'\\')
    {
        wcscpy(Tmp, Name);
    }
    else
    {
        swprintf(Tmp, MAX_PATH * 3, L"%s\\%ls", g_ServicingRoot, Name);
    }

    Len = GetFullPathNameW(Tmp, MAX_PATH * 3, Full, NULL);
    if (Len == 0 || Len >= MAX_PATH * 3)
        return STATUS_ACCESS_DENIED;
    if (_wcsnicmp(Full, g_FakeRoot, (DWORD)RootLen) != 0 ||
        (Full[RootLen] != L'\\' && Full[RootLen] != 0))
        return STATUS_ACCESS_DENIED;
    wcscpy(Out, Full);
    return STATUS_SUCCESS;
}

static int g_TraceEnabled = -1;
static FILE *g_TraceFile = NULL;

static __inline void ShimTrace(LPCWSTR Fmt, ...)
{
    va_list Ap;
    WCHAR Buf[1024];

    if (g_TraceEnabled < 0)
    {
        WCHAR Path[MAX_PATH];

        g_TraceEnabled = 0;
        if (GetEnvironmentVariableW(L"S1_SHIM_LOG", Path, MAX_PATH) > 0)
        {
            g_TraceFile = _wfopen(Path, L"a, ccs=UTF-8");
            if (g_TraceFile)
                g_TraceEnabled = 1;
        }
    }
    if (!g_TraceEnabled)
        return;
    va_start(Ap, Fmt);
    _vsnwprintf(Buf, 1024, Fmt, Ap);
    va_end(Ap);
    Buf[1023] = 0;
    fwprintf(g_TraceFile, L"%s", Buf);
    fflush(g_TraceFile);
}

#define SHIM_TRACE(...) ShimTrace(__VA_ARGS__)

static __inline NTSTATUS ShimWin32Error(VOID)
{
    DWORD e = GetLastError();

    switch (e)
    {
        case ERROR_FILE_NOT_FOUND:
            return STATUS_OBJECT_NAME_NOT_FOUND;
        case ERROR_PATH_NOT_FOUND:
            return STATUS_OBJECT_PATH_NOT_FOUND;
        case ERROR_ACCESS_DENIED:
            return STATUS_ACCESS_DENIED;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return STATUS_OBJECT_NAME_COLLISION;
        case ERROR_SHARING_VIOLATION:
            return STATUS_SHARING_VIOLATION;
        case ERROR_LOCK_VIOLATION:
            return STATUS_LOCK_NOT_GRANTED;
        default:
            return (NTSTATUS)(0xC0000000L | e);
    }
}

/* B1 review M4 test hook: count every operation that can modify the fake volume (a write/delete open, a
 * create disposition, a write, a rename/delete/position change that mutates). host_applier.c reports the count
 * at the moment the applier asks whether the volume is dirty: it must be 0. */
static ULONG g_ShimMutations = 0;

static __inline NTSTATUS NTAPI NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
    PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
    ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength)
{
    DWORD Access = 0;
    DWORD Disposition;
    WCHAR Path[MAX_PATH * 3];

    (void)AllocationSize;
    (void)EaBuffer;
    (void)EaLength;
    if ((DesiredAccess & (FILE_WRITE_DATA | DELETE)) || CreateDisposition != FILE_OPEN)
        g_ShimMutations++;

    if (DesiredAccess & FILE_READ_DATA)
        Access |= GENERIC_READ;
    if (DesiredAccess & FILE_WRITE_DATA)
        Access |= GENERIC_WRITE;
    if (DesiredAccess & DELETE)
        Access |= DELETE;
    if ((DesiredAccess & (FILE_READ_DATA | FILE_WRITE_DATA)) == 0 && (DesiredAccess & DELETE))
        Access |= GENERIC_READ;

    switch (CreateDisposition)
    {
        case FILE_OPEN:
            Disposition = OPEN_EXISTING;
            break;
        case FILE_CREATE:
            Disposition = CREATE_NEW;
            break;
        case FILE_OPEN_IF:
            Disposition = OPEN_ALWAYS;
            break;
        default:
            return STATUS_INVALID_PARAMETER;
    }

    {
        NTSTATUS MapStatus = ShimMapPath(ObjectAttributes->ObjectName->Buffer, Path);
        if (!NT_SUCCESS(MapStatus))
            return MapStatus;
    }
    *FileHandle = CreateFileW(Path, Access, ShareAccess, NULL, Disposition,
                              FileAttributes, NULL);
    if (*FileHandle == INVALID_HANDLE_VALUE)
    {
        NTSTATUS st;
        *FileHandle = NULL;
        st = ShimWin32Error();
        SHIM_TRACE(L"create '%s' disp=%lu failed: 0x%08lx\n", Path, CreateDisposition, st);
        IoStatusBlock->Status = st;
        return st;
    }
    SHIM_TRACE(L"create '%s' disp=%lu access=0x%lx ok\n", Path, CreateDisposition, DesiredAccess);
    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = 0;
    return STATUS_SUCCESS;
}

static __inline NTSTATUS NTAPI NtReadFile(
    HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset, PVOID Key)
{
    DWORD Read = 0;

    (void)Event;
    (void)ApcRoutine;
    (void)ApcContext;
    (void)ByteOffset;
    (void)Key;

    if (!ReadFile(FileHandle, Buffer, Length, &Read, NULL))
    {
        IoStatusBlock->Status = ShimWin32Error();
        return IoStatusBlock->Status;
    }
    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = Read;
    return STATUS_SUCCESS;
}

static __inline NTSTATUS NTAPI NtWriteFile(
    HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset, PVOID Key)
{
    DWORD Written = 0;

    (void)Event;
    (void)ApcRoutine;
    (void)ApcContext;
    (void)ByteOffset;
    (void)Key;

    if (!WriteFile(FileHandle, Buffer, Length, &Written, NULL))
    {
        IoStatusBlock->Status = ShimWin32Error();
        return IoStatusBlock->Status;
    }
    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = Written;
    return STATUS_SUCCESS;
}

static __inline NTSTATUS NTAPI NtFlushBuffersFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock)
{
    if (!FlushFileBuffers(FileHandle))
    {
        IoStatusBlock->Status = ShimWin32Error();
        return IoStatusBlock->Status;
    }
    IoStatusBlock->Status = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

static __inline NTSTATUS NTAPI NtDelayExecution(BOOLEAN Alertable, PLARGE_INTEGER DelayInterval)
{
    (void)Alertable;

    if (DelayInterval->QuadPart >= 0)
        return STATUS_SUCCESS;
    Sleep((DWORD)((-DelayInterval->QuadPart) / 10000));
    return STATUS_SUCCESS;
}

static __inline NTSTATUS NTAPI NtQueryInformationFile(
    HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
    ULONG Length, ULONG FileInformationClass)
{
    (void)Length;

    if (FileInformationClass == FileStandardInformation)
    {
        FILE_STANDARD_INFORMATION *Info = (FILE_STANDARD_INFORMATION *)FileInformation;
        LARGE_INTEGER Size;

        if (!GetFileSizeEx(FileHandle, &Size))
        {
            IoStatusBlock->Status = ShimWin32Error();
            return IoStatusBlock->Status;
        }
        Info->AllocationSize.QuadPart = Size.QuadPart;
        Info->EndOfFile.QuadPart = Size.QuadPart;
        Info->NumberOfLinks = 1;
        Info->DeletePending = FALSE;
        Info->Directory = FALSE;
        IoStatusBlock->Status = STATUS_SUCCESS;
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

static __inline NTSTATUS ShimRenameByHandle(
    HANDLE FileHandle, PCWSTR NewName, BOOLEAN ReplaceIfExists)
{
    WCHAR Current[MAX_PATH * 3];
    WCHAR Mapped[MAX_PATH * 3];
    DWORD Flags = ReplaceIfExists ? MOVEFILE_REPLACE_EXISTING : 0;
    DWORD Len = GetFinalPathNameByHandleW(FileHandle, Current, MAX_PATH * 3,
                                          VOLUME_NAME_DOS | FILE_NAME_NORMALIZED);

    if (Len == 0 || Len >= MAX_PATH * 3)
        return STATUS_ACCESS_DENIED;
    {
        NTSTATUS MapStatus = ShimMapPath(NewName, Mapped);
        if (!NT_SUCCESS(MapStatus))
            return MapStatus;
    }
    SHIM_TRACE(L"rename '%s' -> '%s'%s\n", Current, Mapped,
               ReplaceIfExists ? L" (replace)" : L"");
    if (!MoveFileExW(Current, Mapped, Flags))
    {
        NTSTATUS st = ShimWin32Error();
        SHIM_TRACE(L"  rename failed: 0x%08lx\n", st);
        return st;
    }
    return STATUS_SUCCESS;
}

static __inline NTSTATUS NTAPI NtSetInformationFile(
    HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
    ULONG Length, ULONG FileInformationClass)
{
    if (FileInformationClass == FileRenameInformation || FileInformationClass == FileDispositionInformation)
        g_ShimMutations++;
    if (FileInformationClass == FileRenameInformation)
    {
        FILE_RENAME_INFORMATION *Info = (FILE_RENAME_INFORMATION *)FileInformation;
        NTSTATUS Status;
        WCHAR Target[MAX_PATH * 3];

        /* Strict, kernel-shaped validation: the legacy branch that accepted
         * a bare UNICODE_STRING here masked a real bug (the engine passed
         * one for years; LiberNT failed it closed in the VM). A malformed
         * rename info must fail like the kernel, not be guessed. */
        if (Length < (ULONG)(FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName)) ||
            Info->FileNameLength == 0 || (Info->FileNameLength & 1) ||
            Length - FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) < Info->FileNameLength)
        {
            IoStatusBlock->Status = STATUS_INFO_LENGTH_MISMATCH;
            return IoStatusBlock->Status;
        }
        if (Info->RootDirectory != NULL)
            return STATUS_INVALID_PARAMETER;
        if ((size_t)Info->FileNameLength + sizeof(WCHAR) > sizeof(Target))
            return STATUS_ACCESS_DENIED;
        memcpy(Target, Info->FileName, Info->FileNameLength);
        Target[Info->FileNameLength / sizeof(WCHAR)] = 0;
        Status = ShimRenameByHandle(FileHandle, Target, Info->ReplaceIfExists);
        IoStatusBlock->Status = Status;
        return Status;
    }
    if (FileInformationClass == FilePositionInformation)
    {
        FILE_POSITION_INFORMATION *Info = (FILE_POSITION_INFORMATION *)FileInformation;
        LARGE_INTEGER Distance;

        Distance.QuadPart = Info->CurrentByteOffset.QuadPart;
        if (SetFilePointerEx(FileHandle, Distance, NULL, FILE_BEGIN) == 0)
        {
            IoStatusBlock->Status = ShimWin32Error();
            return IoStatusBlock->Status;
        }
        IoStatusBlock->Status = STATUS_SUCCESS;
        return STATUS_SUCCESS;
    }
    if (FileInformationClass == FileEndOfFileInformation)
    {
        FILE_END_OF_FILE_INFORMATION *Info = (FILE_END_OF_FILE_INFORMATION *)FileInformation;

        /* the boot writer truncates to zero (position already set to 0) */
        if (Info->EndOfFile.QuadPart != 0)
            return STATUS_INVALID_PARAMETER;
        if (SetEndOfFile(FileHandle) == 0)
        {
            IoStatusBlock->Status = ShimWin32Error();
            return IoStatusBlock->Status;
        }
        IoStatusBlock->Status = STATUS_SUCCESS;
        return STATUS_SUCCESS;
    }
    if (FileInformationClass == FileDispositionInformation)
    {
        FILE_DISPOSITION_INFORMATION *Info = (FILE_DISPOSITION_INFORMATION *)FileInformation;
        FILE_DISPOSITION_INFO Win32Info;

        Win32Info.DeleteFile = Info->DeleteFile;
        SHIM_TRACE(L"set-disposition delete=%u\n", Info->DeleteFile);
        if (!SetFileInformationByHandle(FileHandle, FileDispositionInfo, &Win32Info,
                                        sizeof(Win32Info)))
        {
            IoStatusBlock->Status = ShimWin32Error();
            return IoStatusBlock->Status;
        }
        IoStatusBlock->Status = STATUS_SUCCESS;
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

static __inline NTSTATUS NTAPI NtClose(HANDLE Handle)
{
    CloseHandle(Handle);
    return STATUS_SUCCESS;
}

static __inline NTSTATUS NTAPI NtTerminateProcess(HANDLE Process, NTSTATUS ExitStatus)
{
    (void)Process;
    exit((int)ExitStatus);
}

#endif /* S1HOST_SHIM_H */
