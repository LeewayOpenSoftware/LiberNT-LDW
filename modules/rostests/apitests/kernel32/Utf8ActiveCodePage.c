/*
 * PROJECT:     LiberNT kernel32 API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     activeCodePage UTF-8 in the application manifest ("Use UTF-8 code pages in
 *              Windows apps"): one child with the manifest, one without
 */

#include "precomp.h"

static
void
RunChild(PCWSTR Program, PCWSTR Mode, PCSTR What)
{
    WCHAR Self[MAX_PATH], Command[MAX_PATH + 64];
    STARTUPINFOW Startup = { sizeof(Startup) };
    PROCESS_INFORMATION Info;
    PWCHAR Slash;
    DWORD Exit = 0xDEADBEEF;

    GetModuleFileNameW(NULL, Self, ARRAYSIZE(Self));
    Slash = wcsrchr(Self, L'\\');
    if (!Slash)
    {
        skip("%s: no directory for the test program\n", What);
        return;
    }
    Slash[1] = UNICODE_NULL;
    StringCchPrintfW(Command, ARRAYSIZE(Command), L"%ls%ls %ls", Self, Program, Mode);

    if (!CreateProcessW(NULL, Command, NULL, NULL, FALSE, 0, NULL, NULL, &Startup, &Info))
    {
        skip("%s: %ls is not available (error %lu)\n", What, Program, GetLastError());
        return;
    }

    WaitForSingleObject(Info.hProcess, 30000);
    GetExitCodeProcess(Info.hProcess, &Exit);
    CloseHandle(Info.hThread);
    CloseHandle(Info.hProcess);

    /* 10 to 12: the manifest was ignored or the conversions did not follow UTF-8; 20: UTF-8 without it */
    ok(Exit == 0, "%s: the child returned %lu\n", What, Exit);
}

START_TEST(Utf8ActiveCodePage)
{
    RunChild(L"utf8acp_on.exe", L"utf8", "manifest asks for UTF-8");
    RunChild(L"utf8acp_off.exe", L"legacy", "no manifest");
}
