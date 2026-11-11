/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Launcher stub of a standalone update package (KBxxxx.exe, S1-PACKAGE-EXE-DESIGN.md)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The package tool appends the S1 package directory to this program (see libservicing/lnpk.h). Like the
 * Windows 7 KB executables, the stub does nothing itself: it runs
 *     %SystemRoot%\System32\wusa.exe "<its own path>" <the switches it was given>
 * and returns wusa's exit code (0 done, 3010 restart required, otherwise an error). wusa.exe reads the
 * package, asks, and hands it to LiberNT Update, which verifies the signature and stages it. The stub
 * writes no file, reads no container and verifies nothing. If wusa.exe is missing it says so and exits 1.
 */
#include <windows.h>
#include <shellapi.h>

#define STUB_E_NOWUSA 1

/* the part of the command line after the program name (quoted or not), or "" */
static const WCHAR *SkipProgramName(const WCHAR *p)
{
    if (*p == L'"')
    {
        for (p++; *p && *p != L'"'; p++)
            ;
        if (*p)
            p++;
    }
    else
    {
        while (*p && *p != L' ' && *p != L'\t')
            p++;
    }
    while (*p == L' ' || *p == L'\t')
        p++;
    return p;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdLine, int show)
{
    WCHAR self[MAX_PATH], wusa[MAX_PATH * 2], args[MAX_PATH * 3 + 64];
    const WCHAR *rest = SkipProgramName(GetCommandLineW());
    SHELLEXECUTEINFOW sei;
    DWORD code = 1;
    BOOL quiet = FALSE;

    (void)inst; (void)prev; (void)cmdLine; (void)show;
    {
        int argc, i;
        WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

        for (i = 1; argv && i < argc; i++)
        {
            if (!lstrcmpiW(argv[i], L"/quiet") || !lstrcmpiW(argv[i], L"-quiet") || !lstrcmpiW(argv[i], L"/q"))
                quiet = TRUE;
        }
    }

#ifdef LN_TESTING
    if (!GetEnvironmentVariableW(L"LNSTUB_WUSA", wusa, MAX_PATH * 2))
#endif
    {
        GetSystemDirectoryW(wusa, MAX_PATH);
        lstrcatW(wusa, L"\\wusa.exe");
    }
    if (!GetModuleFileNameW(NULL, self, MAX_PATH))
        return STUB_E_NOWUSA;
    if (GetFileAttributesW(wusa) == INVALID_FILE_ATTRIBUTES)
    {
        if (!quiet)
            MessageBoxW(NULL, L"This update needs Windows Update Standalone Installer (wusa.exe), which is not installed on this system.",
                        L"LiberNT Update", MB_ICONERROR);
        return STUB_E_NOWUSA;
    }

    wsprintfW(args, L"\"%s\" %s", self, rest);
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"open";                                  /* wusa.exe's manifest asks for elevation */
    sei.lpFile = wusa;
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess)
    {
        if (!quiet)
            MessageBoxW(NULL, L"Windows Update Standalone Installer could not be started.", L"LiberNT Update", MB_ICONERROR);
        return GetLastError() ? (int)GetLastError() : STUB_E_NOWUSA;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}
