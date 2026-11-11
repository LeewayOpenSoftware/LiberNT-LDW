/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Ribbon control, F1 framework: tabs, groups, controls, collections, QAT, app menu, keytips
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 *
 * Plain C API (owner decision D-R1: own control, own API; no COM vtable).
 *
 * Model (RIBBON-CONTROLS-DESIGN.md §1): the app hands the ribbon a command
 * table (one entry per action: id, strings, icon, static state) and two
 * callbacks, execute(id) and query-state(id). Layout is separate from
 * identity: a LeewayUILayout places command ids in tabs, groups, the QAT
 * and the application menu, and the same id may appear in several places
 * with one state. Collections (galleries, dropdown and split-button menus)
 * are lists of commands hung off the command that opens them.
 *
 * Ownership: every Set* call copies what it is given (structures and
 * strings), so the app may build its tables on the stack or in static
 * const data and let them go afterwards.
 *
 * Lifetime: LeewayRibbonCreate makes a child window of `parent`; the parent
 * positions it with LeewayRibbonGetHeight (which changes when the band is
 * minimised or the DPI changes; the ribbon then sends WM_NOTIFY
 * LWUI_RBN_HEIGHTCHANGED to the parent). LeewayRibbonDestroy is the only
 * destructor.
 */

#pragma once

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Framework-owned command ids (RIBBON-APPS-SPEC.md §0) ----
 * 0xF001..0xF003 are handled inside the ribbon and never reach execute().
 * 0xF004 and 0xF005 are sent to execute(); the app answers their state
 * (for 0xF003 the ribbon answers: checked = band minimised). */
#define LWUI_APPMENU            0xF001u   /* the application button */
#define LWUI_QAT_CUSTOMIZE      0xF002u   /* the QAT's trailing dropdown */
#define LWUI_RIBBON_MINIMIZE    0xF003u   /* toggle the band (Ctrl+F1) */
#define LWUI_HELP               0xF004u   /* help button at the right of the tab strip */
#define LWUI_STATUSBAR_TOGGLE   0xF005u   /* show/hide the frame status bar (frame-owned) */

/* ---- Icon ids ----
 * icon_id is app-defined and passed to the resolver. OR in LWUI_ICON_GLYPH
 * for glyph-type icons (letters, lines, arrows in the ink colour): on a dark
 * band the ribbon asks for their MONO variant drawn in the text colour
 * (icons/README.md dark-theme rule). In high contrast every icon is MONO. */
#define LWUI_ICON_NONE          0u
#define LWUI_ICON_GLYPH         0x80000000u
#define LWUI_ICON_ID_MASK       0x7FFFFFFFu

/* ---- Command table ---- */

typedef struct {
    UINT32 id;
    const WCHAR *label;         /* button label and menu row text */
    const WCHAR *tooltip;       /* NULL = label */
    const WCHAR *keytip;        /* 1-3 characters; the default for every placement */
    UINT32 icon_id;             /* LWUI_ICON_NONE = placeholder glyph */
    BOOL enabled;               /* static defaults; query-state may override at every paint */
    BOOL checked;
    BOOL visible;
} LeewayUICommand;

/* What query-state returns. The ribbon fills it from the table before the
 * call; the handler changes what differs and returns S_OK (E_NOTIMPL or a
 * NULL handler keeps the table values). label/tooltip NULL = table strings;
 * a non-NULL pointer must stay valid until the next query. */
typedef struct {
    BOOL enabled;
    BOOL checked;
    BOOL visible;
    const WCHAR *label;
    const WCHAR *tooltip;
} LeewayUICommandState;

typedef HRESULT (*LeewayUICommandExecuteHandler)(UINT32 id, void *app_context);
typedef HRESULT (*LeewayUICommandStateHandler)(UINT32 id, LeewayUICommandState *state, void *app_context);

/* ---- Control kinds (a layout property: the same id may be large in one
 * tab and small in another; RIBBON-APPS-SPEC.md §0 kinds) ---- */

typedef enum {
    LWUI_CONTROL_BUTTON_LARGE = 0,  /* L: 32 px icon, label below */
    LWUI_CONTROL_BUTTON_SMALL,      /* S: 16 px icon, label right */
    LWUI_CONTROL_BUTTON_SMALL_ICON, /* Si: 16 px icon only */
    LWUI_CONTROL_TOGGLE_LARGE,      /* TL: checked state from query-state */
    LWUI_CONTROL_TOGGLE_SMALL,      /* TS */
    LWUI_CONTROL_SPLIT_LARGE,       /* SPLIT L: primary action + arrow opening the command's collection */
    LWUI_CONTROL_SPLIT_SMALL,       /* SPLIT S */
    LWUI_CONTROL_DROPDOWN,          /* DD: the whole control opens the collection as a menu */
    LWUI_CONTROL_GALLERY,           /* GAL: small control opening the collection as a grid */
    LWUI_CONTROL_COMBO,             /* gap G14: drawn as a dropdown until implemented */
    LWUI_CONTROL_COLORWELL,         /* gap G15: drawn as a small button until implemented */
    LWUI_CONTROL_CHECKBOX,          /* gap G19: drawn as a small toggle until implemented */
    LWUI_CONTROL_LAUNCHER,          /* gap G17: use LeewayUIGroup.launcher_id instead */
} LeewayUIControlKind;

/* ---- Layout tree (arrays + counts, so tables can be static const and
 * later serialised into the compiled layout resource, gap G23) ---- */

typedef struct {
    UINT32 command_id;
    LeewayUIControlKind kind;
    const WCHAR *keytip;        /* per-placement keytip; NULL = the command's keytip */
} LeewayUIPlacement;

/* Group resize variants (gap G11). The app declares the order the group
 * walks through as the window narrows, e.g.
 *   LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_MIXED, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED)
 * 0 = default: large -> collapsed when every control is a large kind, else
 * large -> mixed -> small -> collapsed. */
typedef enum {
    LWUI_VARIANT_NONE = 0,
    LWUI_VARIANT_LARGE = 1,     /* controls as declared */
    LWUI_VARIANT_MIXED = 2,     /* large kinds become small with labels, three rows per column */
    LWUI_VARIANT_SMALL = 3,     /* every control icon-only, three rows per column */
    LWUI_VARIANT_COLLAPSED = 4, /* one large button opening the group in a popup */
} LeewayUIGroupVariant;

#define LWUI_VARIANT_ORDER(a, b, c, d) \
    ((UINT32)(a) | ((UINT32)(b) << 8) | ((UINT32)(c) << 16) | ((UINT32)(d) << 24))

typedef struct {
    const WCHAR *label;
    const WCHAR *keytip;        /* keytip of the collapsed-group button; NULL = none */
    UINT32 variant_order;       /* LWUI_VARIANT_ORDER(...) or 0 */
    UINT32 launcher_id;         /* command of the dialog launcher at the group's corner, 0 = none (G17) */
    UINT32 control_count;
    const LeewayUIPlacement *controls;
} LeewayUIGroup;

typedef struct {
    UINT32 id;                  /* app-chosen tab id, unique in the layout */
    const WCHAR *label;
    const WCHAR *keytip;
    const WCHAR *context;       /* NULL = always shown; else shown while LeewayRibbonSetContext(name, TRUE) (G13) */
    UINT32 group_count;
    const LeewayUIGroup *groups;
} LeewayUITab;

typedef struct {
    const WCHAR *app_name;      /* registry key name: HKCU\Software\Leeway\<app_name>; NULL = no persistence */
    UINT32 tab_count;
    const LeewayUITab *tabs;
    UINT32 appmenu_count;       /* application menu rows, in order (large rows) */
    const UINT32 *appmenu_ids;
    UINT32 qat_count;           /* QAT default contents; the persisted list wins when present */
    const UINT32 *qat_default_ids;
    UINT32 help_command_id;     /* executed by the help button; 0 = execute(LWUI_HELP) */
} LeewayUILayout;

/* ---- Collections (gap G4) ----
 * Items of the menu, gallery or split-button dropdown opened by a command.
 * Each item is itself executed by id. LWUI_COLLECTION_LAST_CHOICE_PRIMARY
 * makes a split button's primary action repeat the last chosen item. */

typedef struct {
    UINT32 id;
    const WCHAR *label;
    UINT32 icon_id;
} LeewayUICollectionItem;

#define LWUI_COLLECTION_LAST_CHOICE_PRIMARY 0x00000001u

/* ---- Icons (gap G10) ----
 * The resolver returns an HICON of size_px (alpha icons draw with DrawIconEx);
 * the ribbon owns it (DestroyIcon on eviction, theme and DPI change). NULL =
 * draw the placeholder glyph. fg/bg are the text and band colours the icon
 * will sit on (MONO masters are drawn in fg; knockouts use bg). */
typedef enum {
    LWUI_ICON_VARIANT_COLOUR = 0,
    LWUI_ICON_VARIANT_MONO = 1,
} LeewayUIIconVariant;

typedef HICON (*LeewayUIIconResolver)(UINT32 icon_id, UINT32 size_px, LeewayUIIconVariant variant,
                                      COLORREF fg, COLORREF bg, void *resolver_context);

/* ---- WM_NOTIFY to the parent (NMHDR.hwndFrom = ribbon window) ---- */
#define LWUI_RBN_FIRST          (0U - 7000U)
#define LWUI_RBN_HEIGHTCHANGED  (LWUI_RBN_FIRST - 0)   /* re-read LeewayRibbonGetHeight and relayout */
#define LWUI_RBN_QATCHANGED     (LWUI_RBN_FIRST - 1)   /* the user customised the QAT (already persisted) */
#define LWUI_RBN_MINIMIZED      (LWUI_RBN_FIRST - 2)   /* band minimised or restored (height also changed) */

/* ---- Errors ---- */
#define LWUI_E_KEYTIP_COLLISION MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0201)  /* pair named via OutputDebugString */
#define LWUI_E_UNKNOWN_COMMAND  MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0202)  /* layout references an id not in the table */

/* ---- The control ---- */

typedef struct ILeewayRibbon ILeewayRibbon;   /* opaque; name kept from F0 */

HRESULT WINAPI LeewayRibbonCreate(HWND parent, const LeewayUICommand *table, UINT32 count,
                                  LeewayUICommandExecuteHandler execute,
                                  LeewayUICommandStateHandler state,
                                  void *app_context, ILeewayRibbon **out);
HRESULT WINAPI LeewayRibbonDestroy(ILeewayRibbon *ribbon);

HWND    WINAPI LeewayRibbonGetHWND(ILeewayRibbon *ribbon);
HWND    WINAPI LeewayRibbonGetWindowHandle(ILeewayRibbon *ribbon);   /* F0 name, same handle */
UINT32  WINAPI LeewayRibbonGetHeight(ILeewayRibbon *ribbon);         /* physical px at the current DPI */

/* Layout (G1, G5, G6, G12). Validates ids against the table and keytips per
 * level (G18); loads the persisted QAT when app_name is set. Replaces any
 * previous layout; collections and contexts are kept. */
HRESULT WINAPI LeewayRibbonSetLayout(ILeewayRibbon *ribbon, const LeewayUILayout *layout);

/* Items of the collection opened by command_id (a gallery, dropdown or
 * split button); count 0 removes it. */
HRESULT WINAPI LeewayRibbonSetCollection(ILeewayRibbon *ribbon, UINT32 command_id,
                                         const LeewayUICollectionItem *items, UINT32 count, UINT32 flags);

/* Contextual tabs (G13): tabs whose context equals name appear while active;
 * the first one is selected on appear and the previous tab restored on disappear. */
HRESULT WINAPI LeewayRibbonSetContext(ILeewayRibbon *ribbon, const WCHAR *name, BOOL active);

HRESULT WINAPI LeewayRibbonSetIconResolver(ILeewayRibbon *ribbon, LeewayUIIconResolver resolver, void *resolver_context);

/* Accelerator table whose keys are shown in tooltips (G22); the ribbon copies it. NULL = none. */
HRESULT WINAPI LeewayRibbonSetAccelerators(ILeewayRibbon *ribbon, HACCEL accelerators);

/* State invalidation (G3): query-state is re-read for the command (every
 * placement of it) or for everything at the next paint. */
HRESULT WINAPI LeewayRibbonInvalidateCommand(ILeewayRibbon *ribbon, UINT32 command_id);
HRESULT WINAPI LeewayRibbonInvalidateAll(ILeewayRibbon *ribbon);

/* Tabs. Contextual tabs cannot be activated while their context is off. */
HRESULT WINAPI LeewayRibbonSetActiveTab(ILeewayRibbon *ribbon, UINT32 tab_id);
UINT32  WINAPI LeewayRibbonGetActiveTab(ILeewayRibbon *ribbon);

/* Band (G21). */
HRESULT WINAPI LeewayRibbonMinimizeRibbon(ILeewayRibbon *ribbon, BOOL minimize);
BOOL    WINAPI LeewayRibbonIsMinimized(ILeewayRibbon *ribbon);

/* QAT contents (ids in order). Set persists when app_name is set and
 * notifies the parent. Get returns the needed count in *count when ids is
 * NULL or too small (E_NOT_SUFFICIENT_BUFFER). */
HRESULT WINAPI LeewayRibbonGetQAT(ILeewayRibbon *ribbon, UINT32 *ids, UINT32 *count);
HRESULT WINAPI LeewayRibbonSetQAT(ILeewayRibbon *ribbon, const UINT32 *ids, UINT32 count);

/* Forward from the top-level window: WM_THEMECHANGED, WM_SETTINGCHANGE and
 * WM_SYSCOLORCHANGE -> OnThemeChanged; WM_DPICHANGED -> SetDPI(HIWORD(wParam)). */
HRESULT WINAPI LeewayRibbonOnThemeChanged(ILeewayRibbon *ribbon);
HRESULT WINAPI LeewayRibbonSetDPI(ILeewayRibbon *ribbon, UINT32 dpi);

/* Call from the message loop before TranslateMessage. Handles Alt / F10
 * (keytip mode), the keytip walk, Esc, and Ctrl+F1. Returns TRUE when the
 * message was consumed. */
BOOL    WINAPI LeewayRibbonPreTranslateMessage(ILeewayRibbon *ribbon, MSG *msg);

#ifdef __cplusplus
}
#endif
