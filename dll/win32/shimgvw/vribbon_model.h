/*
 * PROJECT:     ReactOS Picture and Fax Viewer
 * LICENSE:     GPL-2.0 (https://spdx.org/licenses/GPL-2.0)
 * PURPOSE:     Ribbon command model of the viewer: command table, layout,
 *              named enable predicates, quick access toolbar persistence.
 *              Pure data and logic, no framework calls: the adapter in
 *              vribbon.cpp is the only code that talks to LeewayUI, so the
 *              F1 framework API lands there and nowhere else.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * SPEC:        shell/appframework/RIBBON-APPS-SPEC.md section 1
 */

#pragma once

#ifndef _INC_WINDOWS
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Viewer command ids. Ids below 700 are the existing IDC_* values of
 * resource.h (the host routes them as WM_COMMAND); 700-799 are new. */
#define VRB_CMD_OPEN        700
#define VRB_CMD_WALLPAPER   701
#define VRB_CMD_EXIT        702
#define VRB_CMD_ZOOMLEVEL   710
#define VRB_CMD_ZOOM_FIRST  711   /* 10 %   */
#define VRB_CMD_ZOOM_LAST   720   /* 1600 % */
#define VRB_CMD_ZOOM_FIT    721   /* alias of IDC_BEST_FIT, reserved */
#define VRB_CMD_ZOOM_ACTUAL 722   /* alias of IDC_REAL_SIZE, reserved */
#define VRB_ZOOM_LEVELS     (VRB_CMD_ZOOM_LAST - VRB_CMD_ZOOM_FIRST + 1)

/* Framework-owned commands (spec section 0). */
#define VRB_LWUI_APPMENU            0xF001
#define VRB_LWUI_QAT_CUSTOMIZE      0xF002
#define VRB_LWUI_RIBBON_MINIMIZE    0xF003
#define VRB_LWUI_HELP               0xF004
#define VRB_LWUI_STATUSBAR_TOGGLE   0xF005

/* String table layout: label of command N is VRB_IDS_LABEL_BASE + N.
 * Framework ids 0xF00x map to VRB_IDS_LABEL_BASE + 0x100 + x. */
#define VRB_IDS_LABEL_BASE  1000
#define VRB_IDS_TAB_HOME    3001
#define VRB_IDS_TAB_VIEW    3002
#define VRB_IDS_GRP_NAVIGATE  3011
#define VRB_IDS_GRP_ZOOM      3012
#define VRB_IDS_GRP_ROTATE    3013
#define VRB_IDS_GRP_SLIDESHOW 3014
#define VRB_IDS_GRP_FILE      3015
#define VRB_IDS_GRP_DISPLAY   3016
#define VRB_IDS_GRP_HELP      3017

/* Named predicates the host answers in query-state (spec 1.2). */
typedef enum
{
    VRB_P_ALWAYS = 0,
    VRB_P_HAS_IMAGE,
    VRB_P_HAS_LIST,
    VRB_P_CAN_ZOOM_IN,
    VRB_P_CAN_ZOOM_OUT,
    VRB_P_IS_FIT,
    VRB_P_IS_ACTUAL,
    VRB_P_IS_FULLSCREEN,
    VRB_P_SLIDESHOW_RUNNING,
    VRB_P_IS_ANIMATED,
    VRB_P_CAN_SAVE,
    /* The viewer already gates these on the shell verb or the file being
     * there (EnableCommandIfVerbExists, the delete check); same rule here. */
    VRB_P_CAN_DELETE,
    VRB_P_CAN_PRINT,
    VRB_P_CAN_EDIT,
    /* Frame state. */
    VRB_P_STATUSBAR_SHOWN,
    VRB_P_RIBBON_MINIMIZED,
    VRB_P_COUNT
} VRB_PRED;

typedef struct
{
    UINT id;
    const WCHAR *keytip;        /* default keytip (Home placement) */
    const char *icon_slot;      /* Aureline master, aur.<app>.<slug> */
    UINT icon_bmp;             /* existing IDB_* bitmap or 0 = placeholder */
    UINT ids_tip;              /* tooltip string or 0 = use the label */
    VRB_PRED enable;
    VRB_PRED checked;          /* VRB_P_ALWAYS = not a toggle */
    BOOL framework;            /* supplied by LeewayUI, not by the app */
} VRB_CMDDEF;

typedef enum
{
    VRB_K_LARGE, VRB_K_SMALL, VRB_K_SMALL_ICON,
    VRB_K_TOGGLE_L, VRB_K_TOGGLE_S,
    VRB_K_SPLIT_L, VRB_K_SPLIT_S,
    VRB_K_DROPDOWN, VRB_K_GALLERY
} VRB_KIND;

typedef struct
{
    UINT count;
    const UINT *ids;
    BOOL last_choice_becomes_primary;
    const WCHAR *const *keytips;    /* per item, or NULL (menu level) */
} VRB_COLLECTION;

typedef struct
{
    UINT id;
    VRB_KIND kind;
    const VRB_COLLECTION *collection;
    const WCHAR *keytip;            /* per placement */
} VRB_CONTROL;

/* Variant orders of a group (spec section 0). */
typedef enum
{
    VRB_V_LARGE_COLLAPSED,          /* large -> collapsed */
    VRB_V_FULL,                     /* all-large -> mixed -> small -> collapsed */
    VRB_V_MIXED,                    /* mixed -> small -> collapsed */
    VRB_V_LARGE_SMALL,              /* all-large -> small -> collapsed */
    VRB_V_SMALL                     /* small */
} VRB_VARIANTS;

typedef struct
{
    UINT ids_label;
    VRB_VARIANTS variants;
    UINT count;
    const VRB_CONTROL *controls;
} VRB_GROUP;

typedef struct
{
    UINT ids_label;
    const WCHAR *keytip;
    UINT count;
    const VRB_GROUP *groups;
} VRB_TAB;

typedef struct
{
    UINT id;
    const WCHAR *keytip;
} VRB_MENUROW;

typedef struct
{
    UINT tab_count;
    const VRB_TAB *tabs;
    UINT appmenu_count;
    const VRB_MENUROW *appmenu;
    UINT qat_count;
    const UINT *qat_default;
    const WCHAR *app_keytip;        /* the application button */
} VRB_LAYOUT;

/* What the host answers for the predicates and does for a command. */
typedef struct
{
    void *ctx;
    BOOL (*Predicate)(void *ctx, VRB_PRED pred);
    /* Runs a command. Ids below 700 and the framework aliases are routed by
     * the host to its existing handlers. */
    HRESULT (*Execute)(void *ctx, UINT id);
    /* Current zoom in percent, for the label of the zoom gallery. */
    UINT (*GetZoomPercent)(void *ctx);
} VRB_HOST;

extern const VRB_CMDDEF g_VrbCommands[];
extern const UINT g_VrbCommandCount;
extern const VRB_LAYOUT g_VrbLayout;
extern const UINT g_VrbZoomPercents[VRB_ZOOM_LEVELS];

const VRB_CMDDEF *VRB_FindCommand(UINT id);
UINT VRB_LabelStringId(UINT id);

/* Zoom gallery helpers. */
BOOL VRB_ZoomPercentOfCommand(UINT id, UINT *percent);
UINT VRB_NearestZoomIndex(UINT percent);

/* Enabled/checked of one command through the host's predicates. A command
 * with no definition is reported disabled. */
BOOL VRB_EvalState(const VRB_HOST *host, UINT id, BOOL *enabled, BOOL *checked);

/* Checks the model against itself: ids resolve, keytips are unique per level,
 * every predicate is a real one, the zoom collection matches the zoom
 * levels. Returns the number of problems; writes the first into err. */
UINT VRB_ValidateModel(WCHAR *err, UINT err_chars);

/* Quick access toolbar persistence. The key is
 * HKCU\Software\Leeway\PhotoViewer\QAT, value "Items" (REG_BINARY, an array
 * of UINT32 command ids). Unknown, duplicate or framework ids are dropped on
 * load; an empty or missing list gives the defaults. */
#define VRB_QAT_MAX 32
UINT VRB_QatLoadFrom(const WCHAR *subkey, UINT *ids, UINT max);
BOOL VRB_QatSaveTo(const WCHAR *subkey, const UINT *ids, UINT count);
UINT VRB_QatLoad(UINT *ids, UINT max);
BOOL VRB_QatSave(const UINT *ids, UINT count);

/* Frame options kept next to the QAT under HKCU\Software\Leeway\PhotoViewer. */
BOOL VRB_ReadChromeIsRibbon(BOOL default_value);
BOOL VRB_ReadStatusBarShown(void);
void VRB_WriteStatusBarShown(BOOL shown);

#ifdef __cplusplus
}
#endif
