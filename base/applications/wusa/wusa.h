/*
 * PROJECT:     LiberNT Update - Windows Update Standalone Installer
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Shared declarations of wusa.exe (S1-PACKAGE-EXE-DESIGN.md section 6)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#pragma once
#define WIN32_NO_STATUS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#include <stddef.h>

#define WUSA_E_USAGE      ERROR_INVALID_PARAMETER     /* 87, what the Windows tool reports for bad arguments */
#define WUSA_E_NOTPACKAGE ERROR_BAD_FORMAT            /* 11: the file carries no LiberNT Update package */
#define WUSA_E_NOUPDATER  ERROR_FILE_NOT_FOUND        /* 2: LiberNT Update is not installed */
#define WUSA_REBOOT       ERROR_SUCCESS_REBOOT_REQUIRED /* 3010 */

typedef enum _WUSA_MODE
{
    WusaInstall,
    WusaUninstall,
    WusaExtract
} WUSA_MODE;

typedef struct _WUSA_ARGS
{
    WUSA_MODE mode;
    BOOL quiet;
    BOOL norestart;                 /* never restart, never ask */
    BOOL forcerestart;              /* restart without asking when one is needed */
    BOOL hasFile;
    WCHAR file[MAX_PATH];
    WCHAR kb[24];                   /* normalized "KB<digits>" of /kb:, or empty */
    WCHAR extractDir[MAX_PATH];
} WUSA_ARGS;

/* args.c: pure functions, unit-tested on the host (tests/wusa-tests.c) */
DWORD WusaParseArgs(int argc, WCHAR **argv, WUSA_ARGS *out);                 /* 0 or WUSA_E_USAGE */
BOOL WusaNormalizeKb(const WCHAR *in, WCHAR *out, size_t cap);               /* "123" / "kb123" -> "KB123" */
BOOL WusaValidToken(const char *s);                                          /* [A-Za-z0-9._-]{1,64}: ids and versions that go on a command line */
BOOL WusaFindInstalled(const char *queryText, const char *id, char *version, size_t cap); /* "libernt-update query" output */

/* lnpkview.c: read-only view of the container appended to a package .exe (lnpk.h) */
typedef struct _WUSA_IDENT
{
    char id[80], version[80], kb[24], title[200], description[400];
} WUSA_IDENT;

BOOL WusaOpenContainer(const WCHAR *path);
BOOL WusaReadIdentity(WUSA_IDENT *ident);                                    /* id + version required; kb/title/description optional */
DWORD WusaExtractContainer(const WCHAR *dir);                                /* 0, WUSA_E_NOTPACKAGE or a Win32 error */
void WusaCloseContainer(void);
