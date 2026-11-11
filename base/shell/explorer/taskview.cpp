/*
 * PROJECT:     ReactOS Explorer
 * LICENSE:     GPL-3.0-or-later
 * PURPOSE:     Win+Tab task switcher window
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include "taskview.h"
#include "startmnu2.h"

#include <windowsx.h>

typedef struct _TASKVIEW_ITEM
{
    HWND hWnd;
    HICON hIcon;
    BOOL bDestroyIcon;
    WCHAR szTitle[256];
} TASKVIEW_ITEM;

#define TIMER_ID_TASKVIEW_PRUNE 3

typedef CWinTraits<WS_POPUP, WS_EX_TOOLWINDOW | WS_EX_TOPMOST> CTrayTaskViewTraits;

class CTrayTaskViewWnd;

static CTrayTaskViewWnd *g_pTaskView = NULL;

class CTrayTaskViewWnd :
    public CWindowImpl<CTrayTaskViewWnd, CWindow, CTrayTaskViewTraits>
{
public:
    DECLARE_WND_CLASS_EX(L"TrayTaskView", CS_DROPSHADOW, COLOR_WINDOW)

    ITrayWindow *m_Tray;
    HWND m_hTrayWnd;
    HWND m_hwndFore;
    CSimpleArray<TASKVIEW_ITEM> m_Items;
    SM2_FLYOUT_PALETTE m_Pal;
    HFONT m_hFont;
    INT m_iSel;
    INT m_iHot;
    INT m_iTop;
    INT m_cVisible;
    INT m_cxWidth;
    INT m_cyRow;
    INT m_Pad;
    BOOL m_bActivating;

    CTrayTaskViewWnd(ITrayWindow *pTray) :
        m_Tray(pTray),
        m_hTrayWnd(pTray->GetHWND()),
        m_hwndFore(NULL),
        m_hFont(NULL),
        m_iSel(0),
        m_iHot(-1),
        m_iTop(0),
        m_cVisible(0),
        m_cxWidth(0),
        m_cyRow(0),
        m_Pad(0),
        m_bActivating(FALSE)
    {
        ZeroMemory(&m_Pal, sizeof(m_Pal));
    }

    virtual void OnFinalMessage(HWND) override
    {
        if (g_pTaskView == this)
            g_pTaskView = NULL;
        delete this;
    }

    int Sc(int v) const { return ShellScaleForDpi(v); }

    static HICON GetItemIcon(HWND hWnd, BOOL *pbOwned)
    {
        HICON hIcon = NULL;
        DWORD_PTR dwResult;
        UINT uIcons[] = { ICON_SMALL2, ICON_SMALL, ICON_BIG };

        *pbOwned = FALSE;

        for (UINT i = 0; i < _countof(uIcons) && !hIcon; i++)
        {
            if (SendMessageTimeoutW(hWnd, WM_GETICON, uIcons[i], 0,
                                    SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, 100, &dwResult) &&
                dwResult)
            {
                hIcon = (HICON)dwResult;
            }
        }

        if (!hIcon)
            hIcon = (HICON)GetClassLongPtrW(hWnd, GCL_HICONSM);
        if (!hIcon)
            hIcon = (HICON)GetClassLongPtrW(hWnd, GCL_HICON);

        if (!hIcon)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(hWnd, &pid);
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProcess)
            {
                WCHAR szPath[MAX_PATH];
                DWORD cchPath = _countof(szPath);
                if (QueryFullProcessImageNameW(hProcess, 0, szPath, &cchPath))
                {
                    SHFILEINFOW sfi = { 0 };
                    if (SHGetFileInfoW(szPath, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON))
                    {
                        hIcon = sfi.hIcon;
                        *pbOwned = TRUE;
                    }
                }
                CloseHandle(hProcess);
            }
        }

        return hIcon;
    }

    static BOOL CALLBACK s_EnumProc(HWND hWnd, LPARAM lParam)
    {
        CTrayTaskViewWnd *pThis = (CTrayTaskViewWnd *)lParam;

        if (!pThis->m_Tray->IsTaskWnd(hWnd))
            return TRUE;

        TASKVIEW_ITEM item;
        item.hWnd = hWnd;
        item.hIcon = GetItemIcon(hWnd, &item.bDestroyIcon);
        if (InternalGetWindowText(hWnd, item.szTitle, _countof(item.szTitle)) <= 0)
            item.szTitle[0] = UNICODE_NULL;

        pThis->m_Items.Add(item);
        return TRUE;
    }

    VOID DestroyItemIcons()
    {
        for (INT i = 0; i < m_Items.GetSize(); i++)
        {
            if (m_Items[i].bDestroyIcon && m_Items[i].hIcon)
                DestroyIcon(m_Items[i].hIcon);
        }
    }

    VOID Refresh()
    {
        DestroyItemIcons();
        m_Items.RemoveAll();
        EnumWindows(s_EnumProc, (LPARAM)this);
    }

    INT ItemCount() const { return m_Items.GetSize(); }

    RECT RowRect(INT i) const
    {
        RECT rc;
        SetRect(&rc, m_Pad, m_Pad + (i - m_iTop) * m_cyRow,
                m_cxWidth + m_Pad, m_Pad + (i - m_iTop + 1) * m_cyRow);
        return rc;
    }

    INT ItemAtPoint(POINT pt) const
    {
        if (pt.x < m_Pad || pt.x >= m_cxWidth + m_Pad)
            return -1;

        INT i = m_iTop + (pt.y - m_Pad) / m_cyRow;
        return (i >= 0 && i < ItemCount()) ? i : -1;
    }

    VOID EnsureVisible(INT i)
    {
        if (i < m_iTop)
            m_iTop = i;
        else if (i >= m_iTop + m_cVisible)
            m_iTop = i - m_cVisible + 1;

        INT iMaxTop = max(0, ItemCount() - m_cVisible);
        if (m_iTop > iMaxTop)
            m_iTop = iMaxTop;
        if (m_iTop < 0)
            m_iTop = 0;
    }

    VOID SetSel(INT i)
    {
        if (ItemCount() <= 0)
            return;
        i = max(0, min(i, ItemCount() - 1));
        if (i != m_iSel)
        {
            m_iSel = i;
            EnsureVisible(i);
            InvalidateRect(NULL, FALSE);
        }
    }

    VOID MoveSel(INT delta)
    {
        INT count = ItemCount();
        if (count <= 0)
            return;
        SetSel(((m_iSel + delta) % count + count) % count);
    }

    VOID Layout(INT *pcx, INT *pcy)
    {
        m_Pad = Sc(8);
        m_cyRow = Sc(36);
        m_cxWidth = Sc(300);

        HMONITOR hMon = MonitorFromWindow(m_hTrayWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hMon, &mi);

        INT rowsAvail = max(1, (MulDiv(mi.rcWork.bottom - mi.rcWork.top, 2, 3) - 2 * m_Pad) / m_cyRow);
        m_cVisible = min(ItemCount(), min(rowsAvail, 16));

        *pcx = m_cxWidth + 2 * m_Pad;
        *pcy = m_cVisible * m_cyRow + 2 * m_Pad;
    }

    VOID Open()
    {
        StartMenu2_GetFlyoutPalette(&m_Pal);
        if (!m_hFont)
        {
            m_hFont = CreateFontW(-Sc(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        }

        Refresh();
        if (!ItemCount())
        {
            TRACE("TaskView: no task windows, not opening\n");
            DestroyWindow();
            return;
        }

        m_hwndFore = GetForegroundWindow();

        INT cx, cy;
        Layout(&cx, &cy);

        HWND hwndFore = m_hwndFore;
        m_iSel = 0;
        for (INT i = 0; i < ItemCount(); i++)
        {
            if (m_Items[i].hWnd == hwndFore)
            {
                m_iSel = i;
                break;
            }
        }
        m_iTop = 0;
        EnsureVisible(m_iSel);
        TRACE("TaskView: opening with %d items, selection %d\n", ItemCount(), m_iSel);

        HMONITOR hMon = MonitorFromWindow(m_hTrayWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hMon, &mi);

        INT x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - cx) / 2;
        INT y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - cy) / 2;

        SetWindowPos(HWND_TOPMOST, x, y, cx, cy, SWP_SHOWWINDOW);
        SetForegroundWindow(m_hWnd);
        ::SetFocus(m_hWnd);
        SetTimer(TIMER_ID_TASKVIEW_PRUNE, 500, NULL);
    }

    VOID Cycle(BOOL bForward)
    {
        HWND hwndCur = (m_iSel >= 0 && m_iSel < ItemCount()) ? m_Items[m_iSel].hWnd : NULL;

        Refresh();
        if (!ItemCount())
        {
            DestroyWindow();
            return;
        }

        m_iSel = 0;
        if (hwndCur)
        {
            for (INT i = 0; i < ItemCount(); i++)
            {
                if (m_Items[i].hWnd == hwndCur)
                {
                    m_iSel = i;
                    break;
                }
            }
        }
        MoveSel(bForward ? 1 : -1);
        EnsureVisible(m_iSel);
        InvalidateRect(NULL, FALSE);
        TRACE("TaskView: cycle -> selection %d\n", m_iSel);
    }

    VOID ActivateSel()
    {
        HWND hWnd = NULL;

        if (m_iSel >= 0 && m_iSel < ItemCount() && ::IsWindow(m_Items[m_iSel].hWnd))
            hWnd = m_Items[m_iSel].hWnd;

        if (hWnd)
        {
            TRACE("TaskView: activating %p\n", hWnd);
            m_bActivating = TRUE;
            ::SwitchToThisWindow(hWnd, TRUE);
        }
        DestroyWindow();
    }

    VOID Cancel()
    {
        HWND hwndFore = m_hwndFore;

        if (hwndFore && hwndFore != m_hWnd && ::IsWindow(hwndFore))
        {
            m_bActivating = TRUE;
            ::SetForegroundWindow(hwndFore);
        }
        DestroyWindow();
    }

    VOID PruneDeadItems()
    {
        BOOL bRemoved = FALSE;

        for (INT i = ItemCount() - 1; i >= 0; i--)
        {
            if (!::IsWindow(m_Items[i].hWnd))
            {
                if (m_Items[i].bDestroyIcon && m_Items[i].hIcon)
                    DestroyIcon(m_Items[i].hIcon);
                m_Items.RemoveAt(i);
                bRemoved = TRUE;
            }
        }

        if (!ItemCount())
        {
            DestroyWindow();
            return;
        }

        if (bRemoved)
        {
            if (m_iSel >= ItemCount())
                m_iSel = ItemCount() - 1;
            if (m_iHot >= ItemCount())
                m_iHot = -1;
            EnsureVisible(m_iSel);
            InvalidateRect(NULL, FALSE);
        }
    }

    VOID DrawItems(HDC hdc, const RECT *prc)
    {
        HBRUSH hbrBg = CreateSolidBrush(m_Pal.PanelBg);
        FillRect(hdc, prc, hbrBg);
        DeleteObject(hbrBg);

        HBRUSH hbrBorder = CreateSolidBrush(m_Pal.Border);
        FrameRect(hdc, prc, hbrBorder);
        DeleteObject(hbrBorder);

        HGDIOBJ hOldFont = SelectObject(hdc, m_hFont);
        SetBkMode(hdc, TRANSPARENT);

        INT iEnd = min(m_iTop + m_cVisible, ItemCount());
        INT iIcon = Sc(16);
        for (INT i = m_iTop; i < iEnd; i++)
        {
            RECT rcRow = RowRect(i);

            if (i == m_iSel)
            {
                HBRUSH hbr = CreateSolidBrush(m_Pal.AccentBg);
                FillRect(hdc, &rcRow, hbr);
                DeleteObject(hbr);
                SetTextColor(hdc, m_Pal.AccentText);
            }
            else if (i == m_iHot)
            {
                HBRUSH hbr = CreateSolidBrush(m_Pal.HotFill);
                FillRect(hdc, &rcRow, hbr);
                DeleteObject(hbr);
                SetTextColor(hdc, m_Pal.PanelText);
            }
            else
            {
                SetTextColor(hdc, m_Pal.PanelText);
            }

            if (m_Items[i].hIcon)
            {
                INT yIcon = rcRow.top + (rcRow.bottom - rcRow.top - iIcon) / 2;
                DrawIconEx(hdc, rcRow.left + Sc(4), yIcon, m_Items[i].hIcon,
                           iIcon, iIcon, 0, NULL, DI_NORMAL);
            }

            RECT rcText = rcRow;
            rcText.left += Sc(4) + iIcon + Sc(6);
            rcText.right -= Sc(6);
            DrawTextW(hdc, m_Items[i].szTitle, -1, &rcText,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }

        SelectObject(hdc, hOldFont);
    }

    LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL&)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(&ps);

        RECT rc;
        GetClientRect(&rc);

        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ hOldBmp = SelectObject(hdcMem, hbmMem);

        DrawItems(hdcMem, &rc);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY);

        SelectObject(hdcMem, hOldBmp);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);

        EndPaint(&ps);
        return 0;
    }

    LRESULT OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&)
    {
        return 1;
    }

    LRESULT OnKeyDown(UINT, WPARAM wParam, LPARAM, BOOL&)
    {
        switch (wParam)
        {
            case VK_ESCAPE:
                Cancel();
                break;
            case VK_RETURN:
            case VK_SPACE:
                ActivateSel();
                break;
            case VK_DOWN:
                MoveSel(1);
                break;
            case VK_UP:
                MoveSel(-1);
                break;
            case VK_TAB:
                MoveSel((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
                break;
            case VK_HOME:
                SetSel(0);
                break;
            case VK_END:
                SetSel(ItemCount() - 1);
                break;
            case VK_NEXT:
                SetSel(m_iSel + m_cVisible);
                break;
            case VK_PRIOR:
                SetSel(m_iSel - m_cVisible);
                break;
        }
        return 0;
    }

    LRESULT OnMouseMove(UINT, WPARAM, LPARAM lParam, BOOL&)
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        INT iHot = ItemAtPoint(pt);
        if (iHot != m_iHot)
        {
            m_iHot = iHot;
            InvalidateRect(NULL, FALSE);
        }
        return 0;
    }

    LRESULT OnLButtonDown(UINT, WPARAM, LPARAM lParam, BOOL&)
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        INT iHit = ItemAtPoint(pt);
        if (iHit >= 0)
        {
            m_iSel = iHit;
            ActivateSel();
        }
        return 0;
    }

    LRESULT OnMouseWheel(UINT, WPARAM wParam, LPARAM, BOOL&)
    {
        SetSel(m_iSel + (GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1));
        return 0;
    }

    LRESULT OnActivate(UINT, WPARAM wParam, LPARAM, BOOL&)
    {
        if (LOWORD(wParam) == WA_INACTIVE && !m_bActivating)
            Cancel();
        return 0;
    }

    LRESULT OnTimer(UINT, WPARAM wParam, LPARAM, BOOL&)
    {
        if (wParam == TIMER_ID_TASKVIEW_PRUNE)
            PruneDeadItems();
        return 0;
    }

    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&)
    {
        KillTimer(TIMER_ID_TASKVIEW_PRUNE);
        DestroyItemIcons();
        if (m_hFont)
        {
            DeleteObject(m_hFont);
            m_hFont = NULL;
        }
        return 0;
    }

    BEGIN_MSG_MAP(CTrayTaskViewWnd)
        MESSAGE_HANDLER(WM_PAINT, OnPaint)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
        MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
        MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
        MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
        MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
        MESSAGE_HANDLER(WM_ACTIVATE, OnActivate)
        MESSAGE_HANDLER(WM_TIMER, OnTimer)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    END_MSG_MAP()
};

VOID TaskView_ToggleCycle(IN ITrayWindow *Tray, IN BOOL bForward)
{
    if (g_pTaskView && g_pTaskView->IsWindow() && g_pTaskView->IsWindowVisible())
    {
        g_pTaskView->Cycle(bForward);
        return;
    }

    TaskView_Destroy();

    CTrayTaskViewWnd *pView = new CTrayTaskViewWnd(Tray);
    if (!pView)
        return;

    if (!pView->Create(Tray->GetHWND(), CWindow::rcDefault, NULL))
    {
        delete pView;
        return;
    }

    g_pTaskView = pView;
    pView->Open();
}

VOID TaskView_Destroy(VOID)
{
    if (g_pTaskView && g_pTaskView->IsWindow())
        g_pTaskView->DestroyWindow();
    g_pTaskView = NULL;
}
