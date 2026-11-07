/*
 * PROJECT:     LiberNT Tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Aero wizard M1 screenshot harness: a tiny 3-page wizard in aero, wizard97 and plain modes
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <commctrl.h>
#include <prsht.h>

#include "resource.h"

#define SAFETY_TIMEOUT_MS 300000

static INT_PTR CALLBACK
ShotPageProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
            SetTimer(hwndDlg, 1, SAFETY_TIMEOUT_MS, NULL);
            return TRUE;

        case WM_TIMER:
            KillTimer(hwndDlg, 1);
            PropSheet_PressButton(GetParent(hwndDlg), PSBTN_CANCEL);
            return TRUE;
    }
    return FALSE;
}

static HPROPSHEETPAGE CreateShotPage(HINSTANCE hInst, LPCWSTR pszTemplate,
                                     LPCWSTR pszTitle, LPCWSTR pszSubTitle)
{
    PROPSHEETPAGEW psp;

    ZeroMemory(&psp, sizeof(psp));
    psp.dwSize = sizeof(psp);
    psp.dwFlags = PSP_USEHEADERTITLE | PSP_USEHEADERSUBTITLE;
    psp.hInstance = hInst;
    psp.pszTemplate = pszTemplate;
    psp.pszHeaderTitle = pszTitle;
    psp.pszHeaderSubTitle = pszSubTitle;
    psp.pfnDlgProc = ShotPageProc;
    return CreatePropertySheetPageW(&psp);
}

int wmain(int argc, WCHAR **argv)
{
    HINSTANCE hInst = GetModuleHandleW(NULL);
    HPROPSHEETPAGE hpsp[3];
    PROPSHEETHEADERW psh;
    DWORD dwFlags;
    const WCHAR *pszMode = (argc > 1) ? argv[1] : L"aero";
    INITCOMMONCONTROLSEX icc;

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    if (wcscmp(pszMode, L"wizard97") == 0)
        dwFlags = PSH_WIZARD97;
    else if (wcscmp(pszMode, L"plain") == 0)
        dwFlags = PSH_WIZARD;
    else
        dwFlags = PSH_AEROWIZARD;

    hpsp[0] = CreateShotPage(hInst, MAKEINTRESOURCEW(IDD_SHOT_PAGE1),
                             L"Update drivers: Example Device", L"How do you want to search for drivers?");
    hpsp[1] = CreateShotPage(hInst, MAKEINTRESOURCEW(IDD_SHOT_PAGE2),
                             L"Searching for drivers", L"This should take a few seconds.");
    hpsp[2] = CreateShotPage(hInst, MAKEINTRESOURCEW(IDD_SHOT_PAGE3),
                             L"Completing the wizard", L"The driver was installed.");
    if (!hpsp[0] || !hpsp[1] || !hpsp[2])
        return 1;

    ZeroMemory(&psh, sizeof(psh));
    psh.dwSize = sizeof(psh);
    psh.dwFlags = dwFlags;
    psh.hInstance = hInst;
    psh.hwndParent = NULL;
    psh.pszCaption = L"AeroShot";
    psh.nPages = 3;
    psh.phpage = hpsp;

    return (int)PropertySheetW(&psh);
}
