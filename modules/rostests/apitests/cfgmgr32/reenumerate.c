/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for cfgmgr32!CM_Reenumerate_DevNode argument validation
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>

#include <winreg.h>
#include <cfgmgr32.h>

START_TEST(reenumerate)
{
    CONFIGRET cr;

    cr = CM_Reenumerate_DevNode(0, 0);
    ok(cr == CR_INVALID_DEVNODE, "expected CR_INVALID_DEVNODE, got 0x%lx\n", cr);

    cr = CM_Reenumerate_DevNode(0, CM_REENUMERATE_RETRY_INSTALLATION);
    ok(cr == CR_INVALID_DEVNODE,
       "CM_REENUMERATE_RETRY_INSTALLATION must be an accepted flag, got 0x%lx\n", cr);

    cr = CM_Reenumerate_DevNode((DEVINST)0x1, 0x80000000);
    ok(cr == CR_INVALID_FLAG, "expected CR_INVALID_FLAG, got 0x%lx\n", cr);
}
