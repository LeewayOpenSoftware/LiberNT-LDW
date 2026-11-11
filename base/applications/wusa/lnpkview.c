/*
 * PROJECT:     LiberNT Update - Windows Update Standalone Installer
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Read-only view of the LNPK container appended to a package .exe (lnpk.h)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Reads the trailer, the table and the embedded manifest.json for the text of the dialog, and extracts on
 * request (/extract). It verifies nothing cryptographic: trust comes from manifest.sig and the payload
 * hashes, checked by libservicing when the package is added.
 */
#include "wusa.h"
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

static BOOL ReadAt(uint64_t offset, void *buf, DWORD len)
{
    LARGE_INTEGER li;
    DWORD got = 0;

    li.QuadPart = (LONGLONG)offset;
    return SetFilePointerEx(g_file, li, NULL, FILE_BEGIN) && ReadFile(g_file, buf, len, &got, NULL) && got == len;
}

/* trailer and table only; names are kept as utf-8 bytes */
BOOL WusaOpenContainer(const WCHAR *self)
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

BOOL WusaReadIdentity(WUSA_IDENT *ident)
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
        memset(ident, 0, sizeof(*ident));
        ok = JsonString(text, "packageId", ident->id, sizeof(ident->id)) &&
             JsonString(text, "releaseVersion", ident->version, sizeof(ident->version)) &&
             WusaValidToken(ident->id) && WusaValidToken(ident->version);
        /* optional, display only: the signed manifest may name the KB and give a title and a description */
        JsonString(text, "kb", ident->kb, sizeof(ident->kb));
        JsonString(text, "title", ident->title, sizeof(ident->title));
        JsonString(text, "description", ident->description, sizeof(ident->description));
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

DWORD WusaExtractContainer(const WCHAR *dir)
{
    WCHAR path[MAX_PATH * 2];
    BYTE buf[65536];
    DWORD i;

    lstrcpynW(path, dir, MAX_PATH);
    if (!EnsureDir(path))
        return GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
    for (i = 0; i < g_count; i++)
    {
        WCHAR name[LNPK_MAX_NAME + 1], *slash;
        HANDLE out;
        uint64_t left = g_entries[i].size, offset = g_entries[i].offset;

        if (!MultiByteToWideChar(CP_UTF8, 0, g_entries[i].name, -1, name, LNPK_MAX_NAME + 1) ||
            wcsstr(name, L"..") || name[0] == L'\\' || wcschr(name, L':'))
            return WUSA_E_NOTPACKAGE;                      /* a container may only hold relative names */
        wsprintfW(path, L"%s\\%s", dir, name);
        slash = wcsrchr(path, L'\\');
        *slash = 0;
        EnsureDir(path);
        *slash = L'\\';
        out = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (out == INVALID_HANDLE_VALUE)
            return ERROR_WRITE_FAULT;
        while (left)
        {
            DWORD n = left > sizeof(buf) ? sizeof(buf) : (DWORD)left, wrote;

            if (!ReadAt(offset, buf, n) || !WriteFile(out, buf, n, &wrote, NULL) || wrote != n)
            {
                CloseHandle(out);
                return ERROR_WRITE_FAULT;
            }
            offset += n;
            left -= n;
        }
        CloseHandle(out);
    }
    return 0;
}


void WusaCloseContainer(void)
{
    if (g_file && g_file != INVALID_HANDLE_VALUE)
        CloseHandle(g_file);
    g_file = NULL;
    if (g_entries)
        HeapFree(GetProcessHeap(), 0, g_entries);
    g_entries = NULL;
    g_count = 0;
}
