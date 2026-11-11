/*
 * PROJECT:     browseui
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     ITaskbarList implementation
 * COPYRIGHT:   Copyright 2018 Mark Jansen (mark.jansen@reactos.org)
 */

#include "precomp.h"


/***********************************************************************
 *   ITaskbarList2 implementation
 */

#define TWM_GETTASKSWITCH (WM_USER + 236)
#define TSWM_MARKFULLSCREEN (WM_USER + 7)
/* Button decorations, handled by explorer's task switch window
   (base/shell/explorer/taskswnd.cpp; keep in sync). */
#define TSWM_SETPROGRESSSTATE (WM_USER + 0x40)
#define TSWM_SETPROGRESSVALUE (WM_USER + 0x41)
#define TSWM_SETOVERLAYICON (WM_USER + 0x42)
#define TSWM_SETACTIVEALT (WM_USER + 0x43)
#define TASK_PROGRESS_MAX 0xFFFF

/* A hung Explorer must not hang its clients. */
#define TASKBARLIST_SEND_TIMEOUT 2000

CTaskbarList::CTaskbarList()
    : m_hTaskWnd(NULL)
{
    m_ShellHookMsg = RegisterWindowMessageW(L"SHELLHOOK");
}

CTaskbarList::~CTaskbarList()
{
}

HWND CTaskbarList::TaskWnd()
{
    HWND hTrayWnd;
    if (m_hTaskWnd && ::IsWindow(m_hTaskWnd))
        return m_hTaskWnd;

    m_hTaskWnd = NULL;
    hTrayWnd = FindWindowW(L"Shell_TrayWnd", NULL);
    if (hTrayWnd)
    {
        DWORD_PTR Result = 0;
        if (SendMessageTimeoutW(hTrayWnd, TWM_GETTASKSWITCH, 0, 0, SMTO_ABORTIFHUNG,
                                TASKBARLIST_SEND_TIMEOUT, &Result))
        {
            m_hTaskWnd = (HWND)Result;
        }
    }
    return m_hTaskWnd;
}

BOOL CTaskbarList::SendTaskWnd(UINT Msg, WPARAM wParam, LPARAM lParam, LRESULT *pResult)
{
    DWORD_PTR Result = 0;
    HWND hTaskWnd = TaskWnd();

    if (pResult)
        *pResult = 0;
    if (!hTaskWnd ||
        !SendMessageTimeoutW(hTaskWnd, Msg, wParam, lParam, SMTO_ABORTIFHUNG, TASKBARLIST_SEND_TIMEOUT, &Result))
    {
        return FALSE;
    }
    if (pResult)
        *pResult = (LRESULT)Result;
    return TRUE;
}

/* TRUE in the result means a task button took the change. */
HRESULT CTaskbarList::SendDecoration(UINT Msg, HWND hWnd, LPARAM lParam)
{
    LRESULT Result;

    if (!hWnd)
        return E_INVALIDARG;
    if (!SendTaskWnd(Msg, (WPARAM)hWnd, lParam, &Result))
        return E_FAIL;
    return Result ? S_OK : E_FAIL;
}

void CTaskbarList::SendTaskWndShellHook(WPARAM wParam, HWND hWnd)
{
    if (m_ShellHookMsg)
        SendTaskWnd(m_ShellHookMsg, wParam, (LPARAM)hWnd, NULL);
}


HRESULT WINAPI CTaskbarList::MarkFullscreenWindow(HWND hwnd, BOOL fFullscreen)
{
    HWND hTaskWnd = TaskWnd();
    if (hTaskWnd)
        ::PostMessageW(hTaskWnd, TSWM_MARKFULLSCREEN, (WPARAM)hwnd, fFullscreen);
    return S_OK;
}


/***********************************************************************
 *   ITaskbarList implementation
 */

HRESULT WINAPI CTaskbarList::HrInit()
{
    if (m_ShellHookMsg == NULL)
        return E_OUTOFMEMORY;

    if (!TaskWnd())
        return E_HANDLE;

    return S_OK;
}

HRESULT WINAPI CTaskbarList::AddTab(HWND hwnd)
{
    SendTaskWndShellHook(HSHELL_WINDOWCREATED, hwnd);
    return S_OK;
}

HRESULT WINAPI CTaskbarList::DeleteTab(HWND hwnd)
{
    SendTaskWndShellHook(HSHELL_WINDOWDESTROYED, hwnd);
    return S_OK;
}

HRESULT WINAPI CTaskbarList::ActivateTab(HWND hwnd)
{
    SendTaskWndShellHook(HSHELL_WINDOWACTIVATED, hwnd);
    return S_OK;
}

/* Marks the button shown as active while a window of the same process that
 * has no button of its own is active. It does not activate anything. */
HRESULT WINAPI CTaskbarList::SetActiveAlt(HWND hwnd)
{
    if (!hwnd)
        return E_INVALIDARG;
    SendTaskWnd(TSWM_SETACTIVEALT, (WPARAM)hwnd, 0, NULL);
    return S_OK;
}


/***********************************************************************
 *   ITaskbarList3 implementation
 *
 * Progress and overlay icons are drawn on the task buttons by Explorer.
 * An owned window (such as the copy progress dialog) decorates the button
 * of its owner. Thumbnail toolbars, tabs (RegisterTab and the others),
 * thumbnail tooltips and clips are not implemented and return E_NOTIMPL.
 */

HRESULT WINAPI CTaskbarList::SetProgressValue(HWND hwnd, ULONGLONG completed, ULONGLONG total)
{
    ULONG Value = 0;

    /* Scale to 0..TASK_PROGRESS_MAX without overflow. */
    if (completed > total)
        completed = total;
    while (total > 0xFFFFFFFFULL)
    {
        total >>= 1;
        completed >>= 1;
    }
    if (total)
        Value = (ULONG)((completed * TASK_PROGRESS_MAX) / total);
    return SendDecoration(TSWM_SETPROGRESSVALUE, hwnd, (LPARAM)Value);
}

HRESULT WINAPI CTaskbarList::SetProgressState(HWND hwnd, TBPFLAG flags)
{
    if (flags != TBPF_NOPROGRESS && flags != TBPF_INDETERMINATE && flags != TBPF_NORMAL &&
        flags != TBPF_ERROR && flags != TBPF_PAUSED)
    {
        return E_INVALIDARG;
    }
    return SendDecoration(TSWM_SETPROGRESSSTATE, hwnd, (LPARAM)flags);
}

HRESULT WINAPI CTaskbarList::RegisterTab(HWND tab, HWND mdi)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CTaskbarList::UnregisterTab(HWND tab)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CTaskbarList::SetTabOrder(HWND tab, HWND before)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CTaskbarList::SetTabActive(HWND tab, HWND mdi, DWORD reserved)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CTaskbarList::ThumbBarAddButtons(HWND hwnd, UINT count, LPTHUMBBUTTON buttons)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CTaskbarList::ThumbBarUpdateButtons(HWND hwnd, UINT count, LPTHUMBBUTTON buttons)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CTaskbarList::ThumbBarSetImageList(HWND hwnd, HIMAGELIST images)
{
    return E_NOTIMPL;
}

/* Explorer keeps its own copy of the icon. The description is accepted but
 * not shown: the task buttons have no accessibility text for it yet. */
HRESULT WINAPI CTaskbarList::SetOverlayIcon(HWND hwnd, HICON icon, LPCWSTR description)
{
    return SendDecoration(TSWM_SETOVERLAYICON, hwnd, (LPARAM)icon);
}

HRESULT WINAPI CTaskbarList::SetThumbnailTooltip(HWND hwnd, LPCWSTR tip)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CTaskbarList::SetThumbnailClip(HWND hwnd, RECT *clip)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CTaskbarList::SetTabProperties(HWND tab, STPFLAG flags)
{
    return E_NOTIMPL;
}
