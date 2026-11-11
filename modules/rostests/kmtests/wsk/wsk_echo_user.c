/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     WSK stream and datagram echo over loopback, user-mode part
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>
#include "wsk_echo.h"

START_TEST(WskEcho)
{
    DWORD Error;

    Error = KmtLoadAndOpenDriver(L"WskEcho", TRUE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
    {
        return;
    }
    Error = KmtSendToDriver(IOCTL_WSKECHO_RUN);
    ok_eq_int(Error, ERROR_SUCCESS);
    KmtCloseDriver();
    KmtUnloadDriver();
}
