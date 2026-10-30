/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for cfgmgr32!LnCM_Install_DevInstW argument validation
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>

#include <winreg.h>
#include <cfgmgr32.h>

CONFIGRET WINAPI LnCM_Install_DevInstW(PCWSTR, ULONG, PBOOL);

START_TEST(installdevinst)
{
    CONFIGRET cr;
    BOOL Reboot;

    Reboot = TRUE;
    cr = LnCM_Install_DevInstW(NULL, 0, &Reboot);
    ok(cr == CR_INVALID_DEVINST, "expected CR_INVALID_DEVINST, got 0x%lx\n", cr);
    ok(Reboot == FALSE, "Reboot must be cleared on failure\n");

    cr = LnCM_Install_DevInstW(L"", 0, NULL);
    ok(cr == CR_INVALID_DEVINST, "expected CR_INVALID_DEVINST, got 0x%lx\n", cr);

    cr = LnCM_Install_DevInstW(L"Root\\LEGACY_LN_INSTALL_TEST\\0000", 0x80000000, NULL);
    ok(cr == CR_INVALID_FLAG, "expected CR_INVALID_FLAG, got 0x%lx\n", cr);
}
