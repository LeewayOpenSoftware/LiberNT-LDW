/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Control Panel category view (home page, category pages, icon views)
 *
 * The values below are LiberNT's own: the Windows names of the view state
 * values are not publicly documented.
 */

#ifndef _SHFLDR_CPLCATEGORYVIEW_H_
#define _SHFLDR_CPLCATEGORYVIEW_H_

/* HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\ControlPanel */
#define CPL_VIEW_STATE_KEY  L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ControlPanel"
#define CPL_VALUE_STARTUP   L"StartupPage"      /* 0 = categories, 1 = all items as icons, 2 = classic list */
#define CPL_VALUE_SMALLICON L"AllItemsIconView" /* 0 = large icons, 1 = small icons */

enum CPL_STARTUP_PAGE
{
    CPL_STARTUP_CATEGORY = 0,
    CPL_STARTUP_ICONS = 1,
    CPL_STARTUP_CLASSIC = 2
};

/* The view the Control Panel opens on: the saved choice, categories by default */
UINT CplGetStartupPage(VOID);
BOOL CplGetSmallIcons(VOID);
VOID CplSetViewChoice(UINT startupPage, BOOL smallIcons);

/* Wraps the classic list view (pInner) with the category pages and the "View by" selector */
HRESULT CplCreateCategoryView(IShellFolder *psf, IShellView *pInner, IShellView **ppView);

#endif /* _SHFLDR_CPLCATEGORYVIEW_H_ */
