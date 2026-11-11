/*
 * PROJECT:     LiberNT shell
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Multi-monitor taskbar manager (P1.9a)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include "traymanager.h"

TrayManager* g_TrayManager = NULL;

TrayManager::TrayManager()
{
    g_TrayManager = this;
}

TrayManager::~TrayManager()
{
    Close();
    g_TrayManager = NULL;
}

HRESULT TrayManager::Initialize()
{
    HRESULT hr = CreateTrayWindow(&m_PrimaryTray);
    if (FAILED(hr))
        return hr;

    return S_OK;
}

HRESULT TrayManager::Open()
{
    if (m_PrimaryTray == NULL)
        return E_UNEXPECTED;

    HRESULT hr = m_PrimaryTray->Open();
    if (FAILED(hr))
        return hr;

    return UpdateSecondaryTrays();
}

HRESULT TrayManager::Close()
{
    DestroySecondaryTrays();

    if (m_PrimaryTray != NULL)
    {
        m_PrimaryTray->Close();
        m_PrimaryTray.Release();
    }

    return S_OK;
}

ITrayWindow* TrayManager::GetPrimaryTray() const
{
    return m_PrimaryTray.p;
}

VOID TrayManager::TrayProcessMessages()
{
    if (m_PrimaryTray != NULL)
        ::TrayProcessMessages(m_PrimaryTray);
}

VOID TrayManager::TrayMessageLoop()
{
    if (m_PrimaryTray != NULL)
        ::TrayMessageLoop(m_PrimaryTray);
}

VOID TrayManager::OnDisplayChange()
{
    UpdateSecondaryTrays();
}

/* Live-apply entry point: called when the multi-monitor taskbar settings change. */
VOID TrayManager::OnSettingsChanged()
{
    UpdateSecondaryTrays();
}

HRESULT TrayManager::UpdateSecondaryTrays()
{
    DestroySecondaryTrays();

    if (!g_TaskbarSettings.bMMTaskbarEnabled)
        return S_OK;

    HWND hwndPrimary = m_PrimaryTray ? m_PrimaryTray->GetHWND() : NULL;
    HMONITOR hPrimary = hwndPrimary ? MonitorFromWindow(hwndPrimary, MONITOR_DEFAULTTOPRIMARY) : NULL;

    struct EnumData
    {
        TrayManager* pThis;
        HMONITOR hPrimary;
        HRESULT hr;
    } data = { this, hPrimary, S_OK };

    auto enumProc = [](HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam) -> BOOL
    {
        EnumData* pData = reinterpret_cast<EnumData*>(lParam);
        if (hMonitor == pData->hPrimary)
            return TRUE;

        HRESULT hr = pData->pThis->CreateSecondaryTray(hMonitor);
        if (FAILED(hr))
            pData->hr = hr;

        return TRUE;
    };

    EnumDisplayMonitors(NULL, NULL, enumProc, reinterpret_cast<LPARAM>(&data));

    return data.hr;
}

HRESULT TrayManager::CreateSecondaryTray(HMONITOR hMonitor)
{
    CComPtr<ITrayWindow> tray;
    HRESULT hr = CreateSecondaryTrayWindow(hMonitor, &tray);
    if (FAILED(hr))
        return hr;

    hr = tray->Open();
    if (FAILED(hr))
        return hr;

    m_SecondaryTrays.Add(tray);
    return S_OK;
}

VOID TrayManager::DestroySecondaryTrays()
{
    for (INT i = 0; i < m_SecondaryTrays.GetCount(); ++i)
    {
        if (m_SecondaryTrays[i] != NULL)
            m_SecondaryTrays[i]->Close();
    }
    m_SecondaryTrays.SetCount(0);
}

VOID TrayManager_OnDisplayChange()
{
    if (g_TrayManager != NULL)
        g_TrayManager->OnDisplayChange();
}

VOID TrayManager_OnSettingsChanged()
{
    if (g_TrayManager != NULL)
        g_TrayManager->OnSettingsChanged();
}
