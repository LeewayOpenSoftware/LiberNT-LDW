/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Launcher stub of a standalone update package (LNPK, S1-PACKAGE-EXE-DESIGN.md)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The package tool appends the S1 package directory to this program (see libservicing/lnpk.h). The stub
 * writes no system file and verifies nothing: it hands ITSELF to LiberNT Update
 * (LiberNT\Servicing\bin\libernt-update.exe add-package --exe <own path>), which checks the container, the
 * manifest signature against the pinned certificate and every payload hash, and stages the package into
 * the normal transaction route; then it asks for apply. Flags:
 *   /quiet       no message boxes       /norestart   stage and apply, never ask to restart (the default too:
 *                                                    the stub never restarts; 3010 says a restart is needed)
 *   /stageonly   add-package only       /uninstall   remove-package <id> <version>
 *   /extract:<dir>  write the container's files to <dir> for inspection (the only mode that writes files)
 * Exit codes: 0 done, 3010 done and a restart is required, 1 usage, 2 LiberNT Update not installed,
 * 3 not a package, otherwise the exit code of libernt-update.exe.
 */
#include <windows.h>
#include <shellapi.h>
#include <string.h>
#include <stdint.h>
#include "../../system/servicing/libservicing/lnpk.h"

#define MAX_ENTRIES 4100
#define MANIFEST_CAP (1024 * 1024)

typedef struct _ENTRY
{
    uint64_t offset, size;
    char name[LNPK_MAX_NAME + 1];
} ENTRY;

static HANDLE g_file;
static ENTRY *g_entries;
static DWORD g_count;
static BOOL g_quiet;
static char g_id[80], g_version[80];

static void Say(const WCHAR *text, UINT flags)
{
    if (!g_quiet)
        MessageBoxW(NULL, text, L"LiberNT Update", flags);
}

static BOOL ReadAt(uint64_t offset, void *buf, DWORD len)
{
    LARGE_INTEGER li;
    DWORD got = 0;

    li.QuadPart = (LONGLONG)offset;
    return SetFilePointerEx(g_file, li, NULL, FILE_BEGIN) && ReadFile(g_file, buf, len, &got, NULL) && got == len;
}

/* trailer and table only; names are kept as utf-8 bytes */
static BOOL OpenContainer(const WCHAR *self)
{
    LARGE_INTEGER size;
    LNPK_TRAILER tr;
    BYTE *toc;
    uint64_t tocLen;
    DWORD i, pos = 0;

    g_file = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (g_file == INVALID_HANDLE_VALUE || !GetFileSizeEx(g_file, &size) || size.QuadPart < LNPK_TRAILER_SIZE + 1)
        return FALSE;
    if (!ReadAt((uint64_t)size.QuadPart - LNPK_TRAILER_SIZE, &tr, LNPK_TRAILER_SIZE))
        return FALSE;
    if (memcmp(tr.magic, LNPK_MAGIC, 4) || tr.version != LNPK_VERSION || tr.tocCount == 0 ||
        tr.tocCount > MAX_ENTRIES || tr.tocOffset >= (uint64_t)size.QuadPart - LNPK_TRAILER_SIZE)
        return FALSE;
    tocLen = (uint64_t)size.QuadPart - LNPK_TRAILER_SIZE - tr.tocOffset;
    if (tocLen > LNPK_MAX_TOC)
        return FALSE;
    toc = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)tocLen);
    g_entries = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ENTRY) * tr.tocCount);
    if (!toc || !g_entries || !ReadAt(tr.tocOffset, toc, (DWORD)tocLen))
        return FALSE;
    for (i = 0; i < tr.tocCount; i++)
    {
        LNPK_ENTRY_HEAD head;

        if (pos + sizeof(head) > tocLen)
            return FALSE;
        memcpy(&head, toc + pos, sizeof(head));
        pos += sizeof(head);
        if (head.nameLength == 0 || head.nameLength > LNPK_MAX_NAME || pos + head.nameLength > tocLen)
            return FALSE;
        memcpy(g_entries[i].name, toc + pos, head.nameLength);
        g_entries[i].name[head.nameLength] = 0;
        g_entries[i].offset = head.offset;
        g_entries[i].size = head.size;
        pos += head.nameLength;
    }
    g_count = tr.tocCount;
    HeapFree(GetProcessHeap(), 0, toc);
    return TRUE;
}

/* "key": "value" out of the embedded manifest.json, without a JSON parser: enough for the id and version */
static BOOL JsonString(const char *text, const char *key, char *out, size_t cap)
{
    const char *p = text, *q;
    size_t kl = strlen(key), n;

    while ((p = strstr(p, key)) != NULL)
    {
        if (p > text && p[-1] == '"' && p[kl] == '"')
        {
            p += kl + 1;
            while (*p == ' ' || *p == '\t' || *p == ':')
                p++;
            if (*p != '"')
                return FALSE;
            q = strchr(++p, '"');
            if (!q)
                return FALSE;
            n = (size_t)(q - p);
            if (n == 0 || n >= cap)
                return FALSE;
            memcpy(out, p, n);
            out[n] = 0;
            return TRUE;
        }
        p += kl;
    }
    return FALSE;
}

static BOOL ReadIdentity(void)
{
    DWORD i;

    for (i = 0; i < g_count; i++)
    {
        char *text;
        BOOL ok;

        if (lstrcmpiA(g_entries[i].name, "manifest.json") || g_entries[i].size > MANIFEST_CAP)
            continue;
        text = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)g_entries[i].size + 1);
        if (!text || !ReadAt(g_entries[i].offset, text, (DWORD)g_entries[i].size))
            return FALSE;
        text[g_entries[i].size] = 0;
        ok = JsonString(text, "packageId", g_id, sizeof(g_id)) &&
             JsonString(text, "releaseVersion", g_version, sizeof(g_version));
        HeapFree(GetProcessHeap(), 0, text);
        return ok;
    }
    return FALSE;
}

static BOOL EnsureDir(WCHAR *path)
{
    WCHAR *p;

    for (p = path + 1; *p; p++)
    {
        if (*p == L'\\')
        {
            *p = 0;
            CreateDirectoryW(path, NULL);
            *p = L'\\';
        }
    }
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int Extract(const WCHAR *dir)
{
    WCHAR path[MAX_PATH * 2];
    BYTE buf[65536];
    DWORD i;

    lstrcpynW(path, dir, MAX_PATH);
    if (!EnsureDir(path))
        return 4;
    for (i = 0; i < g_count; i++)
    {
        WCHAR name[LNPK_MAX_NAME + 1], *slash;
        HANDLE out;
        uint64_t left = g_entries[i].size, offset = g_entries[i].offset;

        if (!MultiByteToWideChar(CP_UTF8, 0, g_entries[i].name, -1, name, LNPK_MAX_NAME + 1) ||
            wcsstr(name, L"..") || name[0] == L'\\' || wcschr(name, L':'))
            return 3;                                      /* a container may only hold relative names */
        wsprintfW(path, L"%s\\%s", dir, name);
        slash = wcsrchr(path, L'\\');
        *slash = 0;
        EnsureDir(path);
        *slash = L'\\';
        out = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (out == INVALID_HANDLE_VALUE)
            return 4;
        while (left)
        {
            DWORD n = left > sizeof(buf) ? sizeof(buf) : (DWORD)left, wrote;

            if (!ReadAt(offset, buf, n) || !WriteFile(out, buf, n, &wrote, NULL) || wrote != n)
            {
                CloseHandle(out);
                return 4;
            }
            offset += n;
            left -= n;
        }
        CloseHandle(out);
    }
    return 0;
}

static BOOL IsElevated(void)
{
    SID_IDENTIFIER_AUTHORITY nt = { SECURITY_NT_AUTHORITY };
    PSID admins;
    BOOL member = FALSE;

    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins))
        return FALSE;
    if (!CheckTokenMembership(NULL, admins, &member))
        member = FALSE;
    FreeSid(admins);
    return member;
}

/* run libernt-update.exe with the given arguments, elevated when we are not; the exit code is returned */
static DWORD RunUpdater(const WCHAR *cli, const WCHAR *args)
{
    SHELLEXECUTEINFOW sei;
    DWORD code = 1;

    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = IsElevated() ? L"open" : L"runas";
    sei.lpFile = cli;
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess)
        return GetLastError() ? GetLastError() : 1;
    WaitForSingleObject(sei.hProcess, INFINITE);
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdLine, int show)
{
    WCHAR self[MAX_PATH], cli[MAX_PATH], pending[MAX_PATH], args[MAX_PATH * 2 + 64], text[512];
    const WCHAR *extractDir = NULL;
    BOOL stageOnly = FALSE, uninstall = FALSE;
    int argc, i;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    DWORD code;

    (void)inst; (void)prev; (void)cmdLine; (void)show;
    for (i = 1; argv && i < argc; i++)
    {
        const WCHAR *a = argv[i];

        if (a[0] == L'-')
            a++;
        else if (a[0] == L'/')
            a++;
        else
            return 1;
        if (!lstrcmpiW(a, L"quiet") || !lstrcmpiW(a, L"q"))
            g_quiet = TRUE;
        else if (!lstrcmpiW(a, L"norestart"))
            ;                                              /* the stub never restarts anyway */
        else if (!lstrcmpiW(a, L"stageonly"))
            stageOnly = TRUE;
        else if (!lstrcmpiW(a, L"uninstall"))
            uninstall = TRUE;
        else if (!_wcsnicmp(a, L"extract:", 8) && a[8])
            extractDir = a + 8;
        else
            return 1;
    }

    if (!GetModuleFileNameW(NULL, self, MAX_PATH) || !OpenContainer(self) || !ReadIdentity())
    {
        Say(L"This file does not carry a LiberNT Update package.", MB_ICONERROR);
        return 3;
    }
    if (extractDir)
        return Extract(extractDir);

    GetWindowsDirectoryW(cli, MAX_PATH);
    cli[3] = 0;                                            /* the system drive root */
    wsprintfW(pending, L"%sLiberNT\\Servicing\\pending\\plan.json", cli);
    lstrcatW(cli, L"LiberNT\\Servicing\\bin\\libernt-update.exe");
    if (GetFileAttributesW(cli) == INVALID_FILE_ATTRIBUTES)
    {
        Say(L"LiberNT Update is not installed on this system, so this package cannot be applied.", MB_ICONERROR);
        return 2;
    }

    if (uninstall)
    {
        wsprintfW(text, L"Remove update %S %S from this system?", g_id, g_version);
        if (!g_quiet && MessageBoxW(NULL, text, L"LiberNT Update", MB_ICONQUESTION | MB_YESNO) != IDYES)
            return 0;
        wsprintfW(args, L"remove-package %S %S", g_id, g_version);
        code = RunUpdater(cli, args);
        if (code == 0)
            Say(L"The update was removed. Restart the computer if it asks for it.", MB_ICONINFORMATION);
        else
            Say(L"The update could not be removed; see the LiberNT Update log.", MB_ICONERROR);
        return (int)code;
    }

    wsprintfW(text, L"Install update %S %S on this system?", g_id, g_version);
    if (!g_quiet && MessageBoxW(NULL, text, L"LiberNT Update", MB_ICONQUESTION | MB_YESNO) != IDYES)
        return 0;
    wsprintfW(args, L"add-package --exe \"%s\"", self);
    code = RunUpdater(cli, args);
    if (code != 0)
    {
        Say(L"The update was refused: it is not signed for this system or it is damaged.", MB_ICONERROR);
        return (int)code;
    }
    if (stageOnly)
    {
        Say(L"The update is staged. Apply it from LiberNT Update.", MB_ICONINFORMATION);
        return 0;
    }
    wsprintfW(args, L"apply %S %S", g_id, g_version);
    code = RunUpdater(cli, args);
    if (code != 0)
    {
        Say(L"The update could not be applied; see the LiberNT Update log.", MB_ICONERROR);
        return (int)code;
    }
    if (GetFileAttributesW(pending) != INVALID_FILE_ATTRIBUTES)
    {
        Say(L"The update is installed and will finish at the next restart.", MB_ICONINFORMATION);
        return 3010;
    }
    Say(L"The update is installed.", MB_ICONINFORMATION);
    return 0;
}
