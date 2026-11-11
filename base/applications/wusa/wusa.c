/*
 * PROJECT:     LiberNT Update - Windows Update Standalone Installer
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     wusa.exe: installs and removes update packages (KBxxxx.exe) like the Windows 7 tool
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * wusa is the system side of a standalone package (S1-PACKAGE-EXE-DESIGN.md, sections 2 and 6). The KB
 * stub only runs `wusa.exe "<its own path>" <switches>`; wusa reads the container for the dialog text,
 * asks, and hands the package to LiberNT Update, the same calls as `libernt-update.exe add-package --exe`
 * followed by `apply`. wusa writes no system file and verifies nothing cryptographic itself: libservicing
 * checks the manifest signature against the pinned certificate and every payload hash.
 *
 * Exit codes (as Windows): 0 done, 3010 done and a restart is required, 87 bad arguments, 11 not a package,
 * 2 LiberNT Update is not installed, 1223 the user said No, otherwise the exit code of libernt-update.exe.
 * A restart happens only with /forcerestart or after the user answers Yes; /quiet alone never restarts.
 */
#include "wusa.h"
#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

#define WUSA_TITLE L"Windows Update Standalone Installer"

static WUSA_ARGS g_args;
static HWND g_progress;

static int Say(const WCHAR *text, UINT flags)
{
    if (g_args.quiet)
        return IDYES;
    return MessageBoxW(NULL, text, WUSA_TITLE, flags);
}

/* ---- the progress window: a label and a marquee bar, shown only while LiberNT Update works ---------- */

static LRESULT CALLBACK ProgressProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE)
        return 0;                                          /* the install cannot be abandoned half way */
    return DefWindowProcW(wnd, msg, wp, lp);
}

static void ProgressShow(const WCHAR *text)
{
    WNDCLASSW wc;
    INITCOMMONCONTROLSEX icc;
    HWND label, bar;
    int w = 420, h = 110;

    if (g_args.quiet || g_progress)
        return;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = ProgressProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"LnWusaProgress";
    RegisterClassW(&wc);
    g_progress = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, WUSA_TITLE, WS_POPUP | WS_CAPTION | WS_VISIBLE,
                                 (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2,
                                 w, h, NULL, NULL, wc.hInstance, NULL);
    if (!g_progress)
        return;
    label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, 14, 12, w - 40, 20, g_progress, NULL, wc.hInstance, NULL);
    SendMessageW(label, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | 0x08 /* PBS_MARQUEE */, 14, 40, w - 44, 18,
                          g_progress, NULL, wc.hInstance, NULL);
    if (bar)
        SendMessageW(bar, WM_USER + 10 /* PBM_SETMARQUEE */, TRUE, 40);
    UpdateWindow(g_progress);
}

static void ProgressHide(void)
{
    if (g_progress)
        DestroyWindow(g_progress);
    g_progress = NULL;
}

static void Pump(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* ---- LiberNT Update ---------------------------------------------------------------------------- */

/* the system drive root with a trailing backslash; the host tests point it at an Image target instead */
static void SystemRoot(WCHAR *root)
{
    root[0] = 0;
#ifdef LN_TESTING
    if (GetEnvironmentVariableW(L"LNWUSA_IMAGE", root, MAX_PATH))
    {
        size_t n = wcslen(root);

        if (n && root[n - 1] != L'\\')
            wcscat(root, L"\\");
        return;
    }
#endif
    GetWindowsDirectoryW(root, MAX_PATH);
    root[3] = 0;
}

static BOOL UpdaterPath(WCHAR *cli, WCHAR *image)
{
    WCHAR root[MAX_PATH];

    image[0] = 0;
    SystemRoot(root);
#ifdef LN_TESTING
    if (GetEnvironmentVariableW(L"LNWUSA_UPDATER", cli, MAX_PATH))
    {
        size_t n;

        if (GetEnvironmentVariableW(L"LNWUSA_IMAGE", image, MAX_PATH) && (n = wcslen(image)) && image[n - 1] == L'\\')
            image[n - 1] = 0;
        return GetFileAttributesW(cli) != INVALID_FILE_ATTRIBUTES;
    }
#endif
    wsprintfW(cli, L"%sLiberNT\\Servicing\\bin\\libernt-update.exe", root);
    return GetFileAttributesW(cli) != INVALID_FILE_ATTRIBUTES;
}

/* run libernt-update.exe with args; the exit code comes back, its stdout in *out when out != NULL */
static DWORD RunUpdater(const WCHAR *cli, const WCHAR *image, const WCHAR *args, char **out)
{
    WCHAR cmd[MAX_PATH * 3 + 64];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL, wr = NULL;
    DWORD code = 1, n, cap = 0, len = 0;
    char *buf = NULL;

    if (image[0])
        wsprintfW(cmd, L"\"%s\" --image \"%s\" %s", cli, image, args);
    else
        wsprintfW(cmd, L"\"%s\" %s", cli, args);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (out)
    {
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = NULL;
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&rd, &wr, &sa, 0))
            return GetLastError();
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError = wr;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        *out = NULL;
    }
    if (!CreateProcessW(NULL, cmd, NULL, NULL, out != NULL, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        code = GetLastError() ? GetLastError() : 1;
        if (rd)
            CloseHandle(rd);
        if (wr)
            CloseHandle(wr);
        return code;
    }
    if (wr)
        CloseHandle(wr);
    if (out)
    {
        char tmp[4096];

        while (ReadFile(rd, tmp, sizeof(tmp), &n, NULL) && n)
        {
            if (len + n + 1 > cap)
            {
                char *nb = HeapAlloc(GetProcessHeap(), 0, cap = (len + n + 1) * 2);

                if (!nb)
                    break;
                if (buf)
                {
                    memcpy(nb, buf, len);
                    HeapFree(GetProcessHeap(), 0, buf);
                }
                buf = nb;
            }
            memcpy(buf + len, tmp, n);
            len += n;
        }
        if (buf)
            buf[len] = 0;
        CloseHandle(rd);
        *out = buf;
    }
    for (;;)
    {
        DWORD r = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, INFINITE, QS_ALLINPUT);

        if (r == 1 /* the message queue */)
            Pump();
        else
            break;
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

static void RestartNow(void)
{
#ifndef LN_TESTING                                         /* the host tests never restart the machine */
    HANDLE tok;
    TOKEN_PRIVILEGES tp;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
    {
        LookupPrivilegeValueW(NULL, L"SeShutdownPrivilege", &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        CloseHandle(tok);
    }
    ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_MINOR_UPGRADE | SHTDN_REASON_FLAG_PLANNED);
#endif
}

static void Describe(const WUSA_IDENT *id, WCHAR *text, size_t cap, const WCHAR *question)
{
    WCHAR head[300], desc[420] = L"";

    if (id->title[0])
        wsprintfW(head, L"%S (%S)", id->title, id->kb[0] ? id->kb : id->id);
    else
        wsprintfW(head, L"%S %S", id->kb[0] ? id->kb : id->id, id->version);
    if (id->description[0])
        wsprintfW(desc, L"\n\n%S", id->description);
    _snwprintf(text, cap, L"%s\n\n%s%s", question, head, desc);
    text[cap - 1] = 0;
}

/* the update finished and a restart is needed to complete it: ask, restart or just report 3010 */
static int RestartNeeded(void)
{
    if (g_args.norestart)
        return WUSA_REBOOT;
    if (g_args.forcerestart ||
        (!g_args.quiet && Say(L"The update was installed. Restart the computer now to finish it?", MB_ICONQUESTION | MB_YESNO) == IDYES))
        RestartNow();
    return WUSA_REBOOT;
}

static BOOL PlanPending(void)
{
    WCHAR root[MAX_PATH], plan[MAX_PATH * 2];

    SystemRoot(root);
    wsprintfW(plan, L"%sLiberNT\\Servicing\\pending\\plan.json", root);
    return GetFileAttributesW(plan) != INVALID_FILE_ATTRIBUTES;
}

static int DoInstall(const WUSA_IDENT *id, const WCHAR *cli, const WCHAR *image)
{
    WCHAR text[1100], args[MAX_PATH * 2 + 64];
    DWORD code;

    Describe(id, text, ARRAYSIZE(text), L"Do you want to install the following update for LiberNT?");
    if (Say(text, MB_ICONQUESTION | MB_YESNO) != IDYES)
        return ERROR_CANCELLED;
    ProgressShow(L"Installing the update...");
    wsprintfW(args, L"add-package --exe \"%s\"", g_args.file);
    code = RunUpdater(cli, image, args, NULL);
    if (code)
    {
        ProgressHide();
        Say(L"The update was refused: it is not signed for this system, does not apply to it, or is damaged.", MB_ICONERROR);
        return (int)code;
    }
    wsprintfW(args, L"apply %S %S", id->id, id->version);
    code = RunUpdater(cli, image, args, NULL);
    ProgressHide();
    if (code)
    {
        Say(L"The update could not be installed; see the LiberNT Update log.", MB_ICONERROR);
        return (int)code;
    }
    if (PlanPending())
        return RestartNeeded();
    Say(L"The update was installed.", MB_ICONINFORMATION);
    return 0;
}

/* id/version are known from the package file, or looked up by KB number in `libernt-update query` */
static int DoUninstall(const WCHAR *cli, const WCHAR *image, const char *id, const char *version, const WCHAR *label)
{
    WCHAR text[600], args[256];
    char idBuf[80], verBuf[80];
    DWORD code;

    if (id)
    {
        strcpy(idBuf, id);
        strcpy(verBuf, version);
    }
    else
    {
        char *query = NULL;

        wsprintfA(idBuf, "%S", g_args.kb);
        code = RunUpdater(cli, image, L"query", &query);
        if (code || !query || !WusaFindInstalled(query, idBuf, verBuf, sizeof(verBuf)))
        {
            if (query)
                HeapFree(GetProcessHeap(), 0, query);
            Say(L"The specified update is not installed on this system.", MB_ICONINFORMATION);
            return ERROR_NOT_FOUND;
        }
        HeapFree(GetProcessHeap(), 0, query);
    }
    wsprintfW(text, L"Do you want to uninstall the following update?\n\n%s", label);
    if (Say(text, MB_ICONQUESTION | MB_YESNO) != IDYES)
        return ERROR_CANCELLED;
    ProgressShow(L"Uninstalling the update...");
    wsprintfW(args, L"remove-package %S %S", idBuf, verBuf);
    code = RunUpdater(cli, image, args, NULL);
    ProgressHide();
    if (code)
    {
        Say(L"The update could not be removed; see the LiberNT Update log.", MB_ICONERROR);
        return (int)code;
    }
    if (PlanPending())
        return RestartNeeded();
    Say(L"The update was uninstalled.", MB_ICONINFORMATION);
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdLine, int show)
{
    int argc, rc, i;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    WCHAR cli[MAX_PATH], image[MAX_PATH], label[300];
    WUSA_IDENT ident;
    DWORD err;

    (void)inst; (void)prev; (void)cmdLine; (void)show;
    memset(&ident, 0, sizeof(ident));
    if (!argv || WusaParseArgs(argc, argv, &g_args) != 0)
    {
        /* an explicit /quiet in a rejected command line still silences the usage box */
        for (i = 1; argv && i < argc; i++)
        {
            if (!_wcsicmp(argv[i], L"/quiet") || !_wcsicmp(argv[i], L"-quiet") || !_wcsicmp(argv[i], L"/q"))
                g_args.quiet = TRUE;
        }
        Say(L"Usage:\n  wusa <update.exe> [/quiet] [/norestart | /forcerestart | /promptrestart]\n"
            L"  wusa <update.exe> /extract:<folder>\n  wusa /uninstall <update.exe> | /kb:<number> [/quiet] [/norestart]",
            MB_ICONINFORMATION);
        return WUSA_E_USAGE;
    }

    if (g_args.hasFile)
    {
        WCHAR full[MAX_PATH];

        if (GetFullPathNameW(g_args.file, MAX_PATH, full, NULL))
            wcscpy(g_args.file, full);
        if (!WusaOpenContainer(g_args.file) || !WusaReadIdentity(&ident))
        {
            Say(L"This file does not carry a LiberNT update package.", MB_ICONERROR);
            return WUSA_E_NOTPACKAGE;
        }
    }
    if (g_args.mode == WusaExtract)
    {
        err = WusaExtractContainer(g_args.extractDir);
        WusaCloseContainer();
        return (int)err;
    }
    if (!UpdaterPath(cli, image))
    {
        Say(L"LiberNT Update is not installed on this system, so updates cannot be installed or removed.", MB_ICONERROR);
        return WUSA_E_NOUPDATER;
    }

    if (g_args.mode == WusaInstall)
        rc = DoInstall(&ident, cli, image);
    else if (g_args.hasFile)
    {
        Describe(&ident, label, ARRAYSIZE(label), L"");
        rc = DoUninstall(cli, image, ident.id, ident.version, label + 2);
    }
    else
        rc = DoUninstall(cli, image, NULL, NULL, g_args.kb);
    WusaCloseContainer();
    return rc;
}
