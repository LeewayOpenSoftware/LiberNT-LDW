/*
 * PROJECT:     LiberNT Windows Start-Up Application
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Session 0 start-up: window station, LSA and service control manager
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <stdarg.h>
#include <stdlib.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <winreg.h>
#include <wincon.h>
#include <sddl.h>
#include <ndk/rtlfuncs.h>
#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(wininit);

static HANDLE ShutdownRequest;
static HANDLE ShutdownComplete;
static HANDLE ServicesProcess;

static
DWORD
GetWaitToKillServiceTimeout(VOID)
{
    WCHAR Value[16];
    DWORD Size = sizeof(Value);

    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SYSTEM\\CurrentControlSet\\Control",
                     L"WaitToKillServiceTimeout",
                     RRF_RT_REG_SZ,
                     NULL,
                     Value,
                     &Size) != ERROR_SUCCESS)
    {
        return 20000;
    }
    return wcstoul(Value, NULL, 10);
}

static
DWORD
WINAPI
ShutdownThread(
    _In_ PVOID Parameter)
{
    LPTHREAD_START_ROUTINE CtrlRoutine;
    HANDLE Thread;

    UNREFERENCED_PARAMETER(Parameter);

    for (;;)
    {
        WaitForSingleObject(ShutdownRequest, INFINITE);

        CtrlRoutine = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CtrlRoutine");
        Thread = CtrlRoutine ? CreateRemoteThread(ServicesProcess,
                                                  NULL,
                                                  0,
                                                  CtrlRoutine,
                                                  (PVOID)(ULONG_PTR)CTRL_SHUTDOWN_EVENT,
                                                  0,
                                                  NULL)
                             : NULL;
        if (Thread)
        {
            WaitForSingleObject(Thread, GetWaitToKillServiceTimeout());
            CloseHandle(Thread);
        }
        else
        {
            ERR("WININIT: Could not notify the service control manager (error %lu)\n", GetLastError());
        }

        SetEvent(ShutdownComplete);
    }
    return 0;
}

static
BOOL
CreateShutdownEvents(VOID)
{
    SECURITY_ATTRIBUTES Attributes = { sizeof(Attributes), NULL, FALSE };
    HANDLE Thread;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)",
                                                              SDDL_REVISION_1,
                                                              &Attributes.lpSecurityDescriptor,
                                                              NULL))
    {
        return FALSE;
    }

    ShutdownRequest = CreateEventW(&Attributes, FALSE, FALSE, L"WininitShutdownRequest");
    ShutdownComplete = CreateEventW(&Attributes, FALSE, FALSE, L"WininitShutdownComplete");
    LocalFree(Attributes.lpSecurityDescriptor);
    if (!ShutdownRequest || !ShutdownComplete)
        return FALSE;

    Thread = CreateThread(NULL, 0, ShutdownThread, NULL, 0, NULL);
    if (!Thread)
        return FALSE;
    CloseHandle(Thread);
    return TRUE;
}

/* Session notifications come from win32k, which this session already runs, so services may register them at once. */
static
HANDLE
CreateTermSrvReadyEvent(VOID)
{
    SECURITY_ATTRIBUTES Attributes = { sizeof(Attributes), NULL, FALSE };
    HANDLE Event;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:SYD:(A;;0x100000;;;WD)(A;;0x100000;;;AC)"
            L"(A;;0x100000;;;S-1-15-3-1024-1864111754-776273317-3666925027-2523908081-3792458206-3582472437-4114419977-1582884857)"
            L"(A;;0x2;;;SY)",
            SDDL_REVISION_1,
            &Attributes.lpSecurityDescriptor,
            NULL))
    {
        return NULL;
    }

    Event = CreateEventW(&Attributes, TRUE, TRUE, L"Global\\TermSrvReadyEvent");
    LocalFree(Attributes.lpSecurityDescriptor);
    return Event;
}

static
/* ---- LiberNT Update fallback boot (S1-GROUP3-DESIGN.md) --------------------------------------------
 * services.exe and lsass.exe are swapped on disk by a boot-class update, the previous file kept beside
 * them as <stem>.old. On a fallback boot (SystemStartOptions names a *.old kernel, HAL or bootvid) the
 * files listed in LiberNT\Servicing\pending\bootstate ("ARMED", then "old <path>" lines) start from the
 * pre-image. smss and the kernel do the same for the processes and modules they start. */
#define S1_MAX_OLD 16
#define S1_NAME_CCH 64
static WCHAR S1Old[S1_MAX_OLD][S1_NAME_CCH];
static DWORD S1OldCount;
static BOOL S1Checked;

static BOOL
S1OptionNamesOld(
    _In_ PCWSTR Options,
    _In_ PCWSTR Key)
{
    SIZE_T KeyLength = wcslen(Key);
    PCWSTR p = Options;

    while (*p)
    {
        if ((p == Options || p[-1] == L' ' || p[-1] == L'\t') && _wcsnicmp(p, Key, KeyLength) == 0)
        {
            PCWSTR Value = p + KeyLength;
            SIZE_T Length = 0;

            while (Value[Length] && Value[Length] != L' ' && Value[Length] != L'\t')
                Length++;
            if (Length > 4 && _wcsnicmp(Value + Length - 4, L".old", 4) == 0)
                return TRUE;
        }
        p++;
    }
    return FALSE;
}

static VOID
S1Init(VOID)
{
    HKEY Key;
    WCHAR Options[1024], Path[MAX_PATH];
    DWORD Size = sizeof(Options) - sizeof(WCHAR), Type = 0, Read = 0;
    HANDLE File;
    CHAR *Buffer, *Line, *Next;

    S1Checked = TRUE;
    S1OldCount = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control", 0, KEY_QUERY_VALUE, &Key) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExW(Key, L"SystemStartOptions", NULL, &Type, (LPBYTE)Options, &Size) != ERROR_SUCCESS || Type != REG_SZ)
    {
        RegCloseKey(Key);
        return;
    }
    RegCloseKey(Key);
    Options[Size / sizeof(WCHAR)] = UNICODE_NULL;
    if (!S1OptionNamesOld(Options, L"/KERNEL=") && !S1OptionNamesOld(Options, L"/HAL=") &&
        !S1OptionNamesOld(Options, L"/BOOTVID="))
        return;
    if (!GetSystemWindowsDirectoryW(Path, MAX_PATH) || Path[1] != L':')
        return;
    Path[3] = UNICODE_NULL;
    wcscat(Path, L"LiberNT\\Servicing\\pending\\bootstate");
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return;
    Buffer = HeapAlloc(GetProcessHeap(), 0, 4097);
    if (Buffer && ReadFile(File, Buffer, 4096, &Read, NULL) && Read >= 5 && memcmp(Buffer, "ARMED", 5) == 0)
    {
        Buffer[Read] = 0;
        for (Line = Buffer; Line && *Line; Line = Next)
        {
            PCSTR Base;
            CHAR *p;
            DWORD k;

            Next = strchr(Line, '\n');
            if (Next)
                *Next++ = 0;
            for (p = Line + strlen(Line); p > Line && (p[-1] == '\r' || p[-1] == ' '); p--)
                p[-1] = 0;
            if (_strnicmp(Line, "old ", 4) != 0)
                continue;
            Base = Line + 4;
            for (p = Line + 4; *p; p++)
            {
                if (*p == '\\' || *p == '/')
                    Base = p + 1;
            }
            if (!*Base || strlen(Base) >= S1_NAME_CCH || S1OldCount >= S1_MAX_OLD)
                continue;
            for (k = 0; Base[k]; k++)
                S1Old[S1OldCount][k] = (WCHAR)(UCHAR)Base[k];
            S1Old[S1OldCount][k] = UNICODE_NULL;
            S1OldCount++;
        }
    }
    if (Buffer)
        HeapFree(GetProcessHeap(), 0, Buffer);
    CloseHandle(File);
}

/* ImageName listed and <SystemRoot>\System32\<stem>.old present: OldPath gets that full path */
static BOOL
S1OldImage(
    _In_ PCWSTR ImageName,
    _Out_writes_(MAX_PATH) PWSTR OldPath)
{
    DWORD i;
    PWSTR Dot;

    if (!S1Checked)
        S1Init();
    for (i = 0; i < S1OldCount; i++)
    {
        if (_wcsicmp(S1Old[i], ImageName) == 0)
            break;
    }
    if (i == S1OldCount)
        return FALSE;
    if (!GetSystemDirectoryW(OldPath, MAX_PATH - 70))
        return FALSE;
    wcscat(OldPath, L"\\");
    wcscat(OldPath, ImageName);
    Dot = wcsrchr(OldPath, L'.');
    if (Dot)
        *Dot = UNICODE_NULL;
    wcscat(OldPath, L".old");
    if (GetFileAttributesW(OldPath) == INVALID_FILE_ATTRIBUTES)
    {
        ERR("WININIT: fallback boot, %ls has no .old pre-image, starting it as named\n", ImageName);
        return FALSE;
    }
    ERR("WININIT: fallback boot, starting %ls instead of %ls\n", OldPath, ImageName);
    return TRUE;
}

HANDLE
StartSystemProcess(
    _In_ PCWSTR ImageName)
{
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInformation;
    WCHAR OldPath[MAX_PATH];

    if (S1OldImage(ImageName, OldPath))
        ImageName = OldPath;
    if (!CreateProcessW(ImageName,
                        NULL,
                        NULL,
                        NULL,
                        FALSE,
                        DETACHED_PROCESS,
                        NULL,
                        NULL,
                        &StartupInfo,
                        &ProcessInformation))
    {
        ERR("WININIT: Failed to start %ls (error %lu)\n", ImageName, GetLastError());
        return NULL;
    }

    CloseHandle(ProcessInformation.hThread);
    return ProcessInformation.hProcess;
}

int
WINAPI
wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nShowCmd)
{
    HANDLE Processes[2];
    HWINSTA WindowStation;
    HDESK Desktop;

    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nShowCmd);

    if (!RegisterLogonProcess(GetCurrentProcessId(), TRUE))
        ERR("WININIT: Could not register as the session logon process\n");

    RtlSetProcessIsCritical(TRUE, NULL, FALSE);

    WindowStation = CreateWindowStationW(L"WinSta0", 0, MAXIMUM_ALLOWED, NULL);
    if (!WindowStation || !SetProcessWindowStation(WindowStation))
    {
        ERR("WININIT: Could not create WinSta0 (error %lu)\n", GetLastError());
    }
    else
    {
        if (!CreateDesktopW(L"Winlogon", NULL, NULL, 0, MAXIMUM_ALLOWED, NULL))
            ERR("WININIT: Could not create the Winlogon desktop (error %lu)\n", GetLastError());
        Desktop = CreateDesktopW(L"Default", NULL, NULL, 0, MAXIMUM_ALLOWED, NULL);
        if (!Desktop || !SetThreadDesktop(Desktop))
            ERR("WININIT: Could not use the Default desktop (error %lu)\n", GetLastError());
    }

    if (!CreateTermSrvReadyEvent())
        ERR("WININIT: Could not create the TermSrvReadyEvent (error %lu)\n", GetLastError());

    Processes[0] = StartSystemProcess(L"services.exe");
    Processes[1] = StartSystemProcess(L"lsass.exe");
    if (!Processes[0] || !Processes[1])
        return 1;

    ServicesProcess = Processes[0];
    if (!CreateShutdownEvents())
        ERR("WININIT: Could not set up the shutdown notification (error %lu)\n", GetLastError());

    WaitForMultipleObjects(ARRAYSIZE(Processes), Processes, TRUE, INFINITE);
    return 0;
}
