/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Unprivileged test: the online target path (live system drive)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

/*
 * Regression for the S1 online run 1 failure: `bootstrap install --bundle`
 * against the LIVE system returned ERROR_INVALID_NAME at target init, and
 * every online destination validation would have failed the same way,
 * because the online root is the bare drive ("C:") while
 * GetVolumeInformationW/BsFinalPath demand ("C:\") or reject a drive path,
 * and the canonical-name guard is an --image-only concept (Pam review E1).
 *
 * UNELEVATED on the host: BsTargetInitOnline must now pass the volume check
 * and stop at the SeRestore privilege (ERROR_PRIVILEGE_NOT_HELD) instead of
 * ERROR_INVALID_NAME, and BsValidateTargetRelPath must accept the standard
 * online destinations against a synthetic online target.
 *
 * Build (from this directory, MinGW/Clang on PATH):
 *   clang --target=x86_64-w64-mingw32 -O1 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE -I../../../system/servicing/pending onlinetest.c ../bundle.c ../multisz.c ../reg.c ../selftest.c ../service.c ../target.c ../../../system/servicing/pending/s1core.c -mconsole -o onlinetest.exe -lmsvcrt-os -lkernel32 -lntdll -ladvapi32 -lshell32 -lole32 -luuid
 *   ./onlinetest.exe
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
    BS_TARGET Target;
    DWORD Error;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    /* 1. Online init: volume query must accept the drive root. Unprivileged
     *    it then stops at SeRestore, which is the expected wall. */
    memset(&Target, 0, sizeof(Target));
    Error = BsTargetInitOnline(&Target);
    Check(Error != ERROR_INVALID_NAME,
          "online init no longer fails with ERROR_INVALID_NAME");
    Check(Error == ERROR_PRIVILEGE_NOT_HELD || Error == ERROR_SUCCESS,
          "online init reaches the privilege gate (or succeeds when elevated)");
    if (Error == ERROR_SUCCESS)
        Check(Target.Root[1] == L':' && Target.Root[2] == 0,
              "online root is the bare drive letter");

    /* 2. Online destination validation against the live system drive. */
    memset(&Target, 0, sizeof(Target));
    Target.Root[0] = L'C';
    Target.Root[1] = L':';
    Target.Root[2] = 0;
    Target.IsImage = FALSE;

    Error = BsValidateTargetRelPath(&Target, L"Windows\\System32\\LiberNTUpdatePending.exe");
    Check(Error == ERROR_SUCCESS, "online Windows\\System32 dest accepted");
    Error = BsValidateTargetRelPath(&Target, L"LiberNT\\Servicing\\bin\\libernt-update.exe");
    Check(Error == ERROR_SUCCESS, "online LiberNT dest accepted");
    Error = BsValidateTargetRelPath(&Target, L"LiberNT\\Update\\libernt-update-ui.exe");
    Check(Error == ERROR_SUCCESS, "online UI exe dest accepted");
    Error = BsValidateTargetRelPath(&Target, L"ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\x.lnk");
    Check(Error == ERROR_SUCCESS, "online Start Menu shortcut dest accepted");
    Error = BsValidateTargetRelPath(&Target, L"Temp\\evil.exe");
    Check(Error == ERROR_BAD_PATHNAME, "online still refuses off-allowlist prefixes");
    Error = BsValidateTargetRelPath(&Target, L"..\\Windows\\System32\\evil.exe");
    Check(Error == ERROR_BAD_PATHNAME, "online still refuses traversal");

    /* 3. The reparse walk must inspect real components: a pre-planted
     *    junction on the destination directory is refused, online and in
     *    image mode. (Pam's review: without the separator the walk was
     *    vacuous.) */
    {
        char DirA2[MAX_PATH];
        WCHAR DirW2[MAX_PATH], LinkW[MAX_PATH], TargetW[MAX_PATH], Cmd[1024];

        snprintf(DirA2, sizeof(DirA2), "%s\\s1-reparse-%lu-%lu",
                 getenv("TEMP"), (unsigned long)GetCurrentProcessId(),
                 (unsigned long)GetTickCount());
        Check(CreateDirectoryA(DirA2, NULL) != 0, "reparse fixture root created");
        mbstowcs(DirW2, DirA2, RTL_NUMBER_OF(DirW2));
        DirW2[RTL_NUMBER_OF(DirW2) - 1] = 0;
        swprintf(TargetW, RTL_NUMBER_OF(TargetW), L"%s\\attacker", DirW2);
        CreateDirectoryW(TargetW, NULL);
        swprintf(LinkW, RTL_NUMBER_OF(LinkW), L"%s\\LiberNT", DirW2);
        swprintf(Cmd, RTL_NUMBER_OF(Cmd), L"/c mklink /J \"%s\" \"%s\"", LinkW, TargetW);
        swprintf(Cmd, RTL_NUMBER_OF(Cmd), L"cmd.exe /c mklink /J \"%s\" \"%s\"", LinkW, TargetW);
        {
            STARTUPINFOW Si;
            PROCESS_INFORMATION Pi;
            DWORD MkErr = (DWORD)-1;

            memset(&Si, 0, sizeof(Si));
            Si.cb = sizeof(Si);
            memset(&Pi, 0, sizeof(Pi));
            if (CreateProcessW(NULL, Cmd, NULL, NULL, FALSE, 0, NULL, NULL, &Si, &Pi))
            {
                WaitForSingleObject(Pi.hProcess, 15000);
                GetExitCodeProcess(Pi.hProcess, &MkErr);
                CloseHandle(Pi.hProcess);
                CloseHandle(Pi.hThread);
            }
            Check(MkErr == 0, "junction fixture created (mklink /J)");
        }

        memset(&Target, 0, sizeof(Target));
        wcscpy(Target.Root, DirW2);
        Target.IsImage = FALSE;
        Error = BsValidateTargetRelPath(&Target, L"LiberNT\\Update\\x.exe");
        Check(Error == ERROR_BAD_PATHNAME, "online refuses a junction on the destination dir");

        Target.IsImage = TRUE;
        Error = BsValidateTargetRelPath(&Target, L"LiberNT\\Update\\x.exe");
        Check(Error == ERROR_BAD_PATHNAME, "image mode refuses the same junction");

        /* the pre-existing plain tree still validates, both modes */
        RemoveDirectoryW(LinkW);
        swprintf(LinkW, RTL_NUMBER_OF(LinkW), L"%s\\LiberNT", DirW2);
        CreateDirectoryW(LinkW, NULL);
        swprintf(TargetW, RTL_NUMBER_OF(TargetW), L"%s\\LiberNT\\Update", DirW2);
        CreateDirectoryW(TargetW, NULL);
        Target.IsImage = FALSE;
        Error = BsValidateTargetRelPath(&Target, L"LiberNT\\Update\\x.exe");
        Check(Error == ERROR_SUCCESS, "online accepts the plain nested tree");
        Target.IsImage = TRUE;
        Error = BsValidateTargetRelPath(&Target, L"LiberNT\\Update\\x.exe");
        Check(Error == ERROR_SUCCESS, "image mode accepts the plain nested tree (canonical compare)");

        /* final-component file symlink: refused when the privilege allows
         * creating one, otherwise skipped */
        {
            WCHAR SymW[MAX_PATH], RealW[MAX_PATH];

            swprintf(SymW, RTL_NUMBER_OF(SymW), L"%s\\LiberNT\\Update\\x.exe", DirW2);
            swprintf(RealW, RTL_NUMBER_OF(RealW), L"%s\\attacker\\planted.exe", DirW2);
            {
                HANDLE H = CreateFileW(RealW, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, NULL);
                if (H != INVALID_HANDLE_VALUE)
                    CloseHandle(H);
            }
            if (CreateSymbolicLinkW(SymW, RealW, 0))
            {
                Error = BsValidateTargetRelPath(&Target, L"LiberNT\\Update\\x.exe");
                Check(Error == ERROR_BAD_PATHNAME, "image mode refuses a final file symlink");
            }
            else
            {
                printf("skip: file-symlink case needs SeCreateSymbolicLinkPrivilege\n");
            }
        }

        /* cleanup */
        {
            WCHAR DelW[MAX_PATH * 2];
            static const WCHAR *Rel[] = { L"\\LiberNT\\Update\\x.exe",
                                          L"\\LiberNT\\Update",
                                          L"\\LiberNT",
                                          L"\\attacker\\planted.exe",
                                          L"\\attacker" };
            size_t k;

            for (k = 0; k < RTL_NUMBER_OF(Rel); k++)
            {
                swprintf(DelW, RTL_NUMBER_OF(DelW), L"%s%s", DirW2, Rel[k]);
                DeleteFileW(DelW);
                RemoveDirectoryW(DelW);
            }
            RemoveDirectoryW(DirW2);
        }
    }

    printf("%s\n", Failures ? "ONLINE TARGET TEST FAILED" : "ONLINE TARGET TEST PASSED");
    return Failures ? 1 : 0;
}
