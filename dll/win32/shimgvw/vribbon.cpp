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

#ifdef VRB_HAVE_F1
#include "vribbon_f1.h"
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
    UINT qat[VRB_QAT_MAX];
    UINT qatCount;
#ifdef VRB_HAVE_F1
    /* Layout converted for the framework; the arrays must outlive the call
     * if the framework keeps pointers (it may), so they live here. */
    WCHAR tabLabels[8][VRB_LABEL_CHARS];
    WCHAR groupLabels[32][VRB_LABEL_CHARS];
    LeewayUITab fwTabs[8];
    LeewayUIGroup fwGroups[32];
    LeewayUIControl fwControls[96];
    LeewayUICollection fwColls[96];
    UINT32 fwCollIds[96][16];
    UINT32 fwAppMenu[16];
    UINT32 fwQat[VRB_QAT_MAX];
    LeewayUILayout fwLayout;
#endif
};

BOOL
VRB_RibbonIsDefaultChrome(void)
{
#ifdef VRB_HAVE_F1
    return VRB_ReadChromeIsRibbon(TRUE);
#else
    return VRB_ReadChromeIsRibbon(FALSE);
#endif
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
    if (!VRB_FindCommand(command_id))
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
        return E_INVALIDARG;

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

void
VRB_RibbonQatChanged(VRB_RIBBON *ribbon, const UINT *ids, UINT count)
{
    UINT i;
    if (!ribbon || !ids)
        return;
    if (count > VRB_QAT_MAX)
        count = VRB_QAT_MAX;
    for (i = 0; i < count; ++i)
        ribbon->qat[i] = ids[i];
    ribbon->qatCount = count;
    VRB_QatSave(ids, count);
}

/* ---- F1: layout conversion (spec 4.1) ---- */

#ifdef VRB_HAVE_F1

static LeewayUIKind
ToFwKind(VRB_KIND k)
{
    switch (k)
    {
        case VRB_K_LARGE:       return LWUI_K_LARGE;
        case VRB_K_SMALL:       return LWUI_K_SMALL;
        case VRB_K_SMALL_ICON:  return LWUI_K_SMALL_ICON;
        case VRB_K_TOGGLE_L:    return LWUI_K_TOGGLE_L;
        case VRB_K_TOGGLE_S:    return LWUI_K_TOGGLE_S;
        case VRB_K_SPLIT_L:     return LWUI_K_SPLIT_L;
        case VRB_K_SPLIT_S:     return LWUI_K_SPLIT_S;
        case VRB_K_DROPDOWN:    return LWUI_K_DROPDOWN;
        case VRB_K_GALLERY:     return LWUI_K_GALLERY;
    }
    return LWUI_K_LARGE;
}

static HRESULT
SetFrameworkLayout(VRB_RIBBON *r)
{
    UINT t, g, c, k, nGroup = 0, nCtl = 0, nColl = 0;
    const VRB_LAYOUT *L = &g_VrbLayout;

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
            if (nGroup >= 32)
                return E_FAIL;
            LoadText(r->instance, grp->ids_label, r->groupLabels[nGroup], VRB_LABEL_CHARS);
            for (c = 0; c < grp->count; ++c)
            {
                const VRB_CONTROL *ctl = &grp->controls[c];
                LeewayUIControl *out;
                if (nCtl >= 96)
                    return E_FAIL;
                out = &r->fwControls[nCtl];
                out->id = ctl->id;
                out->kind = ToFwKind(ctl->kind);
                out->keytip = ctl->keytip;
                out->collection = NULL;
                if (ctl->collection)
                {
                    const VRB_COLLECTION *co = ctl->collection;
                    if (nColl >= 96 || co->count > 16)
                        return E_FAIL;
                    for (k = 0; k < co->count; ++k)
                        r->fwCollIds[nColl][k] = co->ids[k];
                    r->fwColls[nColl].count = co->count;
                    r->fwColls[nColl].ids = r->fwCollIds[nColl];
                    r->fwColls[nColl].last_choice_becomes_primary = co->last_choice_becomes_primary;
                    out->collection = &r->fwColls[nColl];
                    ++nColl;
                }
                ++nCtl;
            }
            r->fwGroups[nGroup].label = r->groupLabels[nGroup];
            r->fwGroups[nGroup].keytip = NULL;
            r->fwGroups[nGroup].variant_order = (UINT32)grp->variants;
            r->fwGroups[nGroup].launcher_id = 0;
            r->fwGroups[nGroup].count = grp->count;
            r->fwGroups[nGroup].controls = &r->fwControls[firstCtl];
            ++nGroup;
        }
        r->fwTabs[t].label = r->tabLabels[t];
        r->fwTabs[t].keytip = tab->keytip;
        r->fwTabs[t].context = NULL;
        r->fwTabs[t].count = tab->count;
        r->fwTabs[t].groups = &r->fwGroups[firstGroup];
    }
    for (k = 0; k < L->appmenu_count; ++k)
        r->fwAppMenu[k] = L->appmenu[k].id;
    for (k = 0; k < r->qatCount; ++k)
        r->fwQat[k] = r->qat[k];

    r->fwLayout.tab_count = L->tab_count;
    r->fwLayout.tabs = r->fwTabs;
    r->fwLayout.appmenu_count = L->appmenu_count;
    r->fwLayout.appmenu_ids = r->fwAppMenu;
    r->fwLayout.qat_count = r->qatCount;
    r->fwLayout.qat_default_ids = r->fwQat;
    return LeewayRibbonSetLayout(r->fw, &r->fwLayout);
}

#endif /* VRB_HAVE_F1 */

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

    r->qatCount = VRB_QatLoad(r->qat, VRB_QAT_MAX);

    hr = LeewayRibbonCreate(parent, r->table, g_VrbCommandCount,
                            FwExecute, FwQueryState, r, &r->fw);
    if (FAILED(hr))
    {
        HeapFree(GetProcessHeap(), 0, r);
        return hr;
    }

#ifdef VRB_HAVE_F1
    hr = SetFrameworkLayout(r);
    if (FAILED(hr))
    {
        LeewayRibbonDestroy(r->fw);
        HeapFree(GetProcessHeap(), 0, r);
        return hr;
    }
#endif

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
    return ribbon ? LeewayRibbonGetWindowHandle(ribbon->fw) : NULL;
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
#ifdef VRB_HAVE_F1
    LeewayRibbonInvalidateCommand(ribbon->fw, command_id);
#else
    /* F0: the skeleton re-reads nothing; repaint is all there is. */
    (void)command_id;
    InvalidateRect(VRB_RibbonWindow(ribbon), NULL, FALSE);
#endif
}

void
VRB_RibbonInvalidateAll(VRB_RIBBON *ribbon)
{
    if (!ribbon)
        return;
#ifdef VRB_HAVE_F1
    LeewayRibbonInvalidateAll(ribbon->fw);
#else
    InvalidateRect(VRB_RibbonWindow(ribbon), NULL, FALSE);
#endif
}

void
VRB_RibbonOnThemeChanged(VRB_RIBBON *ribbon)
{
    if (ribbon)
        LeewayRibbonOnThemeChanged(ribbon->fw);
}

void
VRB_RibbonOnDpiChanged(VRB_RIBBON *ribbon, UINT dpi)
{
    if (ribbon)
        LeewayRibbonOnDpiChanged(ribbon->fw, dpi);
}
