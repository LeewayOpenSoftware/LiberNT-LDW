/*
 * PROJECT:     ReactOS Picture and Fax Viewer
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     STAND-IN for the F1 ribbon framework API.
 *              Shape taken from RIBBON-APPS-SPEC.md section 4.1 ("suggested
 *              header shape, not binding") and gaps G1, G3. When Toby's
 *              leeway/ribbon-f1 lands, delete this file and make vribbon.cpp
 *              include <leewayui/ribbon.h> instead: the only code that names
 *              these types is the F1 section of vribbon.cpp.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#include <windows.h>
#include <leewayui/ribbon.h>

typedef enum
{
    LWUI_K_LARGE, LWUI_K_SMALL, LWUI_K_SMALL_ICON, LWUI_K_TOGGLE_L, LWUI_K_TOGGLE_S,
    LWUI_K_SPLIT_L, LWUI_K_SPLIT_S, LWUI_K_DROPDOWN, LWUI_K_GALLERY,
    LWUI_K_COMBO, LWUI_K_COLORWELL, LWUI_K_CHECK, LWUI_K_LAUNCHER
} LeewayUIKind;

typedef struct
{
    UINT32 count;
    const UINT32 *ids;
    BOOL last_choice_becomes_primary;
} LeewayUICollection;

typedef struct
{
    UINT32 id;
    LeewayUIKind kind;
    const LeewayUICollection *collection;
    const WCHAR *keytip;            /* per placement (spec 0: keytips are layout) */
} LeewayUIControl;

typedef struct
{
    const WCHAR *label;
    const WCHAR *keytip;
    UINT32 variant_order;
    UINT32 launcher_id;
    UINT32 count;
    const LeewayUIControl *controls;
} LeewayUIGroup;

typedef struct
{
    const WCHAR *label;
    const WCHAR *keytip;
    const WCHAR *context;           /* NULL = always */
    UINT32 count;
    const LeewayUIGroup *groups;
} LeewayUITab;

typedef struct
{
    UINT32 tab_count;
    const LeewayUITab *tabs;
    UINT32 appmenu_count;
    const UINT32 *appmenu_ids;
    UINT32 qat_count;
    const UINT32 *qat_default_ids;
} LeewayUILayout;

/* G1, G3: not in the F0 skeleton library. Same C++ linkage as ribbon.h. */
HRESULT LeewayRibbonSetLayout(ILeewayRibbon *ribbon, const LeewayUILayout *layout);
void LeewayRibbonInvalidateCommand(ILeewayRibbon *ribbon, UINT32 command_id);
void LeewayRibbonInvalidateAll(ILeewayRibbon *ribbon);

