/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Unprivileged test: BsInstallShortcuts writes a .lnk whose
 *              destination is the fully-qualified target exe
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

/*
 * Regression for Pam's review of ae40ee1347: the shortcut target used to be
 * built as Root+Target without a separator; this tree's BS_TARGET.Root has
 * no trailing backslash ('C:' online, stripped for images), so the .lnk
 * destination came out drive-relative ('C:LiberNT\...').
 *
 * Build (from this directory, MinGW/Clang on PATH):
 *   clang --target=x86_64-w64-mingw32 -O1 -Wall -Wextra -DUNICODE -D_UNICODE -I../../../system/servicing/pending shortcuttest.c ../bundle.c ../multisz.c ../reg.c ../selftest.c ../service.c ../target.c ../../../system/servicing/pending/s1core.c -mconsole -o shortcuttest.exe -lmsvcrt-os -lkernel32 -lntdll -ladvapi32 -lshell32 -lole32 -luuid
 *   ./shortcuttest.exe
 */

#define INITGUID
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>

#include "../bootstrap.h"
#define main bootstrap_main
#include "../main.c"
#undef main

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
    BS_BUNDLE Bundle;
    BS_INVENTORY Inventory;
    DWORD Error;
    WCHAR LinkPath[MAX_PATH * 2];
    IShellLinkW *Link = NULL;
    IPersistFile *Persist = NULL;
    WCHAR GotPath[MAX_PATH * 2];
    WIN32_FIND_DATAW Find;
    HANDLE FindHandle;

    snprintf(DirA, sizeof(DirA), "%s\\s1-shortcut-%lu-%lu",
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

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    memset(&Bundle, 0, sizeof(Bundle));
    wcscpy(Bundle.Shortcuts[0].Name, L"ln-shortcut-test.lnk");
    wcscpy(Bundle.Shortcuts[0].Target, L"LiberNT\\Update\\libernt-update-ui.exe");
    wcscpy(Bundle.Shortcuts[0].Arguments, L"/query");
    Bundle.ShortcutCount = 1;

    memset(&Inventory, 0, sizeof(Inventory));
    Error = BsInstallShortcuts(&Target, &Bundle, &Inventory);
    Check(Error == ERROR_SUCCESS, "BsInstallShortcuts exit 0");
    if (Error != ERROR_SUCCESS)
    {
        printf("      error 0x%08lx\n", (unsigned long)Error);
        printf("SHORTCUT TEST FAILED\n");
        return 1;
    }

    swprintf(LinkPath, RTL_NUMBER_OF(LinkPath),
             L"%s\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\ln-shortcut-test.lnk",
             DirW);
    Check(GetFileAttributesW(LinkPath) != INVALID_FILE_ATTRIBUTES,
          ".lnk exists under Programs");

    Check(Inventory.FileCount == 1, "inventory recorded the shortcut");
    if (Inventory.FileCount == 1)
    {
        Check(wcscmp(Inventory.Files[0].Path,
                     L"ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\ln-shortcut-test.lnk") == 0,
              "inventory path is Programs-relative");
        Check(Inventory.Files[0].Sha256[0] != 0, "inventory carries the .lnk hash");
    }

    /* The fix under test: the .lnk destination must be fully qualified. */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IShellLinkW, (void **)&Link)))
    {
        if (SUCCEEDED(Link->lpVtbl->QueryInterface(Link, &IID_IPersistFile,
                                                   (void **)&Persist)))
        {
            if (SUCCEEDED(Persist->lpVtbl->Load(Persist, LinkPath, STGM_READ)))
            {
                if (SUCCEEDED(Link->lpVtbl->GetPath(Link, GotPath, MAX_PATH * 2,
                                                    NULL, SLGP_RAWPATH)))
                {
                    WCHAR Expected[MAX_PATH * 2];

                    swprintf(Expected, RTL_NUMBER_OF(Expected),
                             L"%s\\LiberNT\\Update\\libernt-update-ui.exe", DirW);
                    Check(wcscmp(GotPath, Expected) == 0,
                          ".lnk destination is the fully-qualified exe");
                    if (wcscmp(GotPath, Expected) != 0)
                        wprintf(L"      got %ls, want %ls\n", GotPath, Expected);
                }
                else
                {
                    Check(0, "IShellLinkW::GetPath");
                }
            }
            else
            {
                Check(0, "IPersistFile::Load");
            }
            Persist->lpVtbl->Release(Persist);
        }
        else
        {
            Check(0, "QueryInterface(IPersistFile)");
        }
        Link->lpVtbl->Release(Link);
    }
    else
    {
        Check(0, "CoCreateInstance(CLSID_ShellLink)");
    }

    /* cleanup */
    DeleteFileW(LinkPath);
    {
        WCHAR Del[MAX_PATH * 2], Pattern[MAX_PATH * 2];

        swprintf(Del, RTL_NUMBER_OF(Del),
                 L"%s\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs", DirW);
        RemoveDirectoryW(Del);
        swprintf(Del, RTL_NUMBER_OF(Del),
                 L"%s\\ProgramData\\Microsoft\\Windows\\Start Menu", DirW);
        RemoveDirectoryW(Del);
        swprintf(Del, RTL_NUMBER_OF(Del), L"%s\\ProgramData\\Microsoft\\Windows", DirW);
        RemoveDirectoryW(Del);
        swprintf(Del, RTL_NUMBER_OF(Del), L"%s\\ProgramData\\Microsoft", DirW);
        RemoveDirectoryW(Del);
        swprintf(Del, RTL_NUMBER_OF(Del), L"%s\\ProgramData", DirW);
        RemoveDirectoryW(Del);
        swprintf(Pattern, RTL_NUMBER_OF(Pattern), L"%s\\*.*", DirW);
        FindHandle = FindFirstFileW(Pattern, &Find);
        if (FindHandle != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (wcscmp(Find.cFileName, L".") && wcscmp(Find.cFileName, L".."))
                {
                    WCHAR Sub[MAX_PATH * 2];
                    swprintf(Sub, RTL_NUMBER_OF(Sub), L"%s\\%s", DirW, Find.cFileName);
                    DeleteFileW(Sub);
                    RemoveDirectoryW(Sub);
                }
            } while (FindNextFileW(FindHandle, &Find));
            FindClose(FindHandle);
        }
        RemoveDirectoryW(DirW);
    }

    printf("%s\n", Failures ? "SHORTCUT TEST FAILED" : "SHORTCUT TEST PASSED");
    return Failures ? 1 : 0;
}
