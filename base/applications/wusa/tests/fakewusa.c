/*
 * PROJECT:     LiberNT Update - wusa host tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     A fake wusa.exe: records the command line it got in %LNFAKE_OUT% and exits with %LNFAKE_EXIT%
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, WCHAR **argv)
{
    WCHAR out[MAX_PATH], code[16];
    FILE *f;
    int i;

    if (GetEnvironmentVariableW(L"LNFAKE_OUT", out, MAX_PATH) && (f = _wfopen(out, L"wb")) != NULL)
    {
        for (i = 1; i < argc; i++)
            fprintf(f, "[%ls]", argv[i]);
        fwprintf(f, L"\n");
        fclose(f);
    }
    return GetEnvironmentVariableW(L"LNFAKE_EXIT", code, 16) ? (int)wcstol(code, NULL, 10) : 0;
}
