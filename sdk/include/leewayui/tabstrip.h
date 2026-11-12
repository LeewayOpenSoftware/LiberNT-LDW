/*
 * PROJECT:     LiberNT LeewayUI Framework
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tab strip control for document-based applications
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 */

#ifndef _LEEWAYUI_TABSTRIP_H_
#define _LEEWAYUI_TABSTRIP_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct _LEEWAYUI_TABSTRIP LEEWAYUI_TABSTRIP, *PLEEWAYUI_TABSTRIP;
typedef struct _LEEWAYUI_TAB LEEWAYUI_TAB, *PLEEWAYUI_TAB;

/* Tab flags */
#define LWUI_TAB_DEFAULT            0x00000000
#define LWUI_TAB_DIRTY              0x00000001  /* Tab document is unsaved */
#define LWUI_TAB_SELECTED           0x00000002  /* Tab is currently active */
#define LWUI_TAB_PINNED             0x00000004  /* Tab cannot be closed by user */

/* Tab colors (theme-aware, use theme system values) */
typedef struct _LEEWAYUI_TAB_COLORS {
    COLORREF foreground;        /* Text color */
    COLORREF background;        /* Background color */
    COLORREF hover_background;  /* Hover background */
    COLORREF active_background; /* Active tab background */
} LEEWAYUI_TAB_COLORS, *PLEEWAYUI_TAB_COLORS;

/* Tab creation parameters */
typedef struct _LEEWAYUI_TAB_PARAMS {
    PCWSTR display_name;         /* Tab label (shown in UI) */
    PVOID user_context;          /* App-defined context for this tab */
    DWORD flags;                 /* LWUI_TAB_* flags */
    HICON icon;                  /* Optional tab icon (16x16 or 32x32) */
} LEEWAYUI_TAB_PARAMS, *PLEEWAYUI_TAB_PARAMS;

/* Notification codes for tab events */
#define LWUI_TABSTRIP_NOTIFY_SELCHANGED    0x0001  /* Tab selection changed; lParam = new tab context */
#define LWUI_TABSTRIP_NOTIFY_CLOSING       0x0002  /* Tab close requested; lParam = tab context; return TRUE to allow */
#define LWUI_TABSTRIP_NOTIFY_CLOSED        0x0003  /* Tab closed; lParam = tab context */
#define LWUI_TABSTRIP_NOTIFY_REORDERED     0x0004  /* Tab order changed; lParam = NULL */
#define LWUI_TABSTRIP_NOTIFY_DIRTY         0x0005  /* Tab dirty state changed; lParam = MAKELPARAM(context, is_dirty) */

/* Tabstrip creation parameters */
typedef struct _LEEWAYUI_TABSTRIP_PARAMS {
    HWND parent_window;          /* Parent window for tabstrip */
    INT x, y, cx, cy;            /* Position and size (relative to parent) */
    DWORD style;                 /* Window style flags (WS_CHILD, WS_VISIBLE, etc.) */
    DWORD ex_style;              /* Extended window style flags */
    HWND notify_window;          /* Window that receives WM_NOTIFY messages */
    UINT notify_id;              /* Control ID for WM_NOTIFY messages */
} LEEWAYUI_TABSTRIP_PARAMS, *PLEEWAYUI_TABSTRIP_PARAMS;

/* ========== Tabstrip API ========== */

/**
 * LeewayUITabstripCreate
 * Creates a new tabstrip control.
 *
 * Parameters:
 *   pParams     Pointer to LEEWAYUI_TABSTRIP_PARAMS structure
 *
 * Returns:
 *   Handle to the new tabstrip control, or NULL on failure
 */
PLEEWAYUI_TABSTRIP
WINAPI
LeewayUITabstripCreate(
    _In_ CONST LEEWAYUI_TABSTRIP_PARAMS *pParams
);

/**
 * LeewayUITabstripDestroy
 * Destroys a tabstrip control.
 *
 * Parameters:
 *   hTabstrip   Handle to tabstrip (from LeewayUITabstripCreate)
 *
 * Returns:
 *   TRUE if successful, FALSE otherwise
 */
BOOL
WINAPI
LeewayUITabstripDestroy(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip
);

/**
 * LeewayUITabstripAddTab
 * Adds a new tab to the tabstrip.
 *
 * Parameters:
 *   hTabstrip   Handle to tabstrip
 *   pTabParams  Pointer to LEEWAYUI_TAB_PARAMS structure
 *   pOutTab     Receives pointer to new tab handle
 *
 * Returns:
 *   S_OK on success, or error code on failure
 */
HRESULT
WINAPI
LeewayUITabstripAddTab(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip,
    _In_ CONST LEEWAYUI_TAB_PARAMS *pTabParams,
    _Out_ PLEEWAYUI_TAB *pOutTab
);

/**
 * LeewayUITabstripRemoveTab
 * Removes a tab from the tabstrip.
 * Sends LWUI_TABSTRIP_NOTIFY_CLOSING; if handler returns FALSE, removal is cancelled.
 *
 * Parameters:
 *   hTabstrip   Handle to tabstrip
 *   hTab        Handle to tab to remove
 *
 * Returns:
 *   S_OK if removed, S_FALSE if cancelled by handler, error code on failure
 */
HRESULT
WINAPI
LeewayUITabstripRemoveTab(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip,
    _In_ PLEEWAYUI_TAB hTab
);

/**
 * LeewayUITabstripSelectTab
 * Selects (activates) a specific tab.
 *
 * Parameters:
 *   hTabstrip   Handle to tabstrip
 *   hTab        Handle to tab to select
 *
 * Returns:
 *   S_OK on success, or error code on failure
 */
HRESULT
WINAPI
LeewayUITabstripSelectTab(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip,
    _In_ PLEEWAYUI_TAB hTab
);

/**
 * LeewayUITabstripGetSelectedTab
 * Retrieves the currently selected tab.
 *
 * Parameters:
 *   hTabstrip   Handle to tabstrip
 *
 * Returns:
 *   Handle to selected tab, or NULL if no tab is selected
 */
PLEEWAYUI_TAB
WINAPI
LeewayUITabstripGetSelectedTab(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip
);

/**
 * LeewayUITabstripSetTabLabel
 * Changes a tab's display label.
 *
 * Parameters:
 *   hTab        Handle to tab
 *   display_name New label text (null-terminated Unicode string)
 *
 * Returns:
 *   S_OK on success, or error code on failure
 */
HRESULT
WINAPI
LeewayUITabstripSetTabLabel(
    _In_ PLEEWAYUI_TAB hTab,
    _In_ PCWSTR display_name
);

/**
 * LeewayUITabstripGetTabLabel
 * Retrieves a tab's display label.
 *
 * Parameters:
 *   hTab        Handle to tab
 *   pBuffer     Receives label text (caller-allocated buffer)
 *   cchBuffer   Size of pBuffer in characters (including null terminator)
 *
 * Returns:
 *   Number of characters copied (not including null terminator), or -1 on error
 */
INT
WINAPI
LeewayUITabstripGetTabLabel(
    _In_ PLEEWAYUI_TAB hTab,
    _Out_writes_(cchBuffer) PWSTR pBuffer,
    _In_ INT cchBuffer
);

/**
 * LeewayUITabstripSetTabDirty
 * Sets the dirty flag for a tab.
 *
 * Parameters:
 *   hTab        Handle to tab
 *   is_dirty    TRUE to mark tab as unsaved, FALSE to mark as clean
 *
 * Returns:
 *   S_OK on success, or error code on failure
 */
HRESULT
WINAPI
LeewayUITabstripSetTabDirty(
    _In_ PLEEWAYUI_TAB hTab,
    _In_ BOOL is_dirty
);

/**
 * LeewayUITabstripGetTabDirty
 * Retrieves the dirty flag for a tab.
 *
 * Parameters:
 *   hTab        Handle to tab
 *
 * Returns:
 *   TRUE if tab is marked as unsaved, FALSE otherwise
 */
BOOL
WINAPI
LeewayUITabstripGetTabDirty(
    _In_ PLEEWAYUI_TAB hTab
);

/**
 * LeewayUITabstripSetTabUserContext
 * Associates application-defined data with a tab.
 *
 * Parameters:
 *   hTab        Handle to tab
 *   user_context New context pointer
 *
 * Returns:
 *   Previous context pointer, or NULL if none was set
 */
PVOID
WINAPI
LeewayUITabstripSetTabUserContext(
    _In_ PLEEWAYUI_TAB hTab,
    _In_opt_ PVOID user_context
);

/**
 * LeewayUITabstripGetTabUserContext
 * Retrieves application-defined data associated with a tab.
 *
 * Parameters:
 *   hTab        Handle to tab
 *
 * Returns:
 *   User context pointer, or NULL if none was set
 */
PVOID
WINAPI
LeewayUITabstripGetTabUserContext(
    _In_ PLEEWAYUI_TAB hTab
);

/**
 * LeewayUITabstripNextTab
 * Selects the next tab (wraps to first if at end).
 * Respects keyboard shortcut Ctrl+Tab.
 *
 * Parameters:
 *   hTabstrip   Handle to tabstrip
 *
 * Returns:
 *   Handle to newly selected tab, or NULL on error
 */
PLEEWAYUI_TAB
WINAPI
LeewayUITabstripNextTab(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip
);

/**
 * LeewayUITabstripPrevTab
 * Selects the previous tab (wraps to last if at start).
 * Respects keyboard shortcut Ctrl+Shift+Tab.
 *
 * Parameters:
 *   hTabstrip   Handle to tabstrip
 *
 * Returns:
 *   Handle to newly selected tab, or NULL on error
 */
PLEEWAYUI_TAB
WINAPI
LeewayUITabstripPrevTab(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip
);

/**
 * LeewayUITabstripCloseSelectedTab
 * Closes the currently selected tab.
 * Sends LWUI_TABSTRIP_NOTIFY_CLOSING; if handler returns FALSE, close is cancelled.
 *
 * Parameters:
 *   hTabstrip   Handle to tabstrip
 *
 * Returns:
 *   S_OK if closed, S_FALSE if cancelled by handler, error code on failure
 */
HRESULT
WINAPI
LeewayUITabstripCloseSelectedTab(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip
);

/**
 * LeewayUITabstripGetTabCount
 * Returns the number of tabs in the tabstrip.
 *
 * Parameters:
 *   hTabstrip   Handle to tabstrip
 *
 * Returns:
 *   Number of tabs, or 0 on error
 */
UINT
WINAPI
LeewayUITabstripGetTabCount(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip
);

/**
 * LeewayUITabstripGetTabByIndex
 * Retrieves a tab handle by its zero-based index.
 *
 * Parameters:
 *   hTabstrip   Handle to tabstrip
 *   index       Zero-based tab index
 *
 * Returns:
 *   Handle to tab, or NULL if index is out of bounds
 */
PLEEWAYUI_TAB
WINAPI
LeewayUITabstripGetTabByIndex(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip,
    _In_ UINT index
);

/**
 * LeewayUITabstripMoveTab
 * Reorders tabs by moving one to a new position.
 *
 * Parameters:
 *   hTabstrip   Handle to tabstrip
 *   hTab        Tab to move
 *   new_index   Destination position (zero-based)
 *
 * Returns:
 *   S_OK on success, or error code on failure
 */
HRESULT
WINAPI
LeewayUITabstripMoveTab(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip,
    _In_ PLEEWAYUI_TAB hTab,
    _In_ UINT new_index
);

/**
 * LeewayUITabstripClearAllTabs
 * Removes all tabs from the tabstrip.
 *
 * Parameters:
 *   hTabstrip   Handle to tabstrip
 *
 * Returns:
 *   S_OK on success, or error code on failure
 */
HRESULT
WINAPI
LeewayUITabstripClearAllTabs(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip
);

/**
 * LeewayUITabstripSetDPI
 * Notifies the tabstrip of a DPI change (per-monitor DPI v2 support).
 *
 * Parameters:
 *   hTabstrip   Handle to tabstrip
 *   dpi         New DPI value
 *
 * Returns:
 *   S_OK on success, or error code on failure
 */
HRESULT
WINAPI
LeewayUITabstripSetDPI(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip,
    _In_ UINT dpi
);

/**
 * LeewayUITabstripPreTranslateMessage
 * Processes keyboard shortcuts (Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+W, Ctrl+N/T).
 * Call this before TranslateMessage in your message loop if tabs should handle shortcuts.
 *
 * Parameters:
 *   hTabstrip   Handle to tabstrip
 *   pMsg        Pointer to MSG structure
 *
 * Returns:
 *   TRUE if message was handled by tabstrip, FALSE if caller should process it
 */
BOOL
WINAPI
LeewayUITabstripPreTranslateMessage(
    _In_ PLEEWAYUI_TABSTRIP hTabstrip,
    _In_ CONST MSG *pMsg
);

#ifdef __cplusplus
}
#endif

#endif /* _LEEWAYUI_TABSTRIP_H_ */
