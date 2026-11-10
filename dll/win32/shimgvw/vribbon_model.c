/*
 * PROJECT:     ReactOS Picture and Fax Viewer
 * LICENSE:     GPL-2.0 (https://spdx.org/licenses/GPL-2.0)
 * PURPOSE:     Ribbon command model of the viewer (see vribbon_model.h)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <wchar.h>
#include <strsafe.h>
#include "resource.h"
#include "vribbon_model.h"

#define REGKEY_ROOT L"Software\\Leeway\\PhotoViewer"
#define REGKEY_QAT  L"Software\\Leeway\\PhotoViewer\\QAT"

#define CMD(id, key, slot, bmp, tip, en, chk) \
    { (id), (key), (slot), (bmp), (tip), (en), (chk), FALSE }
#define FWCMD(id, key, slot, en, chk) \
    { (id), (key), (slot), 0, 0, (en), (chk), TRUE }

const VRB_CMDDEF g_VrbCommands[] =
{
    /* Existing toolbar commands, ids are the IDC_* values. */
    CMD(IDC_PREV_PIC,    L"P", "aur.viewer.prev",         IDB_PREV_PIC,    IDS_TOOLTIP_PREV_PIC,    VRB_P_HAS_LIST,     VRB_P_ALWAYS),
    CMD(IDC_NEXT_PIC,    L"N", "aur.viewer.next",         IDB_NEXT_PIC,    IDS_TOOLTIP_NEXT_PIC,    VRB_P_HAS_LIST,     VRB_P_ALWAYS),
    CMD(IDC_BEST_FIT,    L"F", "aur.viewer.fit",          IDB_BEST_FIT,    IDS_TOOLTIP_BEST_FIT,    VRB_P_HAS_IMAGE,    VRB_P_IS_FIT),
    CMD(IDC_REAL_SIZE,   L"A", "aur.viewer.actual",       IDB_REAL_SIZE,   IDS_TOOLTIP_REAL_SIZE,   VRB_P_HAS_IMAGE,    VRB_P_IS_ACTUAL),
    CMD(IDC_SLIDE_SHOW,  L"S", "aur.viewer.slideshow",    IDB_SLIDE_SHOW,  IDS_TOOLTIP_SLIDE_SHOW,  VRB_P_HAS_IMAGE,    VRB_P_SLIDESHOW_RUNNING),
    CMD(IDC_ZOOM_IN,     L"I", "aur.common.zoom-in",      IDB_ZOOM_IN,     IDS_TOOLTIP_ZOOM_IN,     VRB_P_CAN_ZOOM_IN,  VRB_P_ALWAYS),
    CMD(IDC_ZOOM_OUT,    L"O", "aur.common.zoom-out",     IDB_ZOOM_OUT,    IDS_TOOLTIP_ZOOM_OUT,    VRB_P_CAN_ZOOM_OUT, VRB_P_ALWAYS),
    CMD(IDC_ROT_CLOCKW,  L"R", "aur.common.rotate-right", IDB_ROT_CLOCKW,  IDS_TOOLTIP_ROT_CLOCKW,  VRB_P_HAS_IMAGE,    VRB_P_ALWAYS),
    CMD(IDC_ROT_COUNCW,  L"L", "aur.common.rotate-left",  IDB_ROT_COUNCW,  IDS_TOOLTIP_ROT_COUNCW,  VRB_P_HAS_IMAGE,    VRB_P_ALWAYS),
    CMD(IDC_ROT_CWSAVE,  L"S", "aur.common.rotate-right", IDB_ROT_CWSAVE,  IDS_TOOLTIP_ROT_CWSAVE,  VRB_P_CAN_SAVE,     VRB_P_ALWAYS),
    CMD(IDC_ROT_CCWSAVE, L"S", "aur.common.rotate-left",  IDB_ROT_CCWSAVE, IDS_TOOLTIP_ROT_CCWSAVE, VRB_P_CAN_SAVE,     VRB_P_ALWAYS),
    CMD(IDC_DELETE,      L"D", "aur.common.delete",       IDB_DELETE,      IDS_TOOLTIP_DELETE,      VRB_P_CAN_DELETE,   VRB_P_ALWAYS),
    CMD(IDC_PRINT,       L"T", "aur.common.print",        IDB_PRINT,       IDS_TOOLTIP_PRINT,       VRB_P_CAN_PRINT,    VRB_P_ALWAYS),
    CMD(IDC_SAVEAS,      L"V", "aur.viewer.saveas",       IDB_SAVEAS,      IDS_TOOLTIP_SAVEAS,      VRB_P_CAN_SAVE,     VRB_P_ALWAYS),
    CMD(IDC_MODIFY,      L"E", "aur.viewer.edit",         IDB_MODIFY,      IDS_TOOLTIP_MODIFY,      VRB_P_CAN_EDIT,     VRB_P_ALWAYS),
    CMD(IDC_HELP_TOC,    L"H", "aur.common.help",         IDB_HELP_TOC,    IDS_TOOLTIP_HELP_TOC,    VRB_P_ALWAYS,       VRB_P_ALWAYS),
    CMD(IDC_TOGGLEFULLSCREEN, L"F", "aur.common.fullscreen", 0, 0,                                  VRB_P_HAS_IMAGE,    VRB_P_IS_FULLSCREEN),
    CMD(IDC_INCTIMER,    L"F", "aur.viewer.slideshow",    0, 0,                                     VRB_P_ALWAYS,       VRB_P_ALWAYS),
    CMD(IDC_DECTIMER,    L"S", "aur.viewer.slideshow",    0, 0,                                     VRB_P_ALWAYS,       VRB_P_ALWAYS),
    /* New viewer commands. */
    CMD(VRB_CMD_OPEN,      L"O", "aur.viewer.open",      0, 0, VRB_P_ALWAYS,    VRB_P_ALWAYS),
    CMD(VRB_CMD_WALLPAPER, L"B", "aur.viewer.wallpaper", 0, 0, VRB_P_HAS_IMAGE, VRB_P_ALWAYS),
    CMD(VRB_CMD_EXIT,      L"X", "aur.common.exit",      0, 0, VRB_P_ALWAYS,    VRB_P_ALWAYS),
    CMD(VRB_CMD_ZOOMLEVEL, L"Z", "aur.common.zoom",      0, 0, VRB_P_HAS_IMAGE, VRB_P_ALWAYS),
    CMD(711, NULL, NULL, 0, 0, VRB_P_HAS_IMAGE, VRB_P_ALWAYS),
    CMD(712, NULL, NULL, 0, 0, VRB_P_HAS_IMAGE, VRB_P_ALWAYS),
    CMD(713, NULL, NULL, 0, 0, VRB_P_HAS_IMAGE, VRB_P_ALWAYS),
    CMD(714, NULL, NULL, 0, 0, VRB_P_HAS_IMAGE, VRB_P_ALWAYS),
    CMD(715, NULL, NULL, 0, 0, VRB_P_HAS_IMAGE, VRB_P_ALWAYS),
    CMD(716, NULL, NULL, 0, 0, VRB_P_HAS_IMAGE, VRB_P_ALWAYS),
    CMD(717, NULL, NULL, 0, 0, VRB_P_HAS_IMAGE, VRB_P_ALWAYS),
    CMD(718, NULL, NULL, 0, 0, VRB_P_HAS_IMAGE, VRB_P_ALWAYS),
    CMD(719, NULL, NULL, 0, 0, VRB_P_HAS_IMAGE, VRB_P_ALWAYS),
    CMD(720, NULL, NULL, 0, 0, VRB_P_HAS_IMAGE, VRB_P_ALWAYS),
    /* Frame-owned and framework commands placed by this layout. */
    FWCMD(VRB_LWUI_RIBBON_MINIMIZE,  L"M", "aur.common.ribbon-min", VRB_P_ALWAYS, VRB_P_RIBBON_MINIMIZED),
    FWCMD(VRB_LWUI_STATUSBAR_TOGGLE, L"S", "aur.common.statusbar",  VRB_P_ALWAYS, VRB_P_STATUSBAR_SHOWN),
};
const UINT g_VrbCommandCount = sizeof(g_VrbCommands) / sizeof(g_VrbCommands[0]);

const UINT g_VrbZoomPercents[VRB_ZOOM_LEVELS] =
{
    10, 25, 50, 75, 100, 150, 200, 400, 800, 1600
};

/* ---- collections ---- */

static const UINT s_RotLeftMenu[]  = { IDC_ROT_CCWSAVE };
static const UINT s_RotRightMenu[] = { IDC_ROT_CWSAVE };
static const UINT s_SlideMenu[]    = { IDC_INCTIMER, IDC_DECTIMER };
static const UINT s_ZoomGallery[]  =
{
    711, 712, 713, 714, 715, 716, 717, 718, 719, 720,
    IDC_BEST_FIT, IDC_REAL_SIZE
};
static const WCHAR *const s_SaveKeytip[]   = { L"S" };
static const WCHAR *const s_SlideKeytips[] = { L"F", L"S" };

static const VRB_COLLECTION s_RotLeftColl  = { 1, s_RotLeftMenu,  FALSE, s_SaveKeytip };
static const VRB_COLLECTION s_RotRightColl = { 1, s_RotRightMenu, FALSE, s_SaveKeytip };
static const VRB_COLLECTION s_SlideColl    = { 2, s_SlideMenu,    FALSE, s_SlideKeytips };
static const VRB_COLLECTION s_ZoomColl     = { 12, s_ZoomGallery, FALSE, NULL };

/* ---- layout ---- */

static const VRB_CONTROL s_Navigate[] =
{
    { IDC_PREV_PIC, VRB_K_LARGE, NULL, L"P" },
    { IDC_NEXT_PIC, VRB_K_LARGE, NULL, L"N" },
};

/* Home zoom group; View repeats it with its own keytips (Fit is "B" there so
 * that Alt, V, F stays full screen). */
static const VRB_CONTROL s_ZoomHome[] =
{
    { IDC_BEST_FIT,      VRB_K_TOGGLE_L, NULL,        L"F" },
    { IDC_REAL_SIZE,     VRB_K_TOGGLE_L, NULL,        L"A" },
    { IDC_ZOOM_IN,       VRB_K_SMALL,    NULL,        L"I" },
    { IDC_ZOOM_OUT,      VRB_K_SMALL,    NULL,        L"O" },
    { VRB_CMD_ZOOMLEVEL, VRB_K_GALLERY,  &s_ZoomColl, L"Z" },
};
static const VRB_CONTROL s_ZoomView[] =
{
    { IDC_BEST_FIT,      VRB_K_TOGGLE_L, NULL,        L"B" },
    { IDC_REAL_SIZE,     VRB_K_TOGGLE_L, NULL,        L"A" },
    { IDC_ZOOM_IN,       VRB_K_SMALL,    NULL,        L"I" },
    { IDC_ZOOM_OUT,      VRB_K_SMALL,    NULL,        L"O" },
    { VRB_CMD_ZOOMLEVEL, VRB_K_GALLERY,  &s_ZoomColl, L"Z" },
};

static const VRB_CONTROL s_Rotate[] =
{
    { IDC_ROT_COUNCW, VRB_K_SPLIT_L, &s_RotLeftColl,  L"L" },
    { IDC_ROT_CLOCKW, VRB_K_SPLIT_L, &s_RotRightColl, L"R" },
};
static const VRB_CONTROL s_Slideshow[] =
{
    { IDC_SLIDE_SHOW, VRB_K_SPLIT_L, &s_SlideColl, L"S" },
};
static const VRB_CONTROL s_File[] =
{
    { IDC_DELETE, VRB_K_LARGE, NULL, L"D" },
    { IDC_SAVEAS, VRB_K_SMALL, NULL, L"V" },
    { IDC_PRINT,  VRB_K_SMALL, NULL, L"T" },
    { IDC_MODIFY, VRB_K_SMALL, NULL, L"E" },
};
static const VRB_CONTROL s_Display[] =
{
    { IDC_TOGGLEFULLSCREEN,      VRB_K_TOGGLE_L, NULL, L"F" },
    { VRB_LWUI_STATUSBAR_TOGGLE, VRB_K_TOGGLE_S, NULL, L"S" },
    { VRB_LWUI_RIBBON_MINIMIZE,  VRB_K_TOGGLE_S, NULL, L"M" },
};
static const VRB_CONTROL s_HelpGroup[] =
{
    { IDC_HELP_TOC, VRB_K_SMALL, NULL, L"H" },
};

#define NELEM(a) ((UINT)(sizeof(a) / sizeof((a)[0])))

static const VRB_GROUP s_HomeGroups[] =
{
    { VRB_IDS_GRP_NAVIGATE,  VRB_V_LARGE_COLLAPSED, NELEM(s_Navigate),  s_Navigate },
    { VRB_IDS_GRP_ZOOM,      VRB_V_FULL,            NELEM(s_ZoomHome),  s_ZoomHome },
    { VRB_IDS_GRP_ROTATE,    VRB_V_LARGE_COLLAPSED, NELEM(s_Rotate),    s_Rotate },
    { VRB_IDS_GRP_SLIDESHOW, VRB_V_LARGE_COLLAPSED, NELEM(s_Slideshow), s_Slideshow },
    { VRB_IDS_GRP_FILE,      VRB_V_MIXED,           NELEM(s_File),      s_File },
};
static const VRB_GROUP s_ViewGroups[] =
{
    { VRB_IDS_GRP_ZOOM,    VRB_V_FULL,        NELEM(s_ZoomView),  s_ZoomView },
    { VRB_IDS_GRP_DISPLAY, VRB_V_LARGE_SMALL, NELEM(s_Display),   s_Display },
    { VRB_IDS_GRP_HELP,    VRB_V_SMALL,       NELEM(s_HelpGroup), s_HelpGroup },
};
static const VRB_TAB s_Tabs[] =
{
    { VRB_IDS_TAB_HOME, L"H", NELEM(s_HomeGroups), s_HomeGroups },
    { VRB_IDS_TAB_VIEW, L"V", NELEM(s_ViewGroups), s_ViewGroups },
};

static const VRB_MENUROW s_AppMenu[] =
{
    { VRB_CMD_OPEN,      L"O" },
    { IDC_SAVEAS,        L"A" },
    { IDC_PRINT,         L"P" },
    { IDC_MODIFY,        L"E" },
    { VRB_CMD_WALLPAPER, L"B" },
    { IDC_HELP_TOC,      L"H" },
    { VRB_CMD_EXIT,      L"X" },
};
static const UINT s_QatDefault[] =
{
    IDC_PREV_PIC, IDC_NEXT_PIC, IDC_ROT_COUNCW, IDC_ROT_CLOCKW
};

const VRB_LAYOUT g_VrbLayout =
{
    NELEM(s_Tabs), s_Tabs,
    NELEM(s_AppMenu), s_AppMenu,
    NELEM(s_QatDefault), s_QatDefault,
    L"F"
};

/* ---- lookups ---- */

const VRB_CMDDEF *
VRB_FindCommand(UINT id)
{
    UINT i;
    for (i = 0; i < g_VrbCommandCount; ++i)
    {
        if (g_VrbCommands[i].id == id)
            return &g_VrbCommands[i];
    }
    return NULL;
}

UINT
VRB_LabelStringId(UINT id)
{
    if (id >= 0xF000 && id <= 0xF0FF)
        return VRB_IDS_LABEL_BASE + 0x100 + (id & 0xFF);
    return VRB_IDS_LABEL_BASE + id;
}

BOOL
VRB_ZoomPercentOfCommand(UINT id, UINT *percent)
{
    if (id < VRB_CMD_ZOOM_FIRST || id > VRB_CMD_ZOOM_LAST)
        return FALSE;
    if (percent)
        *percent = g_VrbZoomPercents[id - VRB_CMD_ZOOM_FIRST];
    return TRUE;
}

UINT
VRB_NearestZoomIndex(UINT percent)
{
    UINT i, best = 0, bestDist = (UINT)-1;
    for (i = 0; i < VRB_ZOOM_LEVELS; ++i)
    {
        UINT p = g_VrbZoomPercents[i];
        UINT d = p > percent ? p - percent : percent - p;
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

BOOL
VRB_EvalState(const VRB_HOST *host, UINT id, BOOL *enabled, BOOL *checked)
{
    const VRB_CMDDEF *def = VRB_FindCommand(id);

    if (enabled)
        *enabled = FALSE;
    if (checked)
        *checked = FALSE;
    if (!def || !host || !host->Predicate)
        return FALSE;

    if (enabled)
        *enabled = def->enable == VRB_P_ALWAYS ||
                   host->Predicate(host->ctx, def->enable);
    if (checked && def->checked != VRB_P_ALWAYS)
        *checked = host->Predicate(host->ctx, def->checked);
    return TRUE;
}

/* ---- validation ---- */

typedef struct
{
    UINT problems;
    WCHAR *err;
    UINT err_chars;
} VRB_VCTX;

static void
VrbProblem(VRB_VCTX *v, const WCHAR *what, UINT id)
{
    if (v->problems == 0 && v->err && v->err_chars)
        StringCchPrintfW(v->err, v->err_chars, L"%ls (%u)", what, id);
    v->problems++;
}

/* Keytips seen at one level: all must be non-empty and distinct. */
typedef struct
{
    const WCHAR *seen[32];
    UINT count;
} VRB_LEVEL;

static void
VrbLevelAdd(VRB_VCTX *v, VRB_LEVEL *lvl, const WCHAR *keytip, UINT id)
{
    UINT i;
    if (!keytip || !*keytip)
    {
        VrbProblem(v, L"missing keytip", id);
        return;
    }
    for (i = 0; i < lvl->count; ++i)
    {
        if (!wcscmp(lvl->seen[i], keytip))
        {
            VrbProblem(v, L"duplicate keytip", id);
            return;
        }
    }
    if (lvl->count < 32)
        lvl->seen[lvl->count++] = keytip;
}

static void
VrbCheckId(VRB_VCTX *v, UINT id)
{
    if (!VRB_FindCommand(id))
        VrbProblem(v, L"unknown command id", id);
}

UINT
VRB_ValidateModel(WCHAR *err, UINT err_chars)
{
    VRB_VCTX v;
    VRB_LEVEL lvl;
    UINT i, t, g, c, k;

    v.problems = 0;
    v.err = err;
    v.err_chars = err_chars;
    if (err && err_chars)
        err[0] = L'\0';

    /* Command table: unique ids, real predicates, framework flag matches. */
    for (i = 0; i < g_VrbCommandCount; ++i)
    {
        const VRB_CMDDEF *d = &g_VrbCommands[i];
        for (k = i + 1; k < g_VrbCommandCount; ++k)
        {
            if (g_VrbCommands[k].id == d->id)
                VrbProblem(&v, L"duplicate command id", d->id);
        }
        if (d->enable >= VRB_P_COUNT || d->checked >= VRB_P_COUNT)
            VrbProblem(&v, L"bad predicate", d->id);
        if (d->id >= 0xF000 && !d->framework)
            VrbProblem(&v, L"framework id not flagged", d->id);
        if (d->id < 0xF000 && d->framework)
            VrbProblem(&v, L"framework flag on an app id", d->id);
    }

    /* Tab strip level: tabs plus the application button. */
    lvl.count = 0;
    VrbLevelAdd(&v, &lvl, g_VrbLayout.app_keytip, VRB_LWUI_APPMENU);
    for (t = 0; t < g_VrbLayout.tab_count; ++t)
        VrbLevelAdd(&v, &lvl, g_VrbLayout.tabs[t].keytip, t);

    for (t = 0; t < g_VrbLayout.tab_count; ++t)
    {
        const VRB_TAB *tab = &g_VrbLayout.tabs[t];
        lvl.count = 0;
        for (g = 0; g < tab->count; ++g)
        {
            const VRB_GROUP *grp = &tab->groups[g];
            for (c = 0; c < grp->count; ++c)
            {
                const VRB_CONTROL *ctl = &grp->controls[c];
                VrbCheckId(&v, ctl->id);
                VrbLevelAdd(&v, &lvl, ctl->keytip, ctl->id);

                if (ctl->kind == VRB_K_GALLERY && !ctl->collection)
                    VrbProblem(&v, L"gallery without collection", ctl->id);
                if (ctl->collection)
                {
                    VRB_LEVEL menu;
                    const VRB_COLLECTION *co = ctl->collection;
                    menu.count = 0;
                    for (k = 0; k < co->count; ++k)
                    {
                        VrbCheckId(&v, co->ids[k]);
                        if (co->keytips)
                            VrbLevelAdd(&v, &menu, co->keytips[k], co->ids[k]);
                    }
                }
            }
        }
    }

    lvl.count = 0;
    for (i = 0; i < g_VrbLayout.appmenu_count; ++i)
    {
        VrbCheckId(&v, g_VrbLayout.appmenu[i].id);
        VrbLevelAdd(&v, &lvl, g_VrbLayout.appmenu[i].keytip, g_VrbLayout.appmenu[i].id);
    }

    if (g_VrbLayout.qat_count > 9)
        VrbProblem(&v, L"more QAT defaults than keytips 1-9", g_VrbLayout.qat_count);
    for (i = 0; i < g_VrbLayout.qat_count; ++i)
        VrbCheckId(&v, g_VrbLayout.qat_default[i]);

    /* Zoom commands 711-720 line up with the percent table. */
    for (i = 0; i < VRB_ZOOM_LEVELS; ++i)
    {
        UINT pct = 0;
        if (!VRB_ZoomPercentOfCommand(VRB_CMD_ZOOM_FIRST + i, &pct) || pct != g_VrbZoomPercents[i])
            VrbProblem(&v, L"zoom level mismatch", VRB_CMD_ZOOM_FIRST + i);
        if (i && g_VrbZoomPercents[i] <= g_VrbZoomPercents[i - 1])
            VrbProblem(&v, L"zoom levels not ascending", i);
    }
    return v.problems;
}

/* ---- registry: QAT and frame options ---- */

UINT
VRB_QatLoadFrom(const WCHAR *subkey, UINT *ids, UINT max)
{
    HKEY hKey;
    UINT32 raw[VRB_QAT_MAX];
    DWORD cb = sizeof(raw), type = 0;
    UINT n = 0, i, k;
    LONG rc;

    if (max > VRB_QAT_MAX)
        max = VRB_QAT_MAX;

    rc = RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_READ, &hKey);
    if (rc == ERROR_SUCCESS)
    {
        rc = RegQueryValueExW(hKey, L"Items", NULL, &type, (LPBYTE)raw, &cb);
        RegCloseKey(hKey);
        if (rc == ERROR_SUCCESS && type == REG_BINARY)
        {
            UINT have = cb / sizeof(UINT32);
            for (i = 0; i < have && n < max; ++i)
            {
                const VRB_CMDDEF *d = VRB_FindCommand(raw[i]);
                BOOL dup = FALSE;
                if (!d || d->framework)
                    continue;
                for (k = 0; k < n; ++k)
                    dup |= (ids[k] == raw[i]);
                if (!dup)
                    ids[n++] = raw[i];
            }
        }
    }
    if (n == 0)
    {
        for (i = 0; i < g_VrbLayout.qat_count && n < max; ++i)
            ids[n++] = g_VrbLayout.qat_default[i];
    }
    return n;
}

BOOL
VRB_QatSaveTo(const WCHAR *subkey, const UINT *ids, UINT count)
{
    HKEY hKey;
    UINT32 raw[VRB_QAT_MAX];
    UINT i;
    LONG rc;

    if (count > VRB_QAT_MAX)
        count = VRB_QAT_MAX;
    for (i = 0; i < count; ++i)
        raw[i] = ids[i];

    rc = RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (rc != ERROR_SUCCESS)
        return FALSE;
    rc = RegSetValueExW(hKey, L"Items", 0, REG_BINARY, (const BYTE *)raw, count * sizeof(UINT32));
    RegCloseKey(hKey);
    return rc == ERROR_SUCCESS;
}

UINT
VRB_QatLoad(UINT *ids, UINT max)
{
    return VRB_QatLoadFrom(REGKEY_QAT, ids, max);
}

BOOL
VRB_QatSave(const UINT *ids, UINT count)
{
    return VRB_QatSaveTo(REGKEY_QAT, ids, count);
}

static BOOL
VrbReadString(const WCHAR *name, WCHAR *buf, DWORD cch)
{
    HKEY hKey;
    DWORD cb = cch * sizeof(WCHAR), type = 0;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, REGKEY_ROOT, 0, KEY_READ, &hKey);
    if (rc != ERROR_SUCCESS)
        return FALSE;
    rc = RegQueryValueExW(hKey, name, NULL, &type, (LPBYTE)buf, &cb);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || type != REG_SZ || cb < sizeof(WCHAR))
        return FALSE;
    buf[cch - 1] = L'\0';
    return TRUE;
}

BOOL
VRB_ReadChromeIsRibbon(BOOL default_value)
{
    WCHAR buf[16];
    if (!VrbReadString(L"ChromeStyle", buf, 16))
        return default_value;
    if (!_wcsicmp(buf, L"ribbon"))
        return TRUE;
    if (!_wcsicmp(buf, L"pill"))
        return FALSE;
    return default_value;
}

BOOL
VRB_ReadStatusBarShown(void)
{
    HKEY hKey;
    DWORD val = 1, cb = sizeof(val), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REGKEY_ROOT, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(hKey, L"StatusBar", NULL, &type, (LPBYTE)&val, &cb) != ERROR_SUCCESS ||
            type != REG_DWORD)
            val = 1;
        RegCloseKey(hKey);
    }
    return val != 0;
}

void
VRB_WriteStatusBarShown(BOOL shown)
{
    HKEY hKey;
    DWORD val = shown ? 1 : 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REGKEY_ROOT, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, L"StatusBar", 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
        RegCloseKey(hKey);
    }
}
