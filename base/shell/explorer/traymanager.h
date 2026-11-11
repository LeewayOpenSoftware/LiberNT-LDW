/*
 * PROJECT:     LiberNT shell
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Multi-monitor taskbar manager (P1.9a)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

class TrayManager
{
public:
    TrayManager();
    ~TrayManager();

    HRESULT Initialize();
    HRESULT Open();
    HRESULT Close();

    ITrayWindow* GetPrimaryTray() const;

    VOID TrayProcessMessages();
    VOID TrayMessageLoop();

    VOID OnDisplayChange();
    VOID OnSettingsChanged();

private:
    HRESULT UpdateSecondaryTrays();
    HRESULT CreateSecondaryTray(HMONITOR hMonitor);
    VOID DestroySecondaryTrays();

    CComPtr<ITrayWindow> m_PrimaryTray;
    CAtlArray<CComPtr<ITrayWindow>> m_SecondaryTrays;
};
