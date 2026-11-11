/*
 * PROJECT:     LiberNT Update - Windows Update Standalone Installer
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Command line parsing and the small pure helpers of wusa.exe
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 *   wusa <file.exe> [/quiet] [/norestart | /forcerestart | /promptrestart] [/log:<file>]
 *   wusa <file.exe> /extract:<dir>
 *   wusa /uninstall <file.exe> | /uninstall /kb:<id>   [/quiet] [/norestart | /forcerestart | /promptrestart]
 * Switches start with / or -, are case-insensitive, and may come in any order next to the one file name.
 */
#include "wusa.h"
#include <string.h>

BOOL WusaNormalizeKb(const WCHAR *in, WCHAR *out, size_t cap)
{
    size_t i, digits = 0;

    if (!in || cap < 4)
        return FALSE;
    if ((in[0] == L'K' || in[0] == L'k') && (in[1] == L'B' || in[1] == L'b'))
        in += 2;
    for (i = 0; in[i]; i++)
    {
        if (in[i] < L'0' || in[i] > L'9')
            return FALSE;
        digits++;
    }
    if (digits == 0 || digits > 12 || digits + 3 > cap)
        return FALSE;
    out[0] = L'K';
    out[1] = L'B';
    memcpy(out + 2, in, (digits + 1) * sizeof(WCHAR));
    return TRUE;
}

BOOL WusaValidToken(const char *s)
{
    size_t n = 0;

    if (!s || !*s)
        return FALSE;
    for (; *s; s++, n++)
    {
        char c = *s;

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return FALSE;
    }
    return n <= 64;
}

DWORD WusaParseArgs(int argc, WCHAR **argv, WUSA_ARGS *out)
{
    int i;
    BOOL uninstall = FALSE, haveKb = FALSE, haveExtract = FALSE;
    int restartModes = 0;

    memset(out, 0, sizeof(*out));
    for (i = 1; i < argc; i++)
    {
        const WCHAR *a = argv[i];

        if (a[0] != L'/' && a[0] != L'-')
        {
            if (out->hasFile || wcslen(a) >= MAX_PATH)
                return WUSA_E_USAGE;                       /* one file only */
            wcscpy(out->file, a);
            out->hasFile = TRUE;
            continue;
        }
        a++;
        if (!_wcsicmp(a, L"quiet") || !_wcsicmp(a, L"q"))
            out->quiet = TRUE;
        else if (!_wcsicmp(a, L"norestart"))
        {
            out->norestart = TRUE;
            restartModes++;
        }
        else if (!_wcsicmp(a, L"forcerestart"))
        {
            out->forcerestart = TRUE;
            restartModes++;
        }
        else if (!_wcsicmp(a, L"promptrestart") || !_wcsicmp(a, L"warnrestart"))
            restartModes++;                                /* the default behaviour; accepted for compatibility */
        else if (!_wcsicmp(a, L"uninstall"))
            uninstall = TRUE;
        else if (!_wcsnicmp(a, L"kb:", 3))
        {
            if (!WusaNormalizeKb(a + 3, out->kb, ARRAYSIZE(out->kb)))
                return WUSA_E_USAGE;
            haveKb = TRUE;
        }
        else if (!_wcsnicmp(a, L"extract:", 8))
        {
            if (!a[8] || wcslen(a + 8) >= MAX_PATH)
                return WUSA_E_USAGE;
            wcscpy(out->extractDir, a + 8);
            haveExtract = TRUE;
        }
        else if (!_wcsnicmp(a, L"log:", 4) && a[4])
            ;                                              /* accepted, the log is LiberNT Update's own */
        else
            return WUSA_E_USAGE;
    }
    if (restartModes > 1)
        return WUSA_E_USAGE;
    if (haveKb && !uninstall)
        return WUSA_E_USAGE;
    if (uninstall)
    {
        if (haveExtract || (!out->hasFile && !haveKb))
            return WUSA_E_USAGE;
        out->mode = WusaUninstall;
        return 0;
    }
    if (!out->hasFile)
        return WUSA_E_USAGE;
    out->mode = haveExtract ? WusaExtract : WusaInstall;
    return 0;
}

/* "  <id> <version> : <state>" lines of `libernt-update query`; the last matching line wins */
BOOL WusaFindInstalled(const char *text, const char *id, char *version, size_t cap)
{
    const char *p = text;
    size_t idLen = strlen(id);
    BOOL found = FALSE;

    while (p && *p)
    {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);

        if (len > 2 && p[0] == ' ' && p[1] == ' ' && len > 2 + idLen && !_strnicmp(p + 2, id, idLen) && p[2 + idLen] == ' ')
        {
            const char *v = p + 3 + idLen, *e = v;

            while (e < p + len && *e != ' ' && *e != '\r')
                e++;
            if (e > v && (size_t)(e - v) < cap && (size_t)(e - v) < 65)
            {
                memcpy(version, v, (size_t)(e - v));
                version[e - v] = 0;
                found = TRUE;
            }
        }
        p = eol ? eol + 1 : NULL;
    }
    return found && WusaValidToken(version);
}
