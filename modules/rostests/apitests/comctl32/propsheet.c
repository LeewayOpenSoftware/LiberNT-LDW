/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Test for v6 property sheet
 * COPYRIGHT:   Copyright 2019 Katayama Hirofumi MZ (katayama.hirofumi.mz@gmail.com)
 */

#include "wine/test.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <prsht.h>

#define IDC_APPLY_BUTTON 12321
#define IDC_BACK_BUTTON  12323
#define IDC_NEXT_BUTTON  12324
#define IDC_FINISH_BUTTON 12325
#define IDC_AERO_BACKBUTTON 12330

static BOOL s_bNotified;

static BOOL OnInitDialog(HWND hwnd, HWND hwndFocus, LPARAM lParam)
{
    s_bNotified = FALSE;
    SetDlgItemTextW(hwnd, edt1, L"text");
    SetTimer(hwnd, 999, 300, NULL);
    return TRUE;
}

static void OnCommand(HWND hwnd, int id, HWND hwndCtl, UINT codeNotify)
{
    switch (id)
    {
        case IDOK:
        case IDCANCEL:
            EndDialog(hwnd, id);
            break;
        case edt1:
            if (codeNotify == EN_CHANGE)
            {
                s_bNotified = TRUE;
                PropSheet_Changed(GetParent(hwnd), hwnd);
            }
            break;
    }
}

static void OnTimer(HWND hwnd, UINT id)
{
    HWND hwndParent, hwndApply;

    KillTimer(hwnd, id);

    ok_int(s_bNotified, TRUE);

    hwndParent = GetParent(hwnd);
    hwndApply = GetDlgItem(hwndParent, IDC_APPLY_BUTTON);
    ok_int(IsWindowEnabled(hwndApply), FALSE);

    PropSheet_Changed(hwndParent, hwnd);
    ok_int(IsWindowEnabled(hwndApply), TRUE);

    PropSheet_UnChanged(hwndParent, hwnd);
    ok_int(IsWindowEnabled(hwndApply), FALSE);

    PropSheet_PressButton(hwndParent, PSBTN_OK);
}

static INT_PTR CALLBACK
Page1DlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        HANDLE_MSG(hwnd, WM_INITDIALOG, OnInitDialog);
        HANDLE_MSG(hwnd, WM_COMMAND, OnCommand);
        HANDLE_MSG(hwnd, WM_TIMER, OnTimer);
    }
    return 0;
}

#ifdef _MSC_VER
#define CHECK_STRUCT_SIZE(x, y) C_ASSERT((x) == (y))
#else
// Can't do this compile time, thanks gcc
// 'error: non-nested function with variably modified type'
#define CHECK_STRUCT_SIZE(x, y) ok((x) == (y), "Wrong size for %s, got %Iu, expected %Iu\n", #x, (size_t)(x), (size_t)(y))
#endif

// Validate struct sizes
static void test_StructSizes()
{
#ifdef _WIN64
    CHECK_STRUCT_SIZE(PROPSHEETPAGEA_V1_SIZE, 72);
    CHECK_STRUCT_SIZE(PROPSHEETPAGEA_V2_SIZE, 88);
    CHECK_STRUCT_SIZE(PROPSHEETPAGEA_V3_SIZE, 96);
    CHECK_STRUCT_SIZE(PROPSHEETPAGEA_V4_SIZE, 104);
    CHECK_STRUCT_SIZE(PROPSHEETHEADERA_V1_SIZE, 72);
    CHECK_STRUCT_SIZE(PROPSHEETHEADERA_V2_SIZE, 96);

    CHECK_STRUCT_SIZE(PROPSHEETPAGEW_V1_SIZE, 72);
    CHECK_STRUCT_SIZE(PROPSHEETPAGEW_V2_SIZE, 88);
    CHECK_STRUCT_SIZE(PROPSHEETPAGEW_V3_SIZE, 96);
    CHECK_STRUCT_SIZE(PROPSHEETPAGEW_V4_SIZE, 104);
    CHECK_STRUCT_SIZE(PROPSHEETHEADERW_V1_SIZE, 72);
    CHECK_STRUCT_SIZE(PROPSHEETHEADERW_V2_SIZE, 96);
#else
    CHECK_STRUCT_SIZE(PROPSHEETPAGEA_V1_SIZE, 40);
    CHECK_STRUCT_SIZE(PROPSHEETPAGEA_V2_SIZE, 48);
    CHECK_STRUCT_SIZE(PROPSHEETPAGEA_V3_SIZE, 52);
    CHECK_STRUCT_SIZE(PROPSHEETPAGEA_V4_SIZE, 56);
    CHECK_STRUCT_SIZE(PROPSHEETHEADERA_V1_SIZE, 40);
    CHECK_STRUCT_SIZE(PROPSHEETHEADERA_V2_SIZE, 52);

    CHECK_STRUCT_SIZE(PROPSHEETPAGEW_V1_SIZE, 40);
    CHECK_STRUCT_SIZE(PROPSHEETPAGEW_V2_SIZE, 48);
    CHECK_STRUCT_SIZE(PROPSHEETPAGEW_V3_SIZE, 52);
    CHECK_STRUCT_SIZE(PROPSHEETPAGEW_V4_SIZE, 56);
    CHECK_STRUCT_SIZE(PROPSHEETHEADERW_V1_SIZE, 40);
    CHECK_STRUCT_SIZE(PROPSHEETHEADERW_V2_SIZE, 52);
#endif
}

typedef HPROPSHEETPAGE (WINAPI *FN_CreatePropertySheetPageW)(LPCPROPSHEETPAGEW);
typedef int (WINAPI *FN_PropertySheetW)(LPCPROPSHEETHEADERW);
static FN_CreatePropertySheetPageW pCreatePropertySheetPageW;
static FN_PropertySheetW pPropertySheetW;

// Show that the Apply button is not enabled by default
static void test_ApplyButtonDisabled()
{
    PROPSHEETPAGEW psp = {0};
    PROPSHEETHEADERW header = {0};
    HPROPSHEETPAGE hpsp[1];

    psp.dwSize = sizeof(psp);
    psp.dwFlags = PSP_DEFAULT;
    psp.hInstance = GetModuleHandleW(NULL);
    psp.pszTemplate = MAKEINTRESOURCEW(1);
    psp.pfnDlgProc = Page1DlgProc;
    hpsp[0] = pCreatePropertySheetPageW(&psp);
    ok(hpsp[0] != NULL, "hpsp[0] was NULL.\n");

    header.dwSize = sizeof(header);
    header.dwFlags = 0;
    header.hInstance = GetModuleHandleW(NULL);
    header.hwndParent = NULL;
    header.nPages = ARRAYSIZE(hpsp);
    header.phpage = hpsp;
    header.pszCaption = L"propsheet";
    ok(pPropertySheetW(&header) > 0, "PropertySheet returned non-positive value.\n");
}

static INT s_iAeroState;
static LONG s_lAeroSetActive;

static BOOL AeroOnInitDialog(HWND hwnd, HWND hwndFocus, LPARAM lParam)
{
    SetTimer(hwnd, 999, 300, NULL);
    return TRUE;
}

static BOOL AeroOnNotify(HWND hwnd, UINT idCtrl, LPNMHDR pnmh)
{
    if (pnmh->code == PSN_SETACTIVE)
        InterlockedIncrement(&s_lAeroSetActive);
    return FALSE;
}

static void AeroFinish(HWND hwnd, HWND hwndParent)
{
    KillTimer(hwnd, 999);
    ok(InterlockedCompareExchange(&s_lAeroSetActive, 0, 0) >= 1,
       "PSN_SETACTIVE was not received.\n");
    PropSheet_PressButton(hwndParent, PSBTN_CANCEL);
}

static void AeroOnTimer(HWND hwnd, UINT id)
{
    HWND hwndParent = GetParent(hwnd);
    HWND hwndBack = GetDlgItem(hwndParent, IDC_BACK_BUTTON);
    HWND hwndNext = GetDlgItem(hwndParent, IDC_NEXT_BUTTON);
    HWND hwndFinish = GetDlgItem(hwndParent, IDC_FINISH_BUTTON);
    HWND hwndCancel = GetDlgItem(hwndParent, IDCANCEL);
    WCHAR szText[64];

    switch (s_iAeroState)
    {
        case 0:
            ok(hwndBack != NULL, "IDC_BACK_BUTTON missing.\n");
            ok(hwndNext != NULL, "IDC_NEXT_BUTTON missing.\n");
            ok(hwndFinish != NULL, "IDC_FINISH_BUTTON missing.\n");
            ok(hwndCancel != NULL, "IDCANCEL missing.\n");

            SendMessageW(hwndParent, PSM_SETNEXTTEXTW, 0, (LPARAM)L"Install");
            GetWindowTextW(hwndNext, szText, ARRAYSIZE(szText));
            ok(wcscmp(szText, L"Install") == 0, "PSM_SETNEXTTEXTW failed, got %ls.\n", szText);

            SendMessageW(hwndParent, PSM_SETBUTTONTEXTW, PSBTN_CANCEL, (LPARAM)L"Dismiss");
            GetWindowTextW(hwndCancel, szText, ARRAYSIZE(szText));
            ok(wcscmp(szText, L"Dismiss") == 0, "PSM_SETBUTTONTEXTW on CANCEL failed, got %ls.\n", szText);

            SendMessageW(hwndParent, PSM_SETBUTTONTEXTW, PSBTN_BACK, (LPARAM)L"Return");
            GetWindowTextW(hwndBack, szText, ARRAYSIZE(szText));
            ok(wcscmp(szText, L"Return") == 0, "PSM_SETBUTTONTEXTW on BACK failed, got %ls.\n", szText);

            PropSheet_ShowWizButtons(hwndParent, 0, PSWIZB_BACK | PSWIZB_NEXT);
            break;
        case 1:
            ok(!IsWindowVisible(hwndBack), "Back should be hidden.\n");
            ok(!IsWindowVisible(hwndNext), "Next should be hidden.\n");
            ok(IsWindowVisible(hwndCancel), "Cancel should still be visible.\n");

            PropSheet_ShowWizButtons(hwndParent, PSWIZB_BACK, PSWIZB_BACK);
            PropSheet_ShowWizButtons(hwndParent, PSWIZB_NEXT, PSWIZB_NEXT);
            PropSheet_ShowWizButtons(hwndParent, 0, PSWIZB_CANCEL);
            break;
        case 2:
            ok(IsWindowVisible(hwndBack), "Back should be shown again.\n");
            ok(IsWindowVisible(hwndNext), "Next should be shown again.\n");
            ok(!IsWindowVisible(hwndCancel), "Cancel should be hidden.\n");

            PropSheet_ShowWizButtons(hwndParent, PSWIZB_CANCEL, PSWIZB_CANCEL);
            PropSheet_EnableWizButtons(hwndParent, 0, PSWIZB_NEXT);
            break;
        case 3:
            ok(IsWindowVisible(hwndCancel), "Cancel should be shown again.\n");
            ok(!IsWindowEnabled(hwndNext), "Next should be disabled.\n");

            PropSheet_EnableWizButtons(hwndParent, PSWIZB_NEXT, PSWIZB_NEXT);
            break;
        case 4:
            ok(IsWindowEnabled(hwndNext), "Next should be enabled again.\n");

            SendMessageW(hwndParent, PSM_SETWIZBUTTONS, 0, PSWIZB_BACK | PSWIZB_DISABLEDFINISH);
            ok(!IsWindowVisible(hwndNext), "Next should be hidden with DISABLEDFINISH.\n");
            ok(IsWindowVisible(hwndFinish), "Finish should be visible with DISABLEDFINISH.\n");
            ok(!IsWindowEnabled(hwndFinish), "Finish should be disabled with DISABLEDFINISH.\n");

            SendMessageW(hwndParent, PSM_SETWIZBUTTONS, 0, PSWIZB_BACK | PSWIZB_NEXT);
            ok(IsWindowVisible(hwndNext) && IsWindowEnabled(hwndNext),
               "Next should be restored after DISABLEDFINISH.\n");

            AeroFinish(hwnd, hwndParent);
            break;
    }
    s_iAeroState++;
    if (s_iAeroState < 5)
        SetTimer(hwnd, 999, 300, NULL);
}

static INT_PTR CALLBACK
AeroPage1DlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        HANDLE_MSG(hwnd, WM_INITDIALOG, AeroOnInitDialog);
        HANDLE_MSG(hwnd, WM_NOTIFY, AeroOnNotify);
        HANDLE_MSG(hwnd, WM_TIMER, AeroOnTimer);
    }
    return 0;
}

// PSH_AEROWIZARD contract surface (M0): the four aero wizard messages drive
// the existing wizard chrome identically in the v5 and v6 load paths.
static void test_AeroWizard(void)
{
    PROPSHEETPAGEW psp = {0};
    PROPSHEETHEADERW header = {0};
    HPROPSHEETPAGE hpsp[1];
    INT_PTR ret;

    psp.dwSize = sizeof(psp);
    psp.dwFlags = PSP_DEFAULT;
    psp.hInstance = GetModuleHandleW(NULL);
    psp.pszTemplate = MAKEINTRESOURCEW(1);
    psp.pfnDlgProc = AeroPage1DlgProc;
    hpsp[0] = pCreatePropertySheetPageW(&psp);
    ok(hpsp[0] != NULL, "hpsp[0] was NULL.\n");

    header.dwSize = sizeof(header);
    header.dwFlags = PSH_AEROWIZARD;
    header.hInstance = GetModuleHandleW(NULL);
    header.hwndParent = NULL;
    header.nPages = ARRAYSIZE(hpsp);
    header.phpage = hpsp;
    header.pszCaption = L"aerowizard";

    s_iAeroState = 0;
    s_lAeroSetActive = 0;
    ret = pPropertySheetW(&header);
    ok(ret != -1, "PropertySheet returned -1.\n");
}

// PSH_AEROWIZARD M2: the title-bar back button shows from page two on, and
// click / Alt+Left drive the PSN_WIZBACK path. Classic chrome lacks the
// button; the checks are skipped there.
static INT s_iAeroBackState;
static LONG s_lAeroWizBackCount;
static HWND s_hwndAeroPage0;
static BOOL s_bAeroBackArmed;

static BOOL AeroBackOnInitDialog(HWND hwnd, HWND hwndFocus, LPARAM lParam)
{
    if (!s_bAeroBackArmed)
    {
        s_bAeroBackArmed = TRUE;
        SetTimer(hwnd, 999, 300, NULL);
    }
    return TRUE;
}

static BOOL AeroBackOnNotify(HWND hwnd, UINT idCtrl, LPNMHDR pnmh)
{
    switch (pnmh->code)
    {
        case PSN_SETACTIVE:
            if (!s_hwndAeroPage0)
                s_hwndAeroPage0 = hwnd;
            break;
        case PSN_WIZBACK:
            InterlockedIncrement(&s_lAeroWizBackCount);
            break;
    }
    return FALSE;
}

static void AeroBackFinish(HWND hwnd, HWND hwndParent)
{
    KillTimer(hwnd, 999);
    PropSheet_PressButton(hwndParent, PSBTN_CANCEL);
}

static void AeroBackOnTimer(HWND hwnd, UINT id)
{
    HWND hwndParent = GetParent(hwnd);
    HWND hwndCaptionBack = GetDlgItem(hwndParent, IDC_AERO_BACKBUTTON);
    HWND hwndBack = GetDlgItem(hwndParent, IDC_BACK_BUTTON);
    HWND hwndCurrent = (HWND)SendMessageW(hwndParent, PSM_GETCURRENTPAGEHWND, 0, 0);

    KillTimer(hwnd, id);

    if (!hwndCaptionBack)
    {
        trace("IDC_AERO_BACKBUTTON not present (classic chrome), skipping M2 checks.\n");
        AeroBackFinish(hwnd, hwndParent);
        return;
    }

    switch (s_iAeroBackState)
    {
        case 0:
            ok(hwndCurrent == s_hwndAeroPage0, "Wizard should open on page 1.\n");
            ok(!IsWindowVisible(hwndCaptionBack), "Title-bar Back should be hidden on page 1.\n");
            ok(!IsWindowVisible(hwndBack), "Command-area Back should be hidden in aero chrome.\n");
            ok((GetWindowLongW(hwndCaptionBack, GWL_STYLE) & WS_TABSTOP) != 0,
               "Title-bar Back should participate in tab order.\n");
            ok(GetParent(hwndCaptionBack) == hwndParent, "Title-bar Back should be a child of the sheet.\n");

            PropSheet_PressButton(hwndParent, PSBTN_NEXT);
            break;
        case 1:
            ok(hwndCurrent != s_hwndAeroPage0, "Wizard should be on page 2.\n");
            ok(IsWindowVisible(hwndCaptionBack), "Title-bar Back should be visible on page 2.\n");
            ok(IsWindowEnabled(hwndCaptionBack), "Title-bar Back should be enabled on page 2.\n");

            SendMessageW(hwndCaptionBack, BM_CLICK, 0, 0);
            ok(InterlockedCompareExchange(&s_lAeroWizBackCount, 0, 0) == 1,
               "PSN_WIZBACK should have been sent once.\n");
            ok(SendMessageW(hwndParent, PSM_GETCURRENTPAGEHWND, 0, 0) == (LPARAM)s_hwndAeroPage0,
               "Clicking title-bar Back should return to page 1.\n");
            break;
        case 2:
            PostMessageW(hwndParent, WM_SYSKEYDOWN, VK_LEFT, (1ul << 29));
            break;
        case 3:
            ok(InterlockedCompareExchange(&s_lAeroWizBackCount, 0, 0) == 1,
               "Alt+Left should be ignored while Back is hidden.\n");
            ok(SendMessageW(hwndParent, PSM_GETCURRENTPAGEHWND, 0, 0) == (LPARAM)s_hwndAeroPage0,
               "Should still be on page 1.\n");

            PropSheet_PressButton(hwndParent, PSBTN_NEXT);
            break;
        case 4:
            PostMessageW(hwndParent, WM_SYSKEYDOWN, VK_LEFT, (1ul << 29));
            break;
        case 5:
            ok(InterlockedCompareExchange(&s_lAeroWizBackCount, 0, 0) == 2,
               "Alt+Left should send PSN_WIZBACK.\n");
            ok(SendMessageW(hwndParent, PSM_GETCURRENTPAGEHWND, 0, 0) == (LPARAM)s_hwndAeroPage0,
               "Alt+Left should return to page 1.\n");

            PropSheet_PressButton(hwndParent, PSBTN_NEXT);
            break;
        case 6:
            SendMessageW(hwndParent, PSM_SETWIZBUTTONS, 0, PSWIZB_NEXT);
            ok(!IsWindowEnabled(hwndCaptionBack), "Title-bar Back should follow PSWIZB_BACK off.\n");

            SendMessageW(hwndParent, PSM_SETWIZBUTTONS, 0, PSWIZB_BACK | PSWIZB_NEXT);
            ok(IsWindowEnabled(hwndCaptionBack), "Title-bar Back should follow PSWIZB_BACK on.\n");

            AeroBackFinish(hwnd, hwndParent);
            break;
    }
    s_iAeroBackState++;
    if (s_iAeroBackState < 7)
        SetTimer(hwnd, 999, 300, NULL);
}

static INT_PTR CALLBACK
AeroBackPageProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        HANDLE_MSG(hwnd, WM_INITDIALOG, AeroBackOnInitDialog);
        HANDLE_MSG(hwnd, WM_NOTIFY, AeroBackOnNotify);
        HANDLE_MSG(hwnd, WM_TIMER, AeroBackOnTimer);
    }
    return 0;
}

static void test_AeroWizardBack(void)
{
    PROPSHEETPAGEW psp = {0};
    PROPSHEETHEADERW header = {0};
    HPROPSHEETPAGE hpsp[3];
    INT_PTR ret;
    INT i;

    psp.dwSize = sizeof(psp);
    psp.dwFlags = PSP_DEFAULT;
    psp.hInstance = GetModuleHandleW(NULL);
    psp.pszTemplate = MAKEINTRESOURCEW(1);
    psp.pfnDlgProc = AeroBackPageProc;
    for (i = 0; i < 3; i++)
    {
        hpsp[i] = pCreatePropertySheetPageW(&psp);
        ok(hpsp[i] != NULL, "hpsp[%d] was NULL.\n", i);
    }

    header.dwSize = sizeof(header);
    header.dwFlags = PSH_AEROWIZARD;
    header.hInstance = GetModuleHandleW(NULL);
    header.hwndParent = NULL;
    header.nPages = ARRAYSIZE(hpsp);
    header.phpage = hpsp;
    header.pszCaption = L"aerowizard-back";

    s_iAeroBackState = 0;
    s_lAeroWizBackCount = 0;
    s_hwndAeroPage0 = NULL;
    s_bAeroBackArmed = FALSE;
    ret = pPropertySheetW(&header);
    ok(ret != -1, "PropertySheet returned -1.\n");
}


START_TEST(propsheetv6)
{
    HMODULE hComCtl32;

    hComCtl32 = LoadLibraryW(L"comctl32.dll");
    pCreatePropertySheetPageW = (FN_CreatePropertySheetPageW)GetProcAddress(hComCtl32, "CreatePropertySheetPageW");
    pPropertySheetW = (FN_PropertySheetW)GetProcAddress(hComCtl32, "PropertySheetW");

    {
        WCHAR szPath[MAX_PATH];
        if (GetModuleFileNameW(hComCtl32, szPath, ARRAYSIZE(szPath)))
            trace("comctl32 loaded from %s\n", wine_dbgstr_w(szPath));
    }

    ok(pCreatePropertySheetPageW != NULL, "pCreatePropertySheetPageW was NULL.\n");
    ok(pPropertySheetW != NULL, "pPropertySheetW was NULL.\n");

    if (!pCreatePropertySheetPageW || !pPropertySheetW)
    {
        skip("!pCreatePropertySheetPageW || !pPropertySheetW\n");
        return;
    }

    test_StructSizes();
    test_ApplyButtonDisabled();
    test_AeroWizard();
    test_AeroWizardBack();

    FreeLibrary(hComCtl32);
}
