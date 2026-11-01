/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Target abstraction (online/image root) and the protected servicing tree
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "bootstrap.h"
#include <sddl.h>
#include <aclapi.h>

/* Same rule as libservicing CheckSecure (Pam review B1): owner SYSTEM or
 * Administrators, and a PROTECTED DACL made only of allow ACEs for SYSTEM
 * and Administrators. The SDDL below also pins the owner so the check holds
 * no matter which account creates the tree. */
static const WCHAR BsSecureSddl[] =
    L"O:SYG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";

static BOOL
BsSidTrusted(
    _In_ PSID Sid)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    static PSID SystemSid, AdminSid;
    static BOOL Resolved;

    if (!Resolved)
    {
        if (!AllocateAndInitializeSid(&NtAuthority, 1, SECURITY_LOCAL_SYSTEM_RID,
                                      0, 0, 0, 0, 0, 0, 0, &SystemSid))
            return FALSE;
        if (!AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                      DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                      &AdminSid))
            return FALSE;
        Resolved = TRUE;
    }
    return EqualSid(Sid, SystemSid) || EqualSid(Sid, AdminSid);
}

DWORD
BsCheckSecure(
    _In_ PCWSTR Path)
{
    PSECURITY_DESCRIPTOR Sd = NULL;
    PSID Owner = NULL;
    PACL Dacl = NULL;
    SECURITY_DESCRIPTOR_CONTROL Control = 0;
    DWORD Revision, i, Result = ERROR_ACCESS_DENIED;
    DWORD Error;

    Error = GetNamedSecurityInfoW((PWSTR)Path, SE_FILE_OBJECT,
                                  OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                  &Owner, NULL, &Dacl, NULL, &Sd);
    if (Error != ERROR_SUCCESS)
        return ERROR_ACCESS_DENIED; /* no ACLs (FAT) or unreadable: fail closed */
    if (!Owner || !Dacl ||
        !GetSecurityDescriptorControl(Sd, &Control, &Revision) ||
        !(Control & SE_DACL_PROTECTED))
        goto out;
    if (!BsSidTrusted(Owner) || Dacl->AceCount == 0)
        goto out;
    for (i = 0; i < Dacl->AceCount; i++)
    {
        ACE_HEADER *Ace;

        if (!GetAce(Dacl, i, (void **)&Ace))
            goto out;
        if (Ace->AceType != ACCESS_ALLOWED_ACE_TYPE)
            goto out;
        if (!BsSidTrusted((PSID)&((ACCESS_ALLOWED_ACE *)Ace)->SidStart))
            goto out;
    }
    Result = ERROR_SUCCESS;
out:
    LocalFree(Sd);
    return Result;
}

DWORD
BsCreateSecureDir(
    _In_ PCWSTR Path)
{
    PSECURITY_DESCRIPTOR Sd = NULL;
    SECURITY_ATTRIBUTES Sa;
    DWORD Error = ERROR_SUCCESS;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            BsSecureSddl, SDDL_REVISION_1, &Sd, NULL))
        return GetLastError();
    Sa.nLength = sizeof(Sa);
    Sa.lpSecurityDescriptor = Sd;
    Sa.bInheritHandle = FALSE;
    if (!CreateDirectoryW(Path, &Sa) && GetLastError() != ERROR_ALREADY_EXISTS)
        Error = GetLastError();
    LocalFree(Sd);
    /* A pre-existing directory (race or a previous partial run) must satisfy
     * the protected-DACL rule before we write into it. */
    if (Error == ERROR_SUCCESS)
        Error = BsCheckSecure(Path);
    return Error;
}

/* ---- relative path validation (mirrors libservicing; keep in sync) ------ */

static BOOL
BsIsReservedName(
    _In_ PCWSTR Name,
    _In_ size_t Length)
{
    static const WCHAR *const Reserved[] = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6",
        L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6",
        L"LPT7", L"LPT8", L"LPT9",
    };
    size_t i;

    for (i = 0; i < RTL_NUMBER_OF(Reserved); i++)
    {
        size_t n = wcslen(Reserved[i]);

        if (Length == n && _wcsnicmp(Name, Reserved[i], n) == 0)
            return TRUE;
    }
    if (Length == 4 && _wcsnicmp(Name, L"COM", 3) == 0 &&
        ((Name[3] >= L'0' && Name[3] <= L'9') ||
         Name[3] == 0x00B9 || Name[3] == 0x00B2 || Name[3] == 0x00B3))
        return TRUE;
    return FALSE;
}

/* Same acceptance as LnIsSafeRelativePath (libservicing/util.c): no root,
 * no drive, no "..", no wildcard or control characters, no trailing
 * dot/space, no reserved names, no NAME~N short-name aliasing. */
static BOOL
BsIsSafeRelativePath(
    _In_ PCWSTR Path)
{
    PCWSTR p = Path;

    if (!Path || !*Path)
        return FALSE;
    if (Path[0] == L'\\' || Path[0] == L'/' || wcschr(Path, L':') || wcschr(Path, L'/'))
        return FALSE;

    while (*p)
    {
        PCWSTR e = wcschr(p, L'\\');
        size_t n = e ? (size_t)(e - p) : wcslen(p);
        size_t i;

        if (n == 0)
            return FALSE;
        if ((n == 1 && p[0] == L'.') || (n == 2 && p[0] == L'.' && p[1] == L'.'))
            return FALSE;
        for (i = 0; i < n; i++)
        {
            if (p[i] < 0x20 || p[i] == L'*' || p[i] == L'?' || p[i] == L'"' ||
                p[i] == L'<' || p[i] == L'>' || p[i] == L'|')
                return FALSE;
        }
        if (p[n - 1] == L'.' || p[n - 1] == L' ')
            return FALSE;
        if (n > 255 || BsIsReservedName(p, n))
            return FALSE;
        for (i = 0; i + 1 < n; i++)
        {
            if (p[i] == L'~' && p[i + 1] >= L'0' && p[i + 1] <= L'9')
                return FALSE;
        }
        if (!e)
            break;
        p = e + 1;
    }
    return TRUE;
}

static DWORD
BsFinalPath(
    _In_ PCWSTR Path,
    _Out_ PWSTR Final,
    _In_ size_t Cch)
{
    HANDLE Handle;
    DWORD Len;

    Handle = CreateFileW(Path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return GetLastError();
    Len = GetFinalPathNameByHandleW(Handle, Final, (DWORD)Cch,
                                    VOLUME_NAME_DOS | FILE_NAME_NORMALIZED);
    CloseHandle(Handle);
    if (Len == 0 || Len >= Cch)
        return ERROR_BAD_PATHNAME;
    return ERROR_SUCCESS;
}

/* Mirrors LnTargetValidateReal (libservicing/target.c): the existing part
 * of the path must resolve to exactly the name that was written — no
 * reparse points, no 8.3 aliases — so a write cannot escape the target. */
static DWORD
BsTargetValidateReal(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR Relative)
{
    WCHAR Cur[BS_MAX_PATH_BUF], Deepest[BS_MAX_PATH_BUF];
    WCHAR FinalRoot[BS_MAX_PATH_BUF] = { 0 }, FinalDeep[BS_MAX_PATH_BUF] = { 0 };
    WCHAR Expect[BS_MAX_PATH_BUF];
    WCHAR RootNoSlash[BS_MAX_PATH_BUF];
    PCWSTR p = Relative;
    DWORD Error;
    size_t RootLen;
    PCWSTR Tail;

    if (!BsIsSafeRelativePath(Relative))
        return ERROR_BAD_PATHNAME;
    /* Root has no trailing backslash; without the separator the first
     * component would be appended drive-relative ("C:LiberNT"), the walk
     * would inspect nothing, and Deepest would never advance — leaving the
     * reparse check vacuous online and the canonical compare vacuous in
     * image mode (Pam's review of the online relaxation). */
    wcscpy(Cur, Target->Root);
    wcscat(Cur, L"\\");
    wcscpy(Deepest, Target->Root);
    wcscat(Deepest, L"\\");
    while (*p)
    {
        PCWSTR e = wcschr(p, L'\\');
        size_t n = e ? (size_t)(e - p) : wcslen(p);
        DWORD Attrs;

        if (wcslen(Cur) + n + 2 >= RTL_NUMBER_OF(Cur))
            return ERROR_BAD_PATHNAME;
        wcsncat(Cur, p, n);
        Attrs = GetFileAttributesW(Cur);
        if (Attrs == INVALID_FILE_ATTRIBUTES)
            break; /* the rest does not exist yet: the caller creates it */
        if (Attrs & FILE_ATTRIBUTE_REPARSE_POINT)
            return ERROR_BAD_PATHNAME; /* junction or symlink in the way */
        if (e)
        {
            if (!(Attrs & FILE_ATTRIBUTE_DIRECTORY))
                return ERROR_BAD_PATHNAME;
            wcscpy(Deepest, Cur);
            wcscat(Cur, L"\\");
        }
        if (!e)
            break;
        p = e + 1;
    }

    /* Everything below guards the offline --image flow: it proves the image
     * root is not the live system in disguise (Pam review E1). Online, the
     * root IS the live system drive ("C:"), BsFinalPath rejects a bare
     * drive path, and the canonical-name comparison has no meaning — the
     * prefix allowlist in BsValidateTargetRelPath is the whole check. */
    if (!Target->IsImage)
        return ERROR_SUCCESS;

    wcscpy(RootNoSlash, Target->Root);
    RootLen = wcslen(RootNoSlash);
    if (RootLen > 3 && RootNoSlash[RootLen - 1] == L'\\')
        RootNoSlash[RootLen - 1] = 0;
    Error = BsFinalPath(RootNoSlash, FinalRoot, RTL_NUMBER_OF(FinalRoot));
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = BsFinalPath(Deepest, FinalDeep, RTL_NUMBER_OF(FinalDeep));
    if (Error != ERROR_SUCCESS)
        return Error;
    {
        size_t Rl = wcslen(FinalRoot);

        wcscpy(Expect, FinalRoot);
        Tail = Deepest + wcslen(Target->Root);
        if (*Tail == L'\\')
            Tail++; /* Deepest carries the separator the root omits */
        if (Rl && Expect[Rl - 1] != L'\\' && *Tail)
            wcscat(Expect, L"\\");
        wcscat(Expect, Tail);
        Rl = wcslen(Expect);
        if (Rl > 3 && Expect[Rl - 1] == L'\\')
            Expect[Rl - 1] = 0;
        Rl = wcslen(FinalDeep);
        if (Rl > 3 && FinalDeep[Rl - 1] == L'\\')
            FinalDeep[Rl - 1] = 0;
    }
    if (_wcsicmp(Expect, FinalDeep) != 0)
        return ERROR_BAD_PATHNAME;
    return ERROR_SUCCESS;
}

DWORD
BsValidateTargetRelPath(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR Relative)
{
    /* Stage 1 installs only under these prefixes (DESIGN section 4), plus
     * the all-users Start Menu folder, where the manifest may declare
     * shortcuts; the narrow third prefix is the only exception and keeps
     * the installer out of the rest of the profile. */
    if (_wcsnicmp(Relative, L"Windows\\", 8) != 0 &&
        _wcsnicmp(Relative, L"LiberNT\\", 8) != 0 &&
        _wcsnicmp(Relative, L"ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\",
                  RTL_NUMBER_OF(L"ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\") - 1) != 0)
        return ERROR_BAD_PATHNAME;
    return BsTargetValidateReal(Target, Relative);
}

DWORD
BsEnablePrivilege(
    _In_ PCWSTR Privilege)
{
    HANDLE Token;
    TOKEN_PRIVILEGES TokenPrivileges;
    LUID Luid;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Token))
        return GetLastError();
    if (!LookupPrivilegeValueW(NULL, Privilege, &Luid))
    {
        CloseHandle(Token);
        return GetLastError();
    }
    TokenPrivileges.PrivilegeCount = 1;
    TokenPrivileges.Privileges[0].Luid = Luid;
    TokenPrivileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(Token, FALSE, &TokenPrivileges,
                               sizeof(TokenPrivileges), NULL, NULL))
    {
        CloseHandle(Token);
        return GetLastError();
    }
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
    {
        CloseHandle(Token);
        return ERROR_PRIVILEGE_NOT_HELD;
    }
    CloseHandle(Token);
    return ERROR_SUCCESS;
}

DWORD
BsTargetInitOnline(
    _Out_ BS_TARGET *Target)
{
    WCHAR WindowsDir[BS_MAX_PATH_BUF];
    WCHAR FsName[32];
    DWORD Error;

    memset(Target, 0, sizeof(*Target));
    Target->IsImage = FALSE;
    Target->VolumeLock = NULL;

    if (!GetWindowsDirectoryW(WindowsDir, RTL_NUMBER_OF(WindowsDir)))
        return GetLastError();
    if (WindowsDir[1] != L':')
        return ERROR_BAD_ENVIRONMENT;
    Target->Root[0] = WindowsDir[0];
    Target->Root[1] = L':';
    Target->Root[2] = 0;

    /* GetVolumeInformationW requires the root WITH the trailing backslash
     * ("C:\"); "C:" alone fails with ERROR_INVALID_NAME. Root itself stays
     * backslash-less: BsBuildTargetPath supplies the separator. */
    {
        WCHAR VolumeRoot[4];

        VolumeRoot[0] = Target->Root[0];
        VolumeRoot[1] = L':';
        VolumeRoot[2] = L'\\';
        VolumeRoot[3] = 0;
        if (!GetVolumeInformationW(VolumeRoot, NULL, 0, NULL, NULL, NULL,
                                   FsName, RTL_NUMBER_OF(FsName)))
            return GetLastError();
    }
    if (wcscmp(FsName, L"NTFS") != 0)
        return ERROR_NOT_SUPPORTED; /* DACL protection requires NTFS */

    Error = BsEnablePrivilege(SE_RESTORE_NAME);
    if (Error != ERROR_SUCCESS)
        return Error;
    return ERROR_SUCCESS;
}

DWORD
BsTargetInitImage(
    _Out_ BS_TARGET *Target,
    _In_ PCWSTR ImageRoot)
{
    WCHAR Probe[BS_MAX_PATH_BUF];
    size_t RootLen;
    DWORD Attr, Error;

    memset(Target, 0, sizeof(*Target));
    Target->IsImage = TRUE;
    Target->VolumeLock = NULL;
    if (wcslen(ImageRoot) >= RTL_NUMBER_OF(Target->Root))
        return ERROR_FILENAME_EXCED_RANGE;
    wcscpy(Target->Root, ImageRoot);
    if (Target->Root[wcslen(Target->Root) - 1] == L'\\')
        Target->Root[wcslen(Target->Root) - 1] = 0;
    RootLen = wcslen(Target->Root);

    /* --image must be an offline root, never the running system: refuse NT
     * namespace spellings, a drive root, the live Windows directory (in any
     * spelling, including "C:\Windows\..", 8.3 aliases and roots reached
     * through ancestor junctions — all collapse once the root is
     * canonicalized), any volume root, any ancestor of the Windows
     * directory, and anything inside the Windows directory. Without this,
     * a wrong --image would copy into the real C:\Windows before the hive
     * load fails against the locked live hive (Pam reviews E1). */
    if (RootLen >= 4 &&
        (wcsncmp(Target->Root, L"\\??\\", 4) == 0 ||
         wcsncmp(Target->Root, L"\\\\?\\", 4) == 0 ||
         wcsncmp(Target->Root, L"\\\\.\\", 4) == 0))
        return ERROR_NOT_SUPPORTED;
    Attr = GetFileAttributesW(Target->Root);
    if (Attr == INVALID_FILE_ATTRIBUTES)
        return ERROR_PATH_NOT_FOUND;
    if (Attr & FILE_ATTRIBUTE_REPARSE_POINT)
        return ERROR_NOT_SUPPORTED;
    if (RootLen <= 3 && Target->Root[1] == L':')
        return ERROR_NOT_SUPPORTED; /* drive root such as C: or C:\ */
    {
        WCHAR FinalRoot[BS_MAX_PATH_BUF], WinDir[BS_MAX_PATH_BUF], WinFinal[BS_MAX_PATH_BUF];
        DWORD FinalLen, WinLen;

        if (!GetWindowsDirectoryW(WinDir, RTL_NUMBER_OF(WinDir)))
            return GetLastError();
        /* BsFinalPath already returns the error code (and reports
         * ERROR_BAD_PATHNAME without SetLastError), so propagate it as-is:
         * a failed canonicalization must fail closed, never open. */
        Error = BsFinalPath(Target->Root, FinalRoot, RTL_NUMBER_OF(FinalRoot));
        if (Error != ERROR_SUCCESS)
            return Error;
        Error = BsFinalPath(WinDir, WinFinal, RTL_NUMBER_OF(WinFinal));
        if (Error != ERROR_SUCCESS)
            return Error;
        FinalLen = (DWORD)wcslen(FinalRoot);
        WinLen = (DWORD)wcslen(WinFinal);
        if (FinalLen >= 2 && FinalLen <= 3 && FinalRoot[1] == L':')
            return ERROR_NOT_SUPPORTED; /* canonical volume root */
        if (_wcsicmp(FinalRoot, WinFinal) == 0)
            return ERROR_NOT_SUPPORTED; /* the live Windows directory */
        if (FinalLen < WinLen && _wcsnicmp(WinFinal, FinalRoot, FinalLen) == 0 &&
            WinFinal[FinalLen] == L'\\')
            return ERROR_NOT_SUPPORTED; /* an ancestor of the Windows directory */
        if (WinLen < FinalLen && _wcsnicmp(FinalRoot, WinFinal, WinLen) == 0 &&
            FinalRoot[WinLen] == L'\\')
            return ERROR_NOT_SUPPORTED; /* inside the live Windows directory */
    }

    if (swprintf(Probe, RTL_NUMBER_OF(Probe),
                 L"%s\\Windows\\System32\\config\\SYSTEM", Target->Root) < 0)
        return ERROR_FILENAME_EXCED_RANGE;
    Attr = GetFileAttributesW(Probe);
    if (Attr == INVALID_FILE_ATTRIBUTES || (Attr & FILE_ATTRIBUTE_DIRECTORY))
        return ERROR_DIRECTORY;
    if (Attr & FILE_ATTRIBUTE_REPARSE_POINT)
        return ERROR_NOT_SUPPORTED;
    return ERROR_SUCCESS;
}

VOID
BsTargetFree(
    _Inout_ BS_TARGET *Target)
{
    if (Target->VolumeLock && Target->VolumeLock != INVALID_HANDLE_VALUE)
    {
        DWORD Bytes;

        DeviceIoControl(Target->VolumeLock, FSCTL_UNLOCK_VOLUME,
                        NULL, 0, NULL, 0, &Bytes, NULL);
        CloseHandle(Target->VolumeLock);
        Target->VolumeLock = NULL;
    }
}

DWORD
BsBuildTargetPath(
    _Out_ PWSTR Buffer,
    _In_ size_t Cch,
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR Relative)
{
    INT Len;

    Len = swprintf(Buffer, Cch, L"%s\\%s", Target->Root, Relative);
    if (Len < 0 || (size_t)Len >= Cch)
        return ERROR_FILENAME_EXCED_RANGE;
    return ERROR_SUCCESS;
}

DWORD
BsEnsureSecureTree(
    _In_ const BS_TARGET *Target)
{
    static const PCWSTR Dirs[] = {
        L"LiberNT",
        BS_SERVICING_DIR,
        BS_STORE_DIR,
        BS_JOURNAL_DIR,
        BS_PENDING_DIR,
        BS_BACKUP_DIR,
        BS_BIN_DIR,
        BS_LOGS_DIR,
        BS_META_DIR,
    };
    ULONG i;
    WCHAR Path[BS_MAX_PATH_BUF];
    DWORD Error;

    for (i = 0; i < RTL_NUMBER_OF(Dirs); i++)
    {
        Error = BsBuildTargetPath(Path, RTL_NUMBER_OF(Path), Target, Dirs[i]);
        if (Error != ERROR_SUCCESS)
            return Error;
        if (GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES)
        {
            /* Existing tree: it must already satisfy the protected-DACL rule. */
            Error = BsCheckSecure(Path);
            if (Error != ERROR_SUCCESS)
                return Error;
            continue;
        }
        Error = BsCreateSecureDir(Path);
        if (Error != ERROR_SUCCESS)
            return Error;
    }
    return ERROR_SUCCESS;
}

DWORD
BsCopyFileToTarget(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR Source,
    _In_ PCWSTR RelativeDest,
    _In_ PCSTR ExpectedSha256)
{
    WCHAR Dest[BS_MAX_PATH_BUF], Temp[BS_MAX_PATH_BUF];
    HANDLE In, Out;
    uint8_t Buffer[64 * 1024];
    S1_SHA256_CTX Ctx;
    uint8_t Digest[S1_SHA256_SIZE];
    char Hex[65];
    DWORD Read, Written;
    DWORD Error;

    Error = BsValidateTargetRelPath(Target, RelativeDest);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = BsBuildTargetPath(Dest, RTL_NUMBER_OF(Dest), Target, RelativeDest);
    if (Error != ERROR_SUCCESS)
        return Error;
    if (swprintf(Temp, RTL_NUMBER_OF(Temp), L"%s.tmp", Dest) < 0)
        return ERROR_FILENAME_EXCED_RANGE;

    In = CreateFileW(Source, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (In == INVALID_HANDLE_VALUE)
        return GetLastError();
    /* Copy to a temp name next to the destination, verify the hash, then
     * rename over the destination: an interruption never leaves a partial
     * binary in place (Pam review B4). */
    Out = CreateFileW(Temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    if (Out == INVALID_HANDLE_VALUE)
    {
        Error = GetLastError();
        CloseHandle(In);
        return Error;
    }
    S1Sha256Init(&Ctx);
    for (;;)
    {
        if (!ReadFile(In, Buffer, sizeof(Buffer), &Read, NULL))
        {
            Error = GetLastError();
            break;
        }
        if (Read == 0)
        {
            Error = ERROR_SUCCESS;
            break;
        }
        S1Sha256Update(&Ctx, Buffer, Read);
        if (!WriteFile(Out, Buffer, Read, &Written, NULL) || Written != Read)
        {
            Error = GetLastError();
            break;
        }
    }
    if (Error == ERROR_SUCCESS && !FlushFileBuffers(Out))
        Error = GetLastError();
    CloseHandle(Out);
    CloseHandle(In);
    if (Error != ERROR_SUCCESS)
    {
        DeleteFileW(Temp);
        return Error;
    }
    S1Sha256Final(&Ctx, Digest);
    S1HexEncode(Digest, S1_SHA256_SIZE, Hex);
    if (_stricmp(Hex, ExpectedSha256) != 0)
    {
        DeleteFileW(Temp);
        return ERROR_CRC;
    }
    if (!MoveFileExW(Temp, Dest, MOVEFILE_REPLACE_EXISTING))
    {
        Error = GetLastError();
        DeleteFileW(Temp);
        return Error;
    }
    return ERROR_SUCCESS;
}

DWORD
BsHashTargetFile(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR RelativePath,
    _Out_ char *HexOut)
{
    WCHAR Path[BS_MAX_PATH_BUF];
    HANDLE In;
    uint8_t Buffer[64 * 1024];
    S1_SHA256_CTX Ctx;
    uint8_t Digest[S1_SHA256_SIZE];
    DWORD Read;
    DWORD Error = ERROR_SUCCESS;

    Error = BsBuildTargetPath(Path, RTL_NUMBER_OF(Path), Target, RelativePath);
    if (Error != ERROR_SUCCESS)
        return Error;
    In = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (In == INVALID_HANDLE_VALUE)
        return GetLastError();
    S1Sha256Init(&Ctx);
    for (;;)
    {
        if (!ReadFile(In, Buffer, sizeof(Buffer), &Read, NULL))
        {
            Error = GetLastError();
            break;
        }
        if (Read == 0)
            break;
        S1Sha256Update(&Ctx, Buffer, Read);
    }
    CloseHandle(In);
    if (Error == ERROR_SUCCESS)
    {
        S1Sha256Final(&Ctx, Digest);
        S1HexEncode(Digest, S1_SHA256_SIZE, HexOut);
    }
    return Error;
}

DWORD
BsDeleteTargetFile(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR RelativePath)
{
    WCHAR Path[BS_MAX_PATH_BUF];
    DWORD Error;

    /* installed.json is data, not an authority: revalidate every path the
     * same way as at install time before deleting (Pam review B2). */
    Error = BsValidateTargetRelPath(Target, RelativePath);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = BsBuildTargetPath(Path, RTL_NUMBER_OF(Path), Target, RelativePath);
    if (Error != ERROR_SUCCESS)
        return Error;
    if (!DeleteFileW(Path))
        return GetLastError();
    return ERROR_SUCCESS;
}

BOOL
BsTargetFileExists(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR RelativePath)
{
    WCHAR Path[BS_MAX_PATH_BUF];

    if (BsBuildTargetPath(Path, RTL_NUMBER_OF(Path), Target, RelativePath) != ERROR_SUCCESS)
        return FALSE;
    return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}
