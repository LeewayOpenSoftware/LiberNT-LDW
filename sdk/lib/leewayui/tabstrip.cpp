/*
 * PROJECT:     LiberNT LeewayUI Framework
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tab strip control implementation
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 */

#include <windows.h>
#include <leewayui/tabstrip.h>

/* Window class name for tabstrip */
#define LWUI_TABSTRIP_CLASS L"LeewayUITabstrip"

/* Tab internal structure */
typedef struct _LWUI_TAB_INTERNAL {
    WCHAR *display_name;
    PVOID user_context;
    DWORD flags;
    HICON icon;
    SIZE label_size;  /* Cached text size in pixels */
    INT x, cx;        /* Position and width in tabstrip */
} LWUI_TAB_INTERNAL, *PLWUI_TAB_INTERNAL;

/* Tabstrip internal structure */
typedef struct _LWUI_TABSTRIP_INTERNAL {
    HWND hwnd;
    HWND parent_window;
    HWND notify_window;
    UINT notify_id;
    PLWUI_TAB_INTERNAL *tabs;
    UINT tab_count;
    UINT tab_capacity;
    INT selected_index;
    UINT dpi;
    /* TODO: MSAA/UIA provider interface */
} LWUI_TABSTRIP_INTERNAL, *PLWUI_TABSTRIP_INTERNAL;

/* ========== Static helpers ========== */

static LRESULT CALLBACK
LeewayUITabstripWndProc(
    _In_ HWND hwnd,
    _In_ UINT msg,
    _In_ WPARAM wparam,
    _In_ LPARAM lparam
);

static VOID
LeewayUITabstripNotify(
    _In_ PLWUI_TABSTRIP_INTERNAL pTabstrip,
    _In_ UINT code,
    _In_opt_ LPARAM lParam
)
{
    NMHDR hdr = {0};

    if (!pTabstrip || !pTabstrip->notify_window)
        return;

    hdr.hwndFrom = pTabstrip->hwnd;
    hdr.idFrom = pTabstrip->notify_id;
    hdr.code = code;

    SendMessageW(
        pTabstrip->notify_window,
        WM_NOTIFY,
        (WPARAM)pTabstrip->notify_id,
        (LPARAM)&hdr
    );

    /* TODO: Pass additional context via lParam in extended notification */
}

/* ========== Public API Implementation ========== */

PLEEWAYUI_TABSTRIP
WINAPI
LeewayUITabstripCreate(
    _In_ CONST LEEWAYUI_TABSTRIP_PARAMS *pParams
)
{
    PLWUI_TABSTRIP_INTERNAL pTabstrip;
    HWND hwnd;

    if (!pParams || !pParams->parent_window)
        return NULL;

    /* Register window class if not already registered */
    static BOOL class_registered = FALSE;
    if (!class_registered)
    {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = LeewayUITabstripWndProc;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = LWUI_TABSTRIP_CLASS;
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);

        if (!RegisterClassW(&wc))
            return NULL;

        class_registered = TRUE;
    }

    /* Allocate internal structure */
    pTabstrip = (PLWUI_TABSTRIP_INTERNAL)HeapAlloc(
        GetProcessHeap(),
        HEAP_ZERO_MEMORY,
        sizeof(LWU_TABSTRIP_INTERNAL)
    );

    if (!pTabstrip)
        return NULL;

    pTabstrip->tab_capacity = 16;
    pTabstrip->tabs = (PLWUI_TAB_INTERNAL *)HeapAlloc(
        GetProcessHeap(),
        HEAP_ZERO_MEMORY,
        pTabstrip->tab_capacity * sizeof(PLWUI_TAB_INTERNAL)
    );

    if (!pTabstrip->tabs)
    {
        HeapFree(GetProcessHeap(), 0, pTabstrip);
        return NULL;
    }

    pTabstrip->parent_window = pParams->parent_window;
    pTabstrip->notify_window = pParams->notify_window;
    pTabstrip->notify_id = pParams->notify_id;
    pTabstrip->selected_index = -1;
    pTabstrip->dpi = 96;  /* Default to 96 DPI */

    /* Create window */
    hwnd = CreateWindowExW(
        pParams->ex_style,
        LWUI_TABSTRIP_CLASS,
        L"",
        pParams->style | WS_CHILD,
        pParams->x,
        pParams->y,
        pParams->cx,
        pParams->cy,
        pParams->parent_window,
        NULL,
        GetModuleHandleW(NULL),
        pTabstrip
    );

    if (!hwnd)
    {
        HeapFree(GetProcessHeap(), 0, pTabstrip->tabs);
        HeapFree(GetProcessHeap(), 0, pTabstrip);
        return NULL;
    }

    pTabstrip->hwnd = hwnd;

    return (PLEEWAYUI_TABSTRIP)pTabstrip;
}

BOOL
WINAPI
LeewayUITabstripDestroy(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip
)
{
    PLWUI_TABSTRIP_INTERNAL pTabstrip = (PLWUI_TABSTRIP_INTERNAL)hTabstrip;
    UINT i;

    if (!pTabstrip)
        return FALSE;

    if (IsWindow(pTabstrip->hwnd))
        DestroyWindow(pTabstrip->hwnd);

    /* Clean up tabs */
    for (i = 0; i < pTabstrip->tab_count; i++)
    {
        if (pTabstrip->tabs[i])
        {
            if (pTabstrip->tabs[i]->display_name)
                HeapFree(GetProcessHeap(), 0, pTabstrip->tabs[i]->display_name);

            if (pTabstrip->tabs[i]->icon)
                DestroyIcon(pTabstrip->tabs[i]->icon);

            HeapFree(GetProcessHeap(), 0, pTabstrip->tabs[i]);
        }
    }

    HeapFree(GetProcessHeap(), 0, pTabstrip->tabs);
    HeapFree(GetProcessHeap(), 0, pTabstrip);

    return TRUE;
}

HRESULT
WINAPI
LeewayUITabstripAddTab(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip,
    _In_ CONST LEEWAYUI_TAB_PARAMS *pTabParams,
    _Out_ PLEEWAYUI_TAB *pOutTab
)
{
    PLWUI_TABSTRIP_INTERNAL pTabstrip = (PLWUI_TABSTRIP_INTERNAL)hTabstrip;
    PLWUI_TAB_INTERNAL pNewTab;

    if (!pTabstrip || !pTabParams || !pOutTab)
        return E_INVALIDARG;

    /* Grow tab array if necessary */
    if (pTabstrip->tab_count >= pTabstrip->tab_capacity)
    {
        PLWUI_TAB_INTERNAL *pNewTabs;
        UINT new_capacity = pTabstrip->tab_capacity * 2;

        pNewTabs = (PLWUI_TAB_INTERNAL *)HeapReAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            pTabstrip->tabs,
            new_capacity * sizeof(PLWUI_TAB_INTERNAL)
        );

        if (!pNewTabs)
            return E_OUTOFMEMORY;

        pTabstrip->tabs = pNewTabs;
        pTabstrip->tab_capacity = new_capacity;
    }

    /* Allocate new tab */
    pNewTab = (PLWUI_TAB_INTERNAL)HeapAlloc(
        GetProcessHeap(),
        HEAP_ZERO_MEMORY,
        sizeof(LWUI_TAB_INTERNAL)
    );

    if (!pNewTab)
        return E_OUTOFMEMORY;

    /* Copy display name */
    if (pTabParams->display_name)
    {
        size_t name_len = wcslen(pTabParams->display_name) + 1;
        pNewTab->display_name = (WCHAR *)HeapAlloc(
            GetProcessHeap(),
            0,
            name_len * sizeof(WCHAR)
        );

        if (!pNewTab->display_name)
        {
            HeapFree(GetProcessHeap(), 0, pNewTab);
            return E_OUTOFMEMORY;
        }

        wcscpy_s(pNewTab->display_name, name_len, pTabParams->display_name);
    }

    pNewTab->user_context = pTabParams->user_context;
    pNewTab->flags = pTabParams->flags;
    pNewTab->icon = pTabParams->icon;  /* FIXME: Should duplicate icon for ownership */

    /* Add to array and register */
    pTabstrip->tabs[pTabstrip->tab_count] = pNewTab;
    pTabstrip->tab_count++;

    /* Auto-select first tab */
    if (pTabstrip->selected_index < 0)
    {
        pTabstrip->selected_index = 0;
        pNewTab->flags |= LWUI_TAB_SELECTED;
    }

    if (pTabstrip->hwnd)
        InvalidateRect(pTabstrip->hwnd, NULL, FALSE);

    *pOutTab = (PLEEWAYUI_TAB)pNewTab;
    return S_OK;
}

HRESULT
WINAPI
LeewayUITabstripRemoveTab(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip,
    _In_ PLEEWAYUI_TAB hTab
)
{
    PLWUI_TABSTRIP_INTERNAL pTabstrip = (PLWUI_TABSTRIP_INTERNAL)hTabstrip;
    PLWUI_TAB_INTERNAL pTab = (PLWUI_TAB_INTERNAL)hTab;
    UINT i;

    if (!pTabstrip || !pTab)
        return E_INVALIDARG;

    /* Find tab index */
    for (i = 0; i < pTabstrip->tab_count; i++)
    {
        if (pTabstrip->tabs[i] == pTab)
            break;
    }

    if (i >= pTabstrip->tab_count)
        return E_INVALIDARG;

    /* Send CLOSING notification */
    LeewayUITabstripNotify(pTabstrip, LWUI_TABSTRIP_NOTIFY_CLOSING, (LPARAM)pTab->user_context);

    /* TODO: Check if notification handler cancelled the close */

    /* Remove from array */
    if (pTab->display_name)
        HeapFree(GetProcessHeap(), 0, pTab->display_name);
    if (pTab->icon)
        DestroyIcon(pTab->icon);

    HeapFree(GetProcessHeap(), 0, pTab);

    /* Shift remaining tabs */
    for (UINT j = i; j < pTabstrip->tab_count - 1; j++)
        pTabstrip->tabs[j] = pTabstrip->tabs[j + 1];

    pTabstrip->tab_count--;

    /* Update selected index */
    if (pTabstrip->selected_index >= (INT)pTabstrip->tab_count && pTabstrip->tab_count > 0)
        pTabstrip->selected_index = pTabstrip->tab_count - 1;

    if (pTabstrip->hwnd)
        InvalidateRect(pTabstrip->hwnd, NULL, FALSE);

    LeewayUITabstripNotify(pTabstrip, LWUI_TABSTRIP_NOTIFY_CLOSED, 0);

    return S_OK;
}

HRESULT
WINAPI
LeewayUITabstripSelectTab(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip,
    _In_ PLEEWAYUI_TAB hTab
)
{
    PLWUI_TABSTRIP_INTERNAL pTabstrip = (PLWUI_TABSTRIP_INTERNAL)hTabstrip;
    PLWUI_TAB_INTERNAL pTab = (PLWUI_TAB_INTERNAL)hTab;
    UINT i;

    if (!pTabstrip || !pTab)
        return E_INVALIDARG;

    /* Find tab index */
    for (i = 0; i < pTabstrip->tab_count; i++)
    {
        if (pTabstrip->tabs[i] == pTab)
            break;
    }

    if (i >= pTabstrip->tab_count)
        return E_INVALIDARG;

    /* Update selected state */
    if (pTabstrip->selected_index >= 0 && pTabstrip->selected_index < (INT)pTabstrip->tab_count)
        pTabstrip->tabs[pTabstrip->selected_index]->flags &= ~LWUI_TAB_SELECTED;

    pTabstrip->selected_index = i;
    pTab->flags |= LWUI_TAB_SELECTED;

    LeewayUITabstripNotify(pTabstrip, LWUI_TABSTRIP_NOTIFY_SELCHANGED, (LPARAM)pTab->user_context);

    if (pTabstrip->hwnd)
        InvalidateRect(pTabstrip->hwnd, NULL, FALSE);

    return S_OK;
}

PLEEWAYUI_TAB
WINAPI
LeewayUITabstripGetSelectedTab(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip
)
{
    PLWUI_TABSTRIP_INTERNAL pTabstrip = (PLWUI_TABSTRIP_INTERNAL)hTabstrip;

    if (!pTabstrip || pTabstrip->selected_index < 0 || pTabstrip->selected_index >= (INT)pTabstrip->tab_count)
        return NULL;

    return (PLEEWAYUI_TAB)pTabstrip->tabs[pTabstrip->selected_index];
}

/* TODO: Implement remaining API functions */

UINT
WINAPI
LeewayUITabstripGetTabCount(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip
)
{
    PLWUI_TABSTRIP_INTERNAL pTabstrip = (PLWUI_TABSTRIP_INTERNAL)hTabstrip;

    if (!pTabstrip)
        return 0;

    return pTabstrip->tab_count;
}

HRESULT
WINAPI
LeewayUITabstripSetDPI(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip,
    _In_ UINT dpi
)
{
    PLWUI_TABSTRIP_INTERNAL pTabstrip = (PLWUI_TABSTRIP_INTERNAL)hTabstrip;

    if (!pTabstrip || dpi < 72 || dpi > 576)
        return E_INVALIDARG;

    pTabstrip->dpi = dpi;

    if (pTabstrip->hwnd)
        InvalidateRect(pTabstrip->hwnd, NULL, FALSE);

    return S_OK;
}

BOOL
WINAPI
LeewayUITabstripPreTranslateMessage(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip,
    _In_ CONST MSG *pMsg
)
{
    PLWUI_TABSTRIP_INTERNAL pTabstrip = (PLWUI_TABSTRIP_INTERNAL)hTabstrip;

    if (!pTabstrip || !pMsg)
        return FALSE;

    if (pMsg->message != WM_KEYDOWN)
        return FALSE;

    /* Ctrl+Tab: next tab */
    if (pMsg->wParam == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000))
    {
        if (!(GetKeyState(VK_SHIFT) & 0x8000))
            LeewayUITabstripNextTab(hTabstrip);
        else
            LeewayUITabstripPrevTab(hTabstrip);

        return TRUE;
    }

    /* Ctrl+W: close current tab */
    if (pMsg->wParam == 'W' && (GetKeyState(VK_CONTROL) & 0x8000))
    {
        LeewayUITabstripCloseSelectedTab(hTabstrip);
        return TRUE;
    }

    return FALSE;
}

/* ========== Remaining stub functions ========== */

PLEEWAYUI_TAB
WINAPI
LeewayUITabstripNextTab(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip
)
{
    /* TODO: Implement */
    return NULL;
}

PLEEWAYUI_TAB
WINAPI
LeewayUITabstripPrevTab(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip
)
{
    /* TODO: Implement */
    return NULL;
}

HRESULT
WINAPI
LeewayUITabstripCloseSelectedTab(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip
)
{
    /* TODO: Implement */
    return E_NOTIMPL;
}

/* Window procedure */
static LRESULT CALLBACK
LeewayUITabstripWndProc(
    _In_ HWND hwnd,
    _In_ UINT msg,
    _In_ WPARAM wparam,
    _In_ LPARAM lparam
)
{
    PLWUI_TABSTRIP_INTERNAL pTabstrip = NULL;

    if (msg == WM_CREATE)
    {
        CREATESTRUCT *pCreate = (CREATESTRUCT *)lparam;
        pTabstrip = (PLWUI_TABSTRIP_INTERNAL)pCreate->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pTabstrip);
    }
    else
    {
        pTabstrip = (PLWUI_TABSTRIP_INTERNAL)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }

    switch (msg)
    {
        case WM_PAINT:
        {
            /* TODO: Render tabs */
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (hdc)
            {
                FillRect(hdc, &ps.rcPaint, GetSysColorBrush(COLOR_WINDOW));
                EndPaint(hwnd, &ps);
            }
            return 0;
        }

        case WM_DESTROY:
            /* Window is being destroyed, don't delete pTabstrip here */
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

/* Additional stubs */
HRESULT WINAPI LeewayUITabstripSetTabLabel(_In_ PLEEWAYUI_TAB hTab, _In_ PCWSTR display_name) { return E_NOTIMPL; }
INT WINAPI LeewayUITabstripGetTabLabel(_In_ PLEEWAYUI_TAB hTab, _Out_writes_(cchBuffer) PWSTR pBuffer, _In_ INT cchBuffer) { return -1; }
HRESULT WINAPI LeewayUITabstripSetTabDirty(_In_ PLEEWAYUI_TAB hTab, _In_ BOOL is_dirty) { return E_NOTIMPL; }
BOOL WINAPI LeewayUITabstripGetTabDirty(_In_ PLEEWAYUI_TAB hTab) { return FALSE; }
PVOID WINAPI LeewayUITabstripSetTabUserContext(_In_ PLEEWAYUI_TAB hTab, _In_opt_ PVOID user_context) { return NULL; }
PVOID WINAPI LeewayUITabstripGetTabUserContext(_In_ PLEEWAYUI_TAB hTab) { return NULL; }
PLEEWAYUI_TAB WINAPI LeewayUITabstripGetTabByIndex(_In_ PLEEWAYUI_TABSTRIP hTabstrip, _In_ UINT index) { return NULL; }
HRESULT WINAPI LeewayUITabstripMoveTab(_In_ PLEEWAYUI_TABSTRIP hTabstrip, _In_ PLEEWAYUI_TAB hTab, _In_ UINT new_index) { return E_NOTIMPL; }
HRESULT WINAPI LeewayUITabstripClearAllTabs(_In_ PLEEWAYUI_TABSTRIP hTabstrip) { return E_NOTIMPL; }
