/*
 * PROJECT:     ReactOS Picture and Fax Viewer
 * LICENSE:     GPL-2.0 (https://spdx.org/licenses/GPL-2.0)
 * PURPOSE:     Adapter between the viewer's ribbon model and LeewayUI
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <wchar.h>
#include <strsafe.h>
#include <leewayui/ribbon.h>
#include "resource.h"
#include "vribbon.h"

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define VRB_MAX_COMMANDS 48
#define VRB_LABEL_CHARS  64

struct VRB_RIBBON
{
    ILeewayRibbon *fw;
    HINSTANCE instance;
    VRB_HOST host;
    LeewayUICommand table[VRB_MAX_COMMANDS];
    WCHAR labels[VRB_MAX_COMMANDS][VRB_LABEL_CHARS];
    WCHAR tips[VRB_MAX_COMMANDS][VRB_LABEL_CHARS * 2];
    WCHAR zoomLabel[VRB_LABEL_CHARS];
    /* Layout converted for the framework. SetLayout copies; the nodes stay
     * here so the conversion is plain data and easy to inspect. */
    WCHAR tabLabels[8][VRB_LABEL_CHARS];
    WCHAR groupLabels[32][VRB_LABEL_CHARS];
    LeewayUITab fwTabs[8];
    LeewayUIGroup fwGroups[32];
    LeewayUIPlacement fwPlace[96];
    LeewayUIAppMenuItem fwMenu[16];
    LeewayUIAppMenu fwAppMenu;
    LeewayUIQATItem fwQatItems[VRB_QAT_MAX];
    LeewayUIQAT fwQat;
};

BOOL
VRB_RibbonIsDefaultChrome(void)
{
    return VRB_ReadChromeIsRibbon(TRUE);
}

static void
LoadText(HINSTANCE inst, UINT ids, WCHAR *buf, UINT cch)
{
    buf[0] = L'\0';
    if (ids)
        LoadStringW(inst, ids, buf, (int)cch);
}

/* ---- framework callbacks (F0 shape: one execute, one query-state) ---- */

static HRESULT
FwExecute(UINT32 command_id, void *ctx)
{
    return VRB_RibbonExecute((VRB_RIBBON *)ctx, command_id);
}

static HRESULT
FwQueryState(UINT32 command_id, LeewayUICommand *out, void *ctx)
{
    return VRB_RibbonQueryState((VRB_RIBBON *)ctx, command_id, out);
}

HRESULT
VRB_RibbonExecute(VRB_RIBBON *ribbon, UINT command_id)
{
    UINT percent;

    if (!ribbon || !ribbon->host.Execute)
        return E_INVALIDARG;
    if (!VRB_FindCommand(command_id) && !(command_id >= 0xF001 && command_id <= 0xF005))
        return E_INVALIDARG;
    /* The zoom gallery itself only opens; its cells are 711-720. */
    if (command_id == VRB_CMD_ZOOMLEVEL)
        return S_FALSE;
    if (VRB_ZoomPercentOfCommand(command_id, &percent))
    {
        BOOL enabled = FALSE;
        VRB_EvalState(&ribbon->host, command_id, &enabled, NULL);
        if (!enabled)
            return S_FALSE;
    }
    return ribbon->host.Execute(ribbon->host.ctx, command_id);
}

HRESULT
VRB_RibbonQueryState(VRB_RIBBON *ribbon, UINT command_id, LeewayUICommand *out)
{
    UINT i;
    BOOL enabled, checked;

    if (!ribbon || !out)
        return E_INVALIDARG;
    for (i = 0; i < g_VrbCommandCount; ++i)
    {
        if (ribbon->table[i].id == command_id)
            break;
    }
    if (i >= g_VrbCommandCount)
    {
        /* Framework buttons the model does not list (help, maximize, menu):
         * always available, the host decides what they do. */
        if (command_id < 0xF001 || command_id > 0xF005)
            return E_INVALIDARG;
        ZeroMemory(out, sizeof(*out));
        out->id = command_id;
        out->enabled = TRUE;
        out->visible = TRUE;
        return S_OK;
    }

    *out = ribbon->table[i];
    VRB_EvalState(&ribbon->host, command_id, &enabled, &checked);
    out->enabled = enabled;
    out->checked = checked;
    out->visible = TRUE;
    if (command_id == VRB_CMD_ZOOMLEVEL && ribbon->host.GetZoomPercent)
    {
        /* The gallery's label is the current zoom. */
        UINT pct = ribbon->host.GetZoomPercent(ribbon->host.ctx);
        StringCchPrintfW(ribbon->zoomLabel, VRB_LABEL_CHARS, L"%u%%", pct);
        out->label = ribbon->zoomLabel;
    }
    return S_OK;
}

/* ---- layout conversion (ribbon.h tree) ---- */

static LeewayUIControlKind
ToFwKind(VRB_KIND k)
{
    switch (k)
    {
        case VRB_K_LARGE:       return LWUI_CONTROL_BUTTON_LARGE;
        case VRB_K_SMALL:       return LWUI_CONTROL_BUTTON_SMALL;
        case VRB_K_SMALL_ICON:  return LWUI_CONTROL_BUTTON_SMALL_ICON;
        case VRB_K_TOGGLE_L:    return LWUI_CONTROL_TOGGLE_LARGE;
        case VRB_K_TOGGLE_S:    return LWUI_CONTROL_TOGGLE_SMALL;
        case VRB_K_SPLIT_L:     return LWUI_CONTROL_SPLIT_LARGE;
        case VRB_K_SPLIT_S:     return LWUI_CONTROL_SPLIT_SMALL;
        case VRB_K_DROPDOWN:    return LWUI_CONTROL_DROPDOWN;
        case VRB_K_GALLERY:     return LWUI_CONTROL_GALLERY;
    }
    return LWUI_CONTROL_BUTTON_LARGE;
}

static HRESULT
SetFrameworkLayout(VRB_RIBBON *r)
{
    UINT t, g, c, k, nGroup = 0, nCtl = 0, nQat;
    const VRB_LAYOUT *L = &g_VrbLayout;
    HRESULT hr;

    if (L->tab_count > 8 || L->appmenu_count > 16)
        return E_FAIL;

    for (t = 0; t < L->tab_count; ++t)
    {
        const VRB_TAB *tab = &L->tabs[t];
        UINT firstGroup = nGroup;

        LoadText(r->instance, tab->ids_label, r->tabLabels[t], VRB_LABEL_CHARS);
        for (g = 0; g < tab->count; ++g)
        {
            const VRB_GROUP *grp = &tab->groups[g];
            UINT firstCtl = nCtl;
            LeewayUIGroup *fg;

            if (nGroup >= 32)
                return E_FAIL;
            LoadText(r->instance, grp->ids_label, r->groupLabels[nGroup], VRB_LABEL_CHARS);
            for (c = 0; c < grp->count; ++c)
            {
                const VRB_CONTROL *ctl = &grp->controls[c];
                LeewayUIPlacement *p;

                if (nCtl >= 96)
                    return E_FAIL;
                p = &r->fwPlace[nCtl];
                p->command_id = ctl->id;
                p->kind = ToFwKind(ctl->kind);
                p->keytip = ctl->keytip;
                p->sibling = (c + 1 < grp->count) ? &r->fwPlace[nCtl + 1] : NULL;
                ++nCtl;
            }
            fg = &r->fwGroups[nGroup];
            fg->label = r->groupLabels[nGroup];
            fg->keytip = NULL;
            fg->min_columns = 0;
            fg->max_columns = 0;
            fg->variant_order = 0;      /* framework-set; the model's variant order is not passed */
            fg->launcher_id = 0;
            fg->controls = &r->fwPlace[firstCtl];
            fg->sibling = (g + 1 < tab->count) ? &r->fwGroups[nGroup + 1] : NULL;
            ++nGroup;
        }
        r->fwTabs[t].id = 100 + t;
        r->fwTabs[t].label = r->tabLabels[t];
        r->fwTabs[t].keytip = tab->keytip;
        r->fwTabs[t].groups = &r->fwGroups[firstGroup];
        r->fwTabs[t].sibling = (t + 1 < L->tab_count) ? &r->fwTabs[t + 1] : NULL;
    }

    hr = LeewayRibbonSetLayout(r->fw, &r->fwTabs[0]);
    if (FAILED(hr))
        return hr;

    for (k = 0; k < L->appmenu_count; ++k)
    {
        LeewayUIAppMenuItem *m = &r->fwMenu[k];
        UINT idx;

        m->command_id = L->appmenu[k].id;
        m->kind = LWUI_CONTROL_BUTTON_LARGE;
        m->label = NULL;
        for (idx = 0; idx < g_VrbCommandCount; ++idx)
        {
            if (g_VrbCommands[idx].id == m->command_id)
                m->label = r->labels[idx];
        }
        m->sibling = (k + 1 < L->appmenu_count) ? &r->fwMenu[k + 1] : NULL;
    }
    r->fwAppMenu.items = L->appmenu_count ? &r->fwMenu[0] : NULL;
    hr = LeewayRibbonSetAppMenu(r->fw, &r->fwAppMenu);
    if (FAILED(hr))
        return hr;

    /* QAT: defaults first, then whatever the user saved under
     * HKCU\Software\Leeway\PhotoViewer\QAT (the framework owns persistence). */
    nQat = L->qat_count > VRB_QAT_MAX ? VRB_QAT_MAX : L->qat_count;
    for (k = 0; k < nQat; ++k)
    {
        r->fwQatItems[k].command_id = L->qat_default[k];
        r->fwQatItems[k].kind = LWUI_CONTROL_BUTTON_SMALL;
    }
    r->fwQat.items = r->fwQatItems;
    r->fwQat.count = nQat;
    hr = LeewayRibbonSetQAT(r->fw, &r->fwQat);
    if (SUCCEEDED(hr))
        LeewayRibbonLoadQATFromRegistry(r->fw, HKEY_CURRENT_USER, L"PhotoViewer");  /* absent is fine */
    return hr;
}

/* ---- lifetime ---- */

HRESULT
VRB_RibbonCreate(HWND parent, HINSTANCE instance, const VRB_HOST *host, VRB_RIBBON **out)
{
    VRB_RIBBON *r;
    UINT i;
    HRESULT hr;
    WCHAR err[96];

    if (!parent || !host || !out)
        return E_INVALIDARG;
    *out = NULL;
    if (g_VrbCommandCount > VRB_MAX_COMMANDS)
        return E_FAIL;

#if DBG
    if (VRB_ValidateModel(err, _countof(err)))
    {
        OutputDebugStringW(err);
        return E_FAIL;
    }
#else
    (void)err;
#endif

    r = (VRB_RIBBON *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*r));
    if (!r)
        return E_OUTOFMEMORY;
    r->instance = instance;
    r->host = *host;

    for (i = 0; i < g_VrbCommandCount; ++i)
    {
        const VRB_CMDDEF *d = &g_VrbCommands[i];
        LeewayUICommand *c = &r->table[i];

        LoadText(instance, VRB_LabelStringId(d->id), r->labels[i], VRB_LABEL_CHARS);
        if (d->ids_tip)
            LoadText(instance, d->ids_tip, r->tips[i], _countof(r->tips[i]));
        if (!r->tips[i][0])
            lstrcpynW(r->tips[i], r->labels[i], _countof(r->tips[i]));

        c->id = d->id;
        c->label = r->labels[i];
        c->tooltip = r->tips[i];
        c->keytip = d->keytip;
        c->icon_id = d->icon_bmp;   /* 0: the framework draws its placeholder */
        c->enabled = TRUE;
        c->checked = FALSE;
        c->visible = TRUE;
    }

    hr = LeewayRibbonCreate(parent, r->table, g_VrbCommandCount,
                            FwExecute, FwQueryState, r, &r->fw);
    if (FAILED(hr))
    {
        HeapFree(GetProcessHeap(), 0, r);
        return hr;
    }

    hr = SetFrameworkLayout(r);
    if (FAILED(hr))
    {
        LeewayRibbonDestroy(r->fw);
        HeapFree(GetProcessHeap(), 0, r);
        return hr;
    }

    *out = r;
    return S_OK;
}

void
VRB_RibbonDestroy(VRB_RIBBON *ribbon)
{
    if (!ribbon)
        return;
    LeewayRibbonDestroy(ribbon->fw);
    HeapFree(GetProcessHeap(), 0, ribbon);
}

HWND
VRB_RibbonWindow(VRB_RIBBON *ribbon)
{
    return ribbon ? LeewayRibbonGetHWND(ribbon->fw) : NULL;
}

UINT
VRB_RibbonHeight(VRB_RIBBON *ribbon)
{
    return ribbon ? LeewayRibbonGetHeight(ribbon->fw) : 0;
}

void
VRB_RibbonInvalidate(VRB_RIBBON *ribbon, UINT command_id)
{
    if (!ribbon)
        return;
    LeewayRibbonInvalidateCommand(ribbon->fw, command_id);
}

void
VRB_RibbonInvalidateAll(VRB_RIBBON *ribbon)
{
    if (!ribbon)
        return;
    LeewayRibbonInvalidateAll(ribbon->fw);
}

void
VRB_RibbonOnThemeChanged(VRB_RIBBON *ribbon)
{
    LeewayUITheme th;
    COLORREF face;

    if (!ribbon)
        return;
    face = GetSysColor(COLOR_BTNFACE);
    th.text_color = GetSysColor(COLOR_BTNTEXT);
    th.bg_color = face;
    th.border_color = GetSysColor(COLOR_BTNSHADOW);
    th.hover_color = GetSysColor(COLOR_HOTLIGHT);
    th.pressed_color = GetSysColor(COLOR_HIGHLIGHT);
    th.disabled_color = GetSysColor(COLOR_GRAYTEXT);
    th.dark_mode = (GetRValue(face) * 30 + GetGValue(face) * 59 + GetBValue(face) * 11) / 100 < 128;
    LeewayRibbonSetTheme(ribbon->fw, &th);
}

void
VRB_RibbonOnDpiChanged(VRB_RIBBON *ribbon, UINT dpi)
{
    if (ribbon)
        LeewayRibbonSetDPI(ribbon->fw, dpi);
}
