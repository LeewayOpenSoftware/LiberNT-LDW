/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Per-monitor DPI v2 helpers
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 */

#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

UINT32 LeewayDpiGetForWindow(HWND window);
UINT32 LeewayDpiGetForMonitor(HMONITOR monitor);
int LeewayDpiLogicalToPhysical(UINT32 dpi, int logical_value);
int LeewayDpiPhysicalToLogical(UINT32 dpi, int physical_value);
int LeewayDpiScale(UINT32 dpi, int baseline_value);

#ifdef __cplusplus
}
#endif
