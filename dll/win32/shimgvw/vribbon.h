/*
 * PROJECT:     ReactOS Picture and Fax Viewer
 * LICENSE:     GPL-2.0 (https://spdx.org/licenses/GPL-2.0)
 * PURPOSE:     Thin adapter between the viewer and the LeewayUI ribbon
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The viewer talks only to this header. The model (vribbon_model.h) is data
 * and predicates; vribbon.cpp turns it into calls on the framework. Until
 * the F1 framework lands (VRB_HAVE_F1 undefined) the adapter creates the F0
 * skeleton window and the viewer keeps its pill toolbar as the default chrome.
 */

#pragma once

#include "vribbon_model.h"

#ifdef __cplusplus
#include <leewayui/ribbon.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VRB_RIBBON VRB_RIBBON;

/* TRUE when the ribbon is the default chrome in this build. The registry
 * value HKCU\Software\Leeway\PhotoViewer\ChromeStyle ("ribbon" or "pill")
 * overrides it. */
BOOL VRB_RibbonIsDefaultChrome(void);

HRESULT VRB_RibbonCreate(HWND parent, HINSTANCE instance, const VRB_HOST *host, VRB_RIBBON **out);
void VRB_RibbonDestroy(VRB_RIBBON *ribbon);
HWND VRB_RibbonWindow(VRB_RIBBON *ribbon);
UINT VRB_RibbonHeight(VRB_RIBBON *ribbon);

/* The host calls these after any change a predicate depends on. */
void VRB_RibbonInvalidate(VRB_RIBBON *ribbon, UINT command_id);
void VRB_RibbonInvalidateAll(VRB_RIBBON *ribbon);

void VRB_RibbonOnThemeChanged(VRB_RIBBON *ribbon);
void VRB_RibbonOnDpiChanged(VRB_RIBBON *ribbon, UINT dpi);

/* Framework callbacks, exported for the F1 library to call. */
HRESULT VRB_RibbonExecute(VRB_RIBBON *ribbon, UINT command_id);
#ifdef __cplusplus
HRESULT VRB_RibbonQueryState(VRB_RIBBON *ribbon, UINT command_id, LeewayUICommand *out);
#endif
void VRB_RibbonQatChanged(VRB_RIBBON *ribbon, const UINT *ids, UINT count);

#ifdef __cplusplus
}
#endif
