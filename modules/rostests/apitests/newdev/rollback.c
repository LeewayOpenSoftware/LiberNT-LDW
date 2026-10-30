/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for newdev!DiRollbackDriver argument validation
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>

#include <winuser.h>
#include <winreg.h>
#include <prsht.h>
#include <setupapi.h>

typedef BOOL (WINAPI *PDIROLLBACKDRIVER)(HDEVINFO, PSP_DEVINFO_DATA, HWND, DWORD, PBOOL);

#define ROLLBACK_FLAG_BAD   0x80000000

START_TEST(rollback)
{
    HMODULE Module;
    PDIROLLBACKDRIVER DiRollbackDriver;
    SP_DEVINFO_DATA DevInfoData;
    HDEVINFO Set;
    GUID ClassGuid;
    BOOL NeedReboot;
    BOOL Result;

    Module = LoadLibraryW(L"newdev.dll");
    ok(Module != NULL, "LoadLibraryW(newdev.dll) failed: %lu\n", GetLastError());
    if (!Module)
        return;

    DiRollbackDriver = (PDIROLLBACKDRIVER)GetProcAddress(Module, "DiRollbackDriver");
    ok(DiRollbackDriver != NULL, "DiRollbackDriver must be exported\n");
    if (!DiRollbackDriver)
    {
        FreeLibrary(Module);
        return;
    }

    NeedReboot = TRUE;
    SetLastError(0xdeadbeef);
    Result = DiRollbackDriver(NULL, &DevInfoData, NULL, 0, &NeedReboot);
    ok(Result == FALSE, "A NULL set must fail\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %lu\n", GetLastError());
    ok(NeedReboot == FALSE, "NeedReboot must be cleared on failure\n");

    SetLastError(0xdeadbeef);
    Result = DiRollbackDriver(INVALID_HANDLE_VALUE, &DevInfoData, NULL, 0, NULL);
    ok(Result == FALSE, "An invalid set must fail\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %lu\n", GetLastError());

    Set = SetupDiCreateDeviceInfoList(NULL, NULL);
    ok(Set != INVALID_HANDLE_VALUE, "SetupDiCreateDeviceInfoList failed: %lu\n", GetLastError());
    if (Set != INVALID_HANDLE_VALUE)
    {
        SetLastError(0xdeadbeef);
        Result = DiRollbackDriver(Set, NULL, NULL, 0, NULL);
        ok(Result == FALSE, "A NULL device must fail\n");
        ok(GetLastError() == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %lu\n", GetLastError());

        ZeroMemory(&ClassGuid, sizeof(ClassGuid));
        ZeroMemory(&DevInfoData, sizeof(DevInfoData));
        DevInfoData.cbSize = sizeof(DevInfoData);
        if (SetupDiCreateDeviceInfoW(Set, L"Root\\LEGACY_LN_ROLLBACK_TEST\\0000",
                                     &ClassGuid, NULL, NULL, DICD_GENERATE_ID, &DevInfoData))
        {
            NeedReboot = TRUE;
            SetLastError(0xdeadbeef);
            Result = DiRollbackDriver(Set, &DevInfoData, NULL, ROLLBACK_FLAG_BAD, &NeedReboot);
            ok(Result == FALSE, "An unknown flag must fail\n");
            ok(GetLastError() == ERROR_INVALID_FLAGS, "expected ERROR_INVALID_FLAGS, got %lu\n", GetLastError());
            ok(NeedReboot == FALSE, "NeedReboot must be cleared on failure\n");
        }
        else
        {
            skip("SetupDiCreateDeviceInfoW failed (%lu); flag validation needs an element\n", GetLastError());
        }

        SetupDiDestroyDeviceInfoList(Set);
    }

    FreeLibrary(Module);
}
