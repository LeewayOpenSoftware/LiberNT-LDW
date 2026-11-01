/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     libernt-update-bootstrap: install/uninstall the LiberNT Update stage-1 infrastructure
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define INITGUID
#include "bootstrap.h"
#include <time.h>
#include <objbase.h>
#include <shlobj.h>

static BS_LOG BsLogState;

VOID
BsPrint(
    _In_ PCWSTR Format,
    ...)
{
    WCHAR Buffer[2048];
    va_list Args;
    HANDLE Out;
    DWORD Written;

    va_start(Args, Format);
    vswprintf(Buffer, RTL_NUMBER_OF(Buffer), Format, Args);
    va_end(Args);
    Out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!WriteConsoleW(Out, Buffer, (DWORD)wcslen(Buffer), &Written, NULL))
        WriteFile(Out, Buffer, (DWORD)wcslen(Buffer) * sizeof(WCHAR), &Written, NULL);
}

VOID
BsLog(
    _Inout_ BS_LOG *Log,
    _In_ PCWSTR Format,
    ...)
{
    WCHAR Buffer[2048];
    va_list Args;
    HANDLE Handle;
    DWORD Written;

    va_start(Args, Format);
    vswprintf(Buffer, RTL_NUMBER_OF(Buffer), Format, Args);
    va_end(Args);

    BsPrint(L"%s", Buffer);
    if (!Log->Ready)
        return;
    Handle = CreateFileW(Log->LogPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                         NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return;
    WriteFile(Handle, Buffer, (DWORD)wcslen(Buffer) * sizeof(WCHAR), &Written, NULL);
    CloseHandle(Handle);
}

VOID
BsLogInit(
    _Inout_ BS_LOG *Log,
    _In_ BS_TARGET *Target)
{
    memset(Log, 0, sizeof(*Log));
    Log->Target = Target;
    if (BsBuildTargetPath(Log->LogPath, RTL_NUMBER_OF(Log->LogPath),
                          Target, L"LiberNT\\Servicing\\logs\\bootstrap.log") == ERROR_SUCCESS)
        Log->Ready = TRUE;
}

static VOID
BsFormatError(
    _In_ DWORD Error,
    _Out_ WCHAR *Buffer,
    _In_ size_t Cch)
{
    if (!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, Error, 0,
                        Buffer, (DWORD)Cch, NULL))
        swprintf(Buffer, Cch, L"error 0x%08lx", (unsigned long)Error);
}

/* ---------------------------------------------------------------- inventory */

static DWORD
BsJsonEscapeNarrow(
    _Out_ char *Dst,
    _In_ size_t DstCap,
    _In_ const char *Src)
{
    size_t Used = 0;

    while (*Src)
    {
        unsigned char C = (unsigned char)*Src++;
        const char *Esc = NULL;
        char Tmp[8];

        switch (C)
        {
            case '"': Esc = "\\\""; break;
            case '\\': Esc = "\\\\"; break;
            default:
                if (C < 0x20)
                {
                    snprintf(Tmp, sizeof(Tmp), "\\u%04x", C);
                    Esc = Tmp;
                }
                break;
        }
        if (Esc)
        {
            size_t Len = strlen(Esc);

            if (Used + Len >= DstCap) return ERROR_BUFFER_OVERFLOW;
            memcpy(Dst + Used, Esc, Len);
            Used += Len;
        }
        else
        {
            if (Used + 1 >= DstCap) return ERROR_BUFFER_OVERFLOW;
            Dst[Used++] = (char)C;
        }
    }
    Dst[Used] = 0;
    return ERROR_SUCCESS;
}

DWORD
BsInventorySave(
    _In_ const BS_TARGET *Target,
    _In_ const BS_INVENTORY *Inventory)
{
    WCHAR Path[BS_MAX_PATH_BUF], Temp[BS_MAX_PATH_BUF];
    char Json[8192], *Cursor;
    char VersionNarrow[32], ServiceNarrow[32];
    char VersionEsc[64], ServiceEsc[64], ManifestEsc[140];
    char NarrowPath[BS_MAX_PATH_BUF], EscapedPath[BS_MAX_PATH_BUF * 2];
    HANDLE Handle;
    DWORD Written, Error;
    ULONG i;

    if (BsBuildTargetPath(Path, RTL_NUMBER_OF(Path), Target, BS_INSTALLED_JSON) != ERROR_SUCCESS)
        return ERROR_FILENAME_EXCED_RANGE;
    if (swprintf(Temp, RTL_NUMBER_OF(Temp), L"%s.tmp", Path) < 0)
        return ERROR_FILENAME_EXCED_RANGE;

    /* Narrow UTF-8, exactly what BsInventoryLoad parses back, written
     * atomically through a temp file (Pam review note). Every string is
     * JSON-escaped: recorded paths contain backslashes the parser rejects
     * as invalid escapes when they are written raw. */
    if (wcstombs(VersionNarrow, BS_VERSION, sizeof(VersionNarrow)) == (size_t)-1 ||
        wcstombs(ServiceNarrow, Inventory->ServiceState, sizeof(ServiceNarrow)) == (size_t)-1)
        return ERROR_NO_UNICODE_TRANSLATION;
    VersionNarrow[sizeof(VersionNarrow) - 1] = 0;
    ServiceNarrow[sizeof(ServiceNarrow) - 1] = 0;
    Error = BsJsonEscapeNarrow(VersionEsc, sizeof(VersionEsc), VersionNarrow);
    if (Error == ERROR_SUCCESS)
        Error = BsJsonEscapeNarrow(ServiceEsc, sizeof(ServiceEsc), ServiceNarrow);
    if (Error == ERROR_SUCCESS)
        Error = BsJsonEscapeNarrow(ManifestEsc, sizeof(ManifestEsc), Inventory->ManifestDigest);
    if (Error != ERROR_SUCCESS)
        return Error;

    Cursor = Json;
#define BS_EMIT(...) do { \
        int N_ = snprintf(Cursor, sizeof(Json) - (size_t)(Cursor - Json), __VA_ARGS__); \
        if (N_ < 0 || (size_t)N_ >= sizeof(Json) - (size_t)(Cursor - Json)) return ERROR_BUFFER_OVERFLOW; \
        Cursor += N_; \
    } while (0)
    BS_EMIT("{\"schema\":1,\"version\":\"%s\",\"service\":\"%s\",\"bootExecute\":%s,\"manifest\":\"%s\",\"files\":[",
            VersionEsc, ServiceEsc,
            Inventory->BootExecute ? "true" : "false",
            ManifestEsc);
    for (i = 0; i < Inventory->FileCount; i++)
    {
        if (wcstombs(NarrowPath, Inventory->Files[i].Path, sizeof(NarrowPath)) == (size_t)-1)
            return ERROR_NO_UNICODE_TRANSLATION;
        NarrowPath[sizeof(NarrowPath) - 1] = 0;
        Error = BsJsonEscapeNarrow(EscapedPath, sizeof(EscapedPath), NarrowPath);
        if (Error != ERROR_SUCCESS)
            return Error;
        BS_EMIT("%s{\"path\":\"%s\",\"sha256\":\"%s\"}",
                i ? "," : "", EscapedPath, Inventory->Files[i].Sha256);
    }
    BS_EMIT("]}");
#undef BS_EMIT

    Handle = CreateFileW(Temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!WriteFile(Handle, Json, (DWORD)strlen(Json), &Written, NULL) ||
        Written != (DWORD)strlen(Json))
    {
        DWORD Error = GetLastError();
        CloseHandle(Handle);
        DeleteFileW(Temp);
        return Error ? Error : ERROR_WRITE_FAULT;
    }
    CloseHandle(Handle);
    if (!MoveFileExW(Temp, Path, MOVEFILE_REPLACE_EXISTING))
    {
        DWORD Error = GetLastError();
        DeleteFileW(Temp);
        return Error;
    }
    return ERROR_SUCCESS;
}

DWORD
BsInventoryLoad(
    _In_ const BS_TARGET *Target,
    _Out_ BS_INVENTORY *Inventory)
{
    WCHAR Path[BS_MAX_PATH_BUF];
    HANDLE Handle;
    LARGE_INTEGER Size;
    char *Json;
    S1_JSON *Root;
    DWORD Read, i, Count;

    memset(Inventory, 0, sizeof(*Inventory));
    if (BsBuildTargetPath(Path, RTL_NUMBER_OF(Path), Target, BS_INSTALLED_JSON) != ERROR_SUCCESS)
        return ERROR_FILENAME_EXCED_RANGE;
    Handle = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!GetFileSizeEx(Handle, &Size) || Size.QuadPart <= 0 || Size.QuadPart > 65536)
    {
        CloseHandle(Handle);
        return ERROR_FILE_INVALID;
    }
    Json = malloc((size_t)Size.QuadPart);
    if (!Json)
    {
        CloseHandle(Handle);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    if (!ReadFile(Handle, Json, (DWORD)Size.QuadPart, &Read, NULL) ||
        Read != (DWORD)Size.QuadPart)
    {
        free(Json);
        CloseHandle(Handle);
        return ERROR_READ_FAULT;
    }
    CloseHandle(Handle);

    if (!S1JsonParse(Json, Read, &Root))
    {
        free(Json);
        return ERROR_BAD_FORMAT;
    }
    {
        const char *Service;
        size_t ServiceLength;
        char Narrow[16];
        S1Bool Boot;

        if (S1JsonGetString(Root, "service", &Service, &ServiceLength) &&
            ServiceLength < sizeof(Narrow))
        {
            memcpy(Narrow, Service, ServiceLength);
            Narrow[ServiceLength] = 0;
            mbstowcs(Inventory->ServiceState, Narrow, RTL_NUMBER_OF(Inventory->ServiceState));
        }
        if (S1JsonGetBool(Root, "bootExecute", &Boot) == S1_TRUE)
            Inventory->BootExecute = Boot == S1_TRUE;
        {
            const char *Digest;
            size_t DigestLength;

            if (S1JsonGetString(Root, "manifest", &Digest, &DigestLength) &&
                DigestLength == 64)
            {
                memcpy(Inventory->ManifestDigest, Digest, 64);
                Inventory->ManifestDigest[64] = 0;
            }
        }
    }
    Count = (DWORD)S1JsonGetArrayCount(Root, "files");
    if (Count > RTL_NUMBER_OF(Inventory->Files))
        Count = RTL_NUMBER_OF(Inventory->Files);
    for (i = 0; i < Count; i++)
    {
        char Key[64];
        const char *Value;
        size_t ValueLength;
        BS_FILE_REC *Rec = &Inventory->Files[i];

        snprintf(Key, sizeof(Key), "files.%lu.path", (unsigned long)i);
        if (!S1JsonGetString(Root, Key, &Value, &ValueLength) ||
            ValueLength + 1 > sizeof(Rec->Path))
            break;
        {
            char NarrowPath[BS_MAX_PATH_BUF];

            memcpy(NarrowPath, Value, ValueLength);
            NarrowPath[ValueLength] = 0;
            mbstowcs(Rec->Path, NarrowPath, RTL_NUMBER_OF(Rec->Path));
        }
        snprintf(Key, sizeof(Key), "files.%lu.sha256", (unsigned long)i);
        if (!S1JsonGetString(Root, Key, &Value, &ValueLength) ||
            ValueLength != 64)
            break;
        memcpy(Rec->Sha256, Value, 64);
        Rec->Sha256[64] = 0;
        Inventory->FileCount++;
    }
    S1JsonFree(Root);
    free(Json);
    return Inventory->FileCount > 0 ? ERROR_SUCCESS : ERROR_BAD_FORMAT;
}

/* ------------------------------------------------------------------ install */

/* The all-users Start Menu Programs folder, relative to the target root.
 * Matches FOLDERID_CommonPrograms on this tree (shellpath.c: ProgramData
 * parent, Microsoft\Windows\Start Menu\Programs). */
#define BS_PROGRAMS_DIR L"ProgramData\\Microsoft\\Windows\\Start Menu\\Programs"

static DWORD
BsEnsureDirChain(
    _In_ PCWSTR Path)
{
    WCHAR Copy[BS_MAX_PATH_BUF];
    WCHAR *Cursor;

    if (wcslen(Path) >= RTL_NUMBER_OF(Copy))
        return ERROR_FILENAME_EXCED_RANGE;
    wcscpy(Copy, Path);
    for (Cursor = Copy + 3; *Cursor; Cursor++)
    {
        if (*Cursor == L'\\')
        {
            *Cursor = 0;
            if (!CreateDirectoryW(Copy, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                return GetLastError();
            *Cursor = L'\\';
        }
    }
    if (!CreateDirectoryW(Copy, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return GetLastError();
    return ERROR_SUCCESS;
}

/* Materialize the manifest's Start Menu shortcuts and record them in the
 * inventory like any other installed file, so uninstall deletes exactly
 * the paths install created. */
static DWORD
BsInstallShortcuts(
    _In_ const BS_TARGET *Target,
    _In_ const BS_BUNDLE *Bundle,
    _Inout_ BS_INVENTORY *Inventory)
{
    ULONG s;

    for (s = 0; s < Bundle->ShortcutCount; s++)
    {
        const BS_BUNDLE_SHORTCUT *Shortcut = &Bundle->Shortcuts[s];
        WCHAR ProgramsDir[BS_MAX_PATH_BUF], LinkPath[BS_MAX_PATH_BUF];
        WCHAR TargetPath[BS_MAX_PATH_BUF];
        IShellLinkW *Link = NULL;
        IPersistFile *Persist = NULL;
        DWORD Error;

        if (Inventory->FileCount >= RTL_NUMBER_OF(Inventory->Files))
            return ERROR_BUFFER_OVERFLOW;

        if (swprintf(ProgramsDir, RTL_NUMBER_OF(ProgramsDir), L"%s\\%s",
                     Target->Root, BS_PROGRAMS_DIR) < 0 ||
            swprintf(LinkPath, RTL_NUMBER_OF(LinkPath), L"%s\\%s",
                     ProgramsDir, Shortcut->Name) < 0)
            return ERROR_FILENAME_EXCED_RANGE;
        if (swprintf(TargetPath, RTL_NUMBER_OF(TargetPath), L"%s\\%s",
                     Target->Root, Shortcut->Target) < 0)
            return ERROR_FILENAME_EXCED_RANGE;
        if (BsValidateTargetRelPath(Target, Shortcut->Target) != ERROR_SUCCESS)
            return ERROR_BAD_PATHNAME;

        Error = BsEnsureDirChain(ProgramsDir);
        if (Error != ERROR_SUCCESS)
            return Error;

        if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                    &IID_IShellLinkW, (void **)&Link)))
            return ERROR_NOINTERFACE;
        if (FAILED(Link->lpVtbl->QueryInterface(Link, &IID_IPersistFile,
                                                (void **)&Persist)))
        {
            Link->lpVtbl->Release(Link);
            return ERROR_NOINTERFACE;
        }
        Link->lpVtbl->SetPath(Link, TargetPath);
        if (Shortcut->Arguments[0])
            Link->lpVtbl->SetArguments(Link, Shortcut->Arguments);
        if (FAILED(Persist->lpVtbl->Save(Persist, LinkPath, TRUE)))
        {
            Persist->lpVtbl->Release(Persist);
            Link->lpVtbl->Release(Link);
            return ERROR_WRITE_FAULT;
        }
        Persist->lpVtbl->Release(Persist);
        Link->lpVtbl->Release(Link);

        wcscpy(Inventory->Files[Inventory->FileCount].Path, BS_PROGRAMS_DIR);
        wcscat(Inventory->Files[Inventory->FileCount].Path, L"\\");
        wcscat(Inventory->Files[Inventory->FileCount].Path, Shortcut->Name);
        Error = BsHashTargetFile(Target, Inventory->Files[Inventory->FileCount].Path,
                                 Inventory->Files[Inventory->FileCount].Sha256);
        if (Error != ERROR_SUCCESS)
            return Error;
        Inventory->FileCount++;
        BsPrint(L"  shortcut %ls -> %ls\n", LinkPath, TargetPath);
    }
    return ERROR_SUCCESS;
}

static BOOL
BsHasServiceBinary(
    _In_ const BS_BUNDLE *Bundle)
{
    ULONG i;

    for (i = 0; i < Bundle->FileCount; i++)
    {
        if (Bundle->Files[i].IsService)
            return TRUE;
    }
    return FALSE;
}

/* The service SCM entry points at a fixed binary path; a bundle that marks
 * a service must actually deliver that binary there (Pam review B6). */
static BOOL
BsHasServiceAtPath(
    _In_ const BS_BUNDLE *Bundle,
    _In_ PCWSTR Dest)
{
    ULONG i;

    for (i = 0; i < Bundle->FileCount; i++)
    {
        if (Bundle->Files[i].IsService &&
            wcscmp(Bundle->Files[i].Dest, Dest) == 0)
            return TRUE;
    }
    return FALSE;
}

static DWORD
BsInstall(
    _In_ BS_TARGET *Target,
    _In_ PCWSTR BundlePath,
    _In_ BOOL DryRun)
{
    BS_BUNDLE Bundle;
    BS_INVENTORY Inventory;
    WCHAR Source[BS_MAX_PATH_BUF], Dest[BS_MAX_PATH_BUF];
    ULONG64 FreeBytes;
    BOOL ServiceWanted, ServiceInstalled;
    DWORD Error;
    ULONG i;

    Error = BsBundleLoad(&Bundle, BundlePath);
    if (Error != ERROR_SUCCESS)
    {
        BsPrint(L"bundle: cannot load or verify manifest (0x%08lx)%s\n",
                (unsigned long)Error,
                Error == ERROR_TRUST_FAILURE ? L" — refused (enforce, condition 2)" : L"");
        return Error;
    }
    BsPrint(L"bundle: %lu files, %llu bytes, manifest %S\n",
            Bundle.FileCount, (unsigned long long)Bundle.TotalBytes,
            Bundle.ManifestDigest);

    for (i = 0; i < Bundle.FileCount; i++)
    {
        if (BsValidateTargetRelPath(Target, Bundle.Files[i].Dest) != ERROR_SUCCESS)
        {
            BsPrint(L"bundle: dest %ls is not a safe target path (refused)\n",
                    Bundle.Files[i].Dest);
            return ERROR_BAD_PATHNAME;
        }
    }
    ServiceWanted = BsHasServiceBinary(&Bundle);
    if (ServiceWanted && !BsHasServiceAtPath(&Bundle, BS_SERVICE_BINARY))
    {
        BsPrint(L"bundle: declares a service but not at %ls (refused)\n",
                BS_SERVICE_BINARY);
        return ERROR_BAD_PATHNAME;
    }

    for (i = 0; i < Bundle.FileCount; i++)
    {
        if (!Bundle.Files[i].Required)
            continue;
        if (swprintf(Source, RTL_NUMBER_OF(Source), L"%s\\%s",
                     Bundle.Path, Bundle.Files[i].Source) < 0)
            return ERROR_FILENAME_EXCED_RANGE;
        if (GetFileAttributesW(Source) == INVALID_FILE_ATTRIBUTES)
        {
            BsPrint(L"bundle: required file %ls missing\n", Bundle.Files[i].Source);
            return ERROR_FILE_NOT_FOUND;
        }
    }
    if (BsBuildTargetPath(Dest, RTL_NUMBER_OF(Dest), Target, L"LiberNT") != ERROR_SUCCESS)
        return ERROR_FILENAME_EXCED_RANGE;
    if (Target->IsImage)
    {
        if (!GetDiskFreeSpaceExW(Target->Root, NULL, NULL, (ULARGE_INTEGER *)&FreeBytes))
            return GetLastError();
    }
    else
    {
        WCHAR VolumeRoot[4];

        /* Same "C:" vs "C:\" contract as GetVolumeInformationW above. */
        VolumeRoot[0] = Target->Root[0];
        VolumeRoot[1] = L':';
        VolumeRoot[2] = L'\\';
        VolumeRoot[3] = 0;
        if (!GetDiskFreeSpaceExW(VolumeRoot, NULL, NULL, (ULARGE_INTEGER *)&FreeBytes))
            return GetLastError();
    }
    if (FreeBytes < Bundle.TotalBytes * 4 || FreeBytes < 32ull * 1024 * 1024)
    {
        BsPrint(L"target: not enough free space (%llu bytes free)\n",
                (unsigned long long)FreeBytes);
        return ERROR_DISK_FULL;
    }

    BsPrint(L"PLAN%s:\n", DryRun ? L" (dry-run)" : L"");
    BsPrint(L"  target: %ls (%ls)\n", Target->Root,
            Target->IsImage ? L"offline image" : L"online");
    BsPrint(L"  create secure servicing tree %ls\n", Dest);
    for (i = 0; i < Bundle.FileCount; i++)
        BsPrint(L"  copy %ls -> %ls\\%ls\n", Bundle.Files[i].Source,
                Target->Root, Bundle.Files[i].Dest);
    for (i = 0; i < Bundle.ShortcutCount; i++)
        BsPrint(L"  shortcut %ls -> %ls\\%ls\n", Bundle.Shortcuts[i].Name,
                Target->Root, Bundle.Shortcuts[i].Target);
    BsPrint(L"  pin development public key -> %ls\\%ls\n", Target->Root, BS_DEVKEY_FILE);
    BsPrint(L"  service: %s\n",
            ServiceWanted ? (Target->IsImage ? L"record stub in installed.json (offline)"
                                             : L"create LiberNTUpdateSvc disabled, enable after self-test")
                          : L"explicit stub (no service binary in bundle)");
    BsPrint(L"  self-test: read-back hashes, DACL check, pinned key, trust cert, simulated transaction\n");
    BsPrint(L"  BootExecute += %s (only after the self-test passes)\n", BS_BOOTEXECUTE_APP);
    if (DryRun)
        return ERROR_SUCCESS;

    BsLogInit(&BsLogState, Target);
    BsLog(&BsLogState, L"bootstrap %ls install start (bundle %ls)\n", BS_VERSION, Bundle.Path);

    Error = BsEnsureSecureTree(Target);
    if (Error != ERROR_SUCCESS)
    {
        BsPrint(L"servicing tree: failed or insecure (0x%08lx)\n", (unsigned long)Error);
        return Error;
    }

    /* IShellLinkW needs an apartment; the process is short-lived, so the
     * matching CoUninitialize is the process exit. */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    memset(&Inventory, 0, sizeof(Inventory));
    strcpy(Inventory.ManifestDigest, Bundle.ManifestDigest);
    for (i = 0; i < Bundle.FileCount; i++)
    {
        if (swprintf(Source, RTL_NUMBER_OF(Source), L"%s\\%s",
                     Bundle.Path, Bundle.Files[i].Source) < 0)
            return ERROR_FILENAME_EXCED_RANGE;
        Error = BsCopyFileToTarget(Target, Source, Bundle.Files[i].Dest,
                                   Bundle.Files[i].Sha256);
        if (Error != ERROR_SUCCESS)
        {
            BsPrint(L"copy %ls failed (0x%08lx)\n", Bundle.Files[i].Source,
                    (unsigned long)Error);
            return Error;
        }
        wcscpy(Inventory.Files[Inventory.FileCount].Path, Bundle.Files[i].Dest);
        strcpy(Inventory.Files[Inventory.FileCount].Sha256, Bundle.Files[i].Sha256);
        Inventory.FileCount++;
    }

    Error = BsInstallShortcuts(Target, &Bundle, &Inventory);
    if (Error != ERROR_SUCCESS)
    {
        BsPrint(L"shortcut creation failed (0x%08lx)\n", (unsigned long)Error);
        return Error;
    }

    if (BsBuildTargetPath(Dest, RTL_NUMBER_OF(Dest), Target, BS_DEVKEY_FILE) != ERROR_SUCCESS)
        return ERROR_FILENAME_EXCED_RANGE;
    {
        HANDLE Key = CreateFileW(Dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD Written;

        if (Key == INVALID_HANDLE_VALUE)
            return GetLastError();
        if (!WriteFile(Key, BsDevPublicKeyBytes(), S1_RSA2048_SIZE, &Written, NULL) ||
            Written != S1_RSA2048_SIZE)
        {
            Error = GetLastError();
            CloseHandle(Key);
            return Error;
        }
        CloseHandle(Key);
    }

    /* The service is created (or forced) DISABLED now; it is enabled only
     * after the self-test passes, and a failed self-test leaves it
     * disabled (Pam review B4/B6). */
    if (ServiceWanted && !Target->IsImage)
    {
        Error = BsServiceInstall(Target, FALSE, &ServiceInstalled);
        if (Error != ERROR_SUCCESS)
            return Error;
        wcscpy(Inventory.ServiceState, L"installed");
    }
    else
        wcscpy(Inventory.ServiceState, L"stub");

    Error = BsInventorySave(Target, &Inventory);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = BsSelfTest(Target, &Bundle, &Inventory, ServiceWanted && !Target->IsImage);
    if (Error != ERROR_SUCCESS)
    {
        BsPrint(L"self-test FAILED (0x%08lx) — service left disabled, BootExecute not registered\n",
                (unsigned long)Error);
        BsLog(&BsLogState, L"self-test failed 0x%08lx\n", (unsigned long)Error);
        if (ServiceWanted && !Target->IsImage)
            BsServiceInstall(Target, FALSE, &ServiceInstalled);
        return Error;
    }

    Error = BsBootExecuteAdd(Target, BS_BOOTEXECUTE_APP);
    if (Error != ERROR_SUCCESS)
    {
        BsPrint(L"BootExecute registration failed (0x%08lx)\n", (unsigned long)Error);
        return Error;
    }
    Inventory.BootExecute = TRUE;
    Error = BsInventorySave(Target, &Inventory);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (ServiceWanted && !Target->IsImage)
    {
        Error = BsServiceInstall(Target, TRUE, &ServiceInstalled);
        if (Error != ERROR_SUCCESS)
            return Error;
    }
    BsLog(&BsLogState, L"install complete\n");
    BsPrint(L"install complete: %lu files, BootExecute registered, self-test passed\n",
            Inventory.FileCount);
    return ERROR_SUCCESS;
}

/* ---------------------------------------------------------------- uninstall */

static DWORD
BsUninstall(
    _In_ BS_TARGET *Target,
    _In_ PCWSTR ExportDir,
    _In_ BOOL DryRun)
{
    BS_INVENTORY Inventory;
    ULONG i;
    DWORD Error;

    if (BsTargetFileExists(Target, L"LiberNT\\Servicing\\pending\\plan.json"))
    {
        BsPrint(L"uninstall refused: a servicing plan is pending; run it or roll it back first\n");
        return ERROR_BUSY;
    }
    Error = BsInventoryLoad(Target, &Inventory);
    if (Error != ERROR_SUCCESS)
    {
        BsPrint(L"uninstall: no valid %ls (0x%08lx)\n", BS_INSTALLED_JSON,
                (unsigned long)Error);
        return Error;
    }
    /* installed.json is data from the last install, not an authority: the
     * servicing tree must still satisfy the protected-DACL rule and every
     * recorded path must pass the same validation as at install time
     * before anything is deleted (Pam review B2). */
    {
        WCHAR Tree[BS_MAX_PATH_BUF];

        if (BsBuildTargetPath(Tree, RTL_NUMBER_OF(Tree), Target, BS_SERVICING_DIR) != ERROR_SUCCESS)
            return ERROR_FILENAME_EXCED_RANGE;
        Error = BsCheckSecure(Tree);
        if (Error != ERROR_SUCCESS)
        {
            BsPrint(L"uninstall: servicing tree is not secure (0x%08lx), refusing to trust the inventory\n",
                    (unsigned long)Error);
            return Error;
        }
        for (i = 0; i < Inventory.FileCount; i++)
        {
            Error = BsValidateTargetRelPath(Target, Inventory.Files[i].Path);
            if (Error != ERROR_SUCCESS)
            {
                BsPrint(L"uninstall: inventory path %ls is not safe (refused)\n",
                        Inventory.Files[i].Path);
                return Error;
            }
        }
        Error = BsValidateTargetRelPath(Target, BS_DEVKEY_FILE);
        if (Error != ERROR_SUCCESS)
            return Error;
    }

    BsPrint(L"PLAN%s:\n", DryRun ? L" (dry-run)" : L"");
    BsPrint(L"  BootExecute -= %s\n", BS_BOOTEXECUTE_APP);
    if (wcscmp(Inventory.ServiceState, L"installed") == 0 && !Target->IsImage)
        BsPrint(L"  delete service LiberNTUpdateSvc\n");
    for (i = 0; i < Inventory.FileCount; i++)
        BsPrint(L"  delete %ls\n", Inventory.Files[i].Path);
    BsPrint(L"  keep logs; export to %ls\n",
            ExportDir ? ExportDir : L"LiberNT\\Servicing\\logs (in place)");
    if (DryRun)
        return ERROR_SUCCESS;

    BsLogInit(&BsLogState, Target);
    BsLog(&BsLogState, L"bootstrap %ls uninstall start\n", BS_VERSION);

    if (ExportDir)
    {
        WCHAR Src[BS_MAX_PATH_BUF], Dst[BS_MAX_PATH_BUF];

        if (!CreateDirectoryW(ExportDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            return GetLastError();
        Error = BsBuildTargetPath(Src, RTL_NUMBER_OF(Src), Target,
                                  L"LiberNT\\Servicing\\journal\\transactions.jsonl");
        if (Error == ERROR_SUCCESS &&
            GetFileAttributesW(Src) != INVALID_FILE_ATTRIBUTES)
        {
            if (swprintf(Dst, RTL_NUMBER_OF(Dst), L"%ls\\transactions.jsonl", ExportDir) > 0)
                CopyFileW(Src, Dst, FALSE);
        }
        Error = BsBuildTargetPath(Src, RTL_NUMBER_OF(Src), Target, BS_LOGS_DIR);
        if (Error == ERROR_SUCCESS)
        {
            WCHAR FindPattern[BS_MAX_PATH_BUF];
            WIN32_FIND_DATAW Find;
            HANDLE FindHandle;

            swprintf(FindPattern, RTL_NUMBER_OF(FindPattern), L"%ls\\*", Src);
            FindHandle = FindFirstFileW(FindPattern, &Find);
            if (FindHandle != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (Find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        continue;
                    swprintf(Dst, RTL_NUMBER_OF(Dst), L"%ls\\%ls", ExportDir, Find.cFileName);
                    swprintf(FindPattern, RTL_NUMBER_OF(FindPattern),
                             L"%ls\\%ls", Src, Find.cFileName);
                    CopyFileW(FindPattern, Dst, FALSE);
                } while (FindNextFileW(FindHandle, &Find));
                FindClose(FindHandle);
            }
        }
    }

    Error = BsBootExecuteRemove(Target, BS_BOOTEXECUTE_APP);
    if (Error != ERROR_SUCCESS && Error != ERROR_FILE_NOT_FOUND)
        return Error;

    if (wcscmp(Inventory.ServiceState, L"installed") == 0 && !Target->IsImage)
    {
        Error = BsServiceRemove(Target);
        if (Error != ERROR_SUCCESS)
            return Error;
    }

    for (i = 0; i < Inventory.FileCount; i++)
    {
        Error = BsDeleteTargetFile(Target, Inventory.Files[i].Path);
        if (Error != ERROR_SUCCESS && Error != ERROR_FILE_NOT_FOUND)
            return Error;
    }

    /* Remove the pinned key, the inventory and any metadata the bootstrap
     * owns; the store/journal/logs stay for the record unless empty. */
    BsDeleteTargetFile(Target, BS_DEVKEY_FILE);
    BsDeleteTargetFile(Target, BS_INSTALLED_JSON);
    BsLog(&BsLogState, L"uninstall complete (files removed, logs kept)\n");
    BsPrint(L"uninstall complete: binaries, service and BootExecute reverted; logs kept\n");
    return ERROR_SUCCESS;
}

/* --------------------------------------------------------------------- main */

static VOID
BsUsage(VOID)
{
    BsPrint(L"libernt-update-bootstrap %ls\n", BS_VERSION);
    BsPrint(L"usage:\n");
    BsPrint(L"  libernt-update-bootstrap.exe install --bundle <dir> [--image <root>] [--dry-run]\n");
    BsPrint(L"  libernt-update-bootstrap.exe uninstall [--image <root>] [--export <dir>] [--dry-run]\n");
}

int
wmain(int argc, WCHAR **argv)
{
    BS_TARGET Target;
    const WCHAR *Image = NULL, *Bundle = NULL, *Export = NULL;
    BOOL DryRun = FALSE, Uninstall = FALSE;
    int i;
    DWORD Error;

    if (argc < 2)
    {
        BsUsage();
        return 1;
    }
    if (wcscmp(argv[1], L"install") == 0)
        Uninstall = FALSE;
    else if (wcscmp(argv[1], L"uninstall") == 0)
        Uninstall = TRUE;
    else
    {
        BsUsage();
        return 1;
    }
    for (i = 2; i < argc; i++)
    {
        if (wcscmp(argv[i], L"--image") == 0 && i + 1 < argc)
            Image = argv[++i];
        else if (wcscmp(argv[i], L"--bundle") == 0 && i + 1 < argc)
            Bundle = argv[++i];
        else if (wcscmp(argv[i], L"--export") == 0 && i + 1 < argc)
            Export = argv[++i];
        else if (wcscmp(argv[i], L"--dry-run") == 0)
            DryRun = TRUE;
        else
        {
            BsPrint(L"unknown or incomplete argument: %ls\n", argv[i]);
            BsUsage();
            return 1;
        }
    }
    if (!Uninstall && !Bundle)
    {
        BsPrint(L"install requires --bundle <dir>\n");
        return 1;
    }

    if (Image)
        Error = BsTargetInitImage(&Target, Image);
    else
        Error = BsTargetInitOnline(&Target);
    if (Error != ERROR_SUCCESS)
    {
        WCHAR Text[256];

        BsFormatError(Error, Text, RTL_NUMBER_OF(Text));
        BsPrint(L"target: %ls\n", Text);
        return 1;
    }

    Error = Uninstall ? BsUninstall(&Target, Export, DryRun)
                      : BsInstall(&Target, Bundle, DryRun);
    BsTargetFree(&Target);
    if (Error != ERROR_SUCCESS)
    {
        WCHAR Text[256];

        BsFormatError(Error, Text, RTL_NUMBER_OF(Text));
        BsPrint(L"FAILED: %ls (0x%08lx)\n", Text, (unsigned long)Error);
        return 1;
    }
    return 0;
}
