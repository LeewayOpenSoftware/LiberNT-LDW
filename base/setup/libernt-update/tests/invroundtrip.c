/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     UNELEVATED host round-trip test for the installed.json inventory
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

/*
 * UNELEVATED regression test for the installed.json writer/reader pair.
 *
 * Reproduces the defect where BsInventorySave embedded recorded paths into
 * the JSON without escaping backslashes, so BsInventoryLoad rejected the
 * very file install wrote (0x000000b) and uninstall refused to run. Runs
 * against a plain temp directory: no DACL, hive or elevation needed.
 *
 * Build (from this directory, with a MinGW/Clang toolchain on PATH):
 *   clang --target=x86_64-w64-mingw32 -O1 -Wall -Werror -c ../main.c -Dmain=LbtBootstrapMain -o main_lt.o -I../../../system/servicing/pending
 *   clang --target=x86_64-w64-mingw32 -O1 -Wall -Werror invroundtrip.c ../bundle.c ../multisz.c ../reg.c ../selftest.c ../service.c ../target.c ../../../system/servicing/pending/s1core.c main_lt.o -mconsole -o invroundtrip.exe -lmsvcrt-os -lkernel32 -lntdll -ladvapi32 -lshell32
 *   ./invroundtrip.exe
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../bootstrap.h"

static int Failures;

static void
Check(int Cond, const char *Msg)
{
    if (Cond)
        printf("ok:   %s\n", Msg);
    else
    {
        printf("FAIL: %s\n", Msg);
        Failures++;
    }
}

int
main(void)
{
    char DirA[MAX_PATH];
    WCHAR DirW[MAX_PATH];
    BS_TARGET Target;
    BS_INVENTORY In, Out;
    DWORD Error;
    char HexA[65];
    HANDLE Handle;
    DWORD Read;
    char FileBuf[4096];
    int i;

    snprintf(DirA, sizeof(DirA), "%s\\s1-invrt-%lu-%lu",
             getenv("TEMP"), (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetTickCount());
    if (!CreateDirectoryA(DirA, NULL))
    {
        printf("FAIL: cannot create %s\n", DirA);
        return 1;
    }
    mbstowcs(DirW, DirA, RTL_NUMBER_OF(DirW));
    DirW[RTL_NUMBER_OF(DirW) - 1] = 0;

    memset(&Target, 0, sizeof(Target));
    wcscpy(Target.Root, DirW);
    Target.IsImage = TRUE;

    CreateDirectoryW(DirW, NULL);
    {
        static const WCHAR *Chain[] = { L"\\LiberNT", L"\\LiberNT\\Servicing",
                                        L"\\LiberNT\\Servicing\\bootstrap" };
        WCHAR Mk[BS_MAX_PATH_BUF];
        size_t c;

        for (c = 0; c < RTL_NUMBER_OF(Chain); c++)
        {
            swprintf(Mk, RTL_NUMBER_OF(Mk), L"%s%s", DirW, Chain[c]);
            CreateDirectoryW(Mk, NULL);
        }
    }

    memset(&In, 0, sizeof(In));
    wcscpy(In.ServiceState, L"stub");
    In.BootExecute = TRUE;
    for (i = 0; i < 64; i++)
        HexA[i] = "0123456789abcdef"[i & 15];
    HexA[64] = 0;
    strcpy(In.ManifestDigest, HexA);
    wcscpy(In.Files[0].Path, L"Windows\\System32\\LiberNTUpdatePending.exe");
    strcpy(In.Files[0].Sha256, HexA);
    wcscpy(In.Files[1].Path, L"LiberNT\\Servicing\\bin\\libernt-update.exe");
    strcpy(In.Files[1].Sha256, HexA);
    wcscpy(In.Files[2].Path, L"Windows\\System32\\quo\"te\x01.dll");
    strcpy(In.Files[2].Sha256, HexA);
    In.FileCount = 3;

    Error = BsInventorySave(&Target, &In);
    Check(Error == ERROR_SUCCESS, "BsInventorySave exit 0");
    if (Error != ERROR_SUCCESS)
        printf("      save error 0x%08lx\n", (unsigned long)Error);

    /* The file on disk must carry escaped backslashes only: a raw \S is
     * the invalid escape that made BsInventoryLoad fail the parse. */
    {
        WCHAR JsonW[BS_MAX_PATH_BUF];
        swprintf(JsonW, RTL_NUMBER_OF(JsonW), L"%s\\%s", DirW, BS_INSTALLED_JSON);
        Handle = CreateFileW(JsonW, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    Check(Handle != INVALID_HANDLE_VALUE, "installed.json exists on disk");
    if (Handle != INVALID_HANDLE_VALUE)
    {
        if (ReadFile(Handle, FileBuf, sizeof(FileBuf) - 1, &Read, NULL))
        {
            char *Cursor;

            FileBuf[Read] = 0;
            Check(strstr(FileBuf, "Windows\\\\System32") != NULL,
                  "on-disk JSON escapes the path backslashes");
            Cursor = FileBuf;
            while ((Cursor = strchr(Cursor, '\\')) != NULL)
            {
                if (Cursor[1] != '"' && Cursor[1] != '\\' && Cursor[1] != '/' &&
                    Cursor[1] != 'b' && Cursor[1] != 'f' && Cursor[1] != 'n' &&
                    Cursor[1] != 'r' && Cursor[1] != 't' && Cursor[1] != 'u')
                    break;
                Cursor += 2;
            }
            Check(Cursor == NULL, "on-disk JSON has no invalid raw escapes");
        }
        else
        {
            Check(0, "installed.json read back");
        }
        CloseHandle(Handle);
    }

    memset(&Out, 0, sizeof(Out));
    Error = BsInventoryLoad(&Target, &Out);
    Check(Error == ERROR_SUCCESS, "BsInventoryLoad exit 0");
    if (Error != ERROR_SUCCESS)
        printf("      load error 0x%08lx\n", (unsigned long)Error);
    if (Error == ERROR_SUCCESS)
    {
        Check(Out.FileCount == 3, "file count round-trips");
        Check(wcscmp(Out.ServiceState, L"stub") == 0, "service state round-trips");
        Check(Out.BootExecute == TRUE, "bootExecute=true round-trips");
        Check(strcmp(Out.ManifestDigest, HexA) == 0, "manifest digest round-trips");
        Check(wcscmp(Out.Files[0].Path, L"Windows\\System32\\LiberNTUpdatePending.exe") == 0,
              "backslash path 1 round-trips");
        Check(wcscmp(Out.Files[1].Path, L"LiberNT\\Servicing\\bin\\libernt-update.exe") == 0,
              "backslash path 2 round-trips");
        Check(wcscmp(Out.Files[2].Path, L"Windows\\System32\\quo\"te\x01.dll") == 0,
              "quote and control-char path round-trips");
        Check(strcmp(Out.Files[0].Sha256, HexA) == 0, "sha256 round-trips");
    }

    {
        WCHAR DelW[BS_MAX_PATH_BUF];

        swprintf(DelW, RTL_NUMBER_OF(DelW), L"%s\\%s", DirW, BS_INSTALLED_JSON);
        DeleteFileW(DelW);
        swprintf(DelW, RTL_NUMBER_OF(DelW), L"%s\\LiberNT\\Servicing\\bootstrap", DirW);
        RemoveDirectoryW(DelW);
        swprintf(DelW, RTL_NUMBER_OF(DelW), L"%s\\LiberNT\\Servicing", DirW);
        RemoveDirectoryW(DelW);
        swprintf(DelW, RTL_NUMBER_OF(DelW), L"%s\\LiberNT", DirW);
        RemoveDirectoryW(DelW);
        RemoveDirectoryW(DirW);
    }

    printf("%s\n", Failures ? "ROUND-TRIP TEST FAILED" : "ROUND-TRIP TEST PASSED");
    return Failures ? 1 : 0;
}
