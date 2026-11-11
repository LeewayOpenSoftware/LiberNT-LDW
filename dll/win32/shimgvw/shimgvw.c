/*
 * PROJECT:     ReactOS Picture and Fax Viewer
 * LICENSE:     GPL-2.0 (https://spdx.org/licenses/GPL-2.0)
 * PURPOSE:     Image file browsing and manipulation
 * COPYRIGHT:   Copyright Dmitry Chapyshev (dmitry@reactos.org)
 *              Copyright 2018-2023 Katayama Hirofumi MZ (katayama.hirofumi.mz@gmail.com)
 *              Copyright 2025 Whindmar Saksit <whindsaks@proton.me>
 */

#include "shimgvw.h"
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include "vribbon.h"
#include <leewayui/frame.h>

EXTERN_C PCWSTR GetExtraExtensionsGdipList(VOID);
EXTERN_C HRESULT LoadImageFromPath(LPCWSTR Path, GpImage** ppImage);

/* Toolbar image size */
#define TB_IMAGE_WIDTH  16
#define TB_IMAGE_HEIGHT 16

/* Slide show timer */
#define SLIDESHOW_TIMER_ID          0xFACE
#define SLIDESHOW_TIMER_INTERVAL    5000 /* 5 seconds */
#define HIDECURSOR_TIMER_ID         0xBABE
#define HIDECURSOR_TIMER_TIMEOUT    3000

HINSTANCE           g_hInstance         = NULL;
HWND                g_hMainWnd          = NULL;
HWND                g_hwndFullscreen    = NULL;
SHIMGVW_FILENODE *  g_pCurrentFile      = NULL;
WCHAR               g_szFile[MAX_PATH];
GpImage *           g_pImage            = NULL;
SHIMGVW_SETTINGS    g_Settings;
UINT                g_ImageId;

static const UINT s_ZoomSteps[] =
{
    5, 10, 25, 50, 100, 200, 300, 500, 1000, 2000, 4000
};

#define MIN_ZOOM s_ZoomSteps[0]
#define MAX_ZOOM s_ZoomSteps[_countof(s_ZoomSteps) - 1]

    /* iBitmap,       idCommand,   fsState,         fsStyle,     bReserved[2], dwData, iString */
#define DEFINE_BTN_INFO(_name) \
    { TBICON_##_name, IDC_##_name, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0 }

#define DEFINE_BTN_SEPARATOR \
    { -1,             0,           TBSTATE_ENABLED, BTNS_SEP,    {0}, 0, 0 }

/* ToolBar Buttons */
static const TBBUTTON s_Buttons[] =
{
    DEFINE_BTN_INFO(PREV_PIC),
    DEFINE_BTN_INFO(NEXT_PIC),
    DEFINE_BTN_SEPARATOR,
    DEFINE_BTN_INFO(BEST_FIT),
    DEFINE_BTN_INFO(REAL_SIZE),
    DEFINE_BTN_INFO(SLIDE_SHOW),
    DEFINE_BTN_SEPARATOR,
    DEFINE_BTN_INFO(ZOOM_IN),
    DEFINE_BTN_INFO(ZOOM_OUT),
    DEFINE_BTN_SEPARATOR,
    DEFINE_BTN_INFO(ROT_CLOCKW),
    DEFINE_BTN_INFO(ROT_COUNCW),
    DEFINE_BTN_SEPARATOR,
    DEFINE_BTN_INFO(ROT_CWSAVE),
    DEFINE_BTN_INFO(ROT_CCWSAVE),
    DEFINE_BTN_SEPARATOR,
    DEFINE_BTN_INFO(DELETE),
    DEFINE_BTN_INFO(PRINT),
    DEFINE_BTN_INFO(SAVEAS),
    DEFINE_BTN_INFO(MODIFY),
    DEFINE_BTN_SEPARATOR,
    DEFINE_BTN_INFO(HELP_TOC)
};

/* ToolBar Button configuration */
typedef struct
{
    DWORD idb;  /* Index to bitmap */
    DWORD ids;  /* Index to tooltip */
} TB_BUTTON_CONFIG;

#define DEFINE_BTN_CONFIG(_name) { IDB_##_name, IDS_TOOLTIP_##_name }

static const TB_BUTTON_CONFIG s_ButtonConfig[] =
{
    DEFINE_BTN_CONFIG(PREV_PIC),
    DEFINE_BTN_CONFIG(NEXT_PIC),
    DEFINE_BTN_CONFIG(BEST_FIT),
    DEFINE_BTN_CONFIG(REAL_SIZE),
    DEFINE_BTN_CONFIG(SLIDE_SHOW),
    DEFINE_BTN_CONFIG(ZOOM_IN),
    DEFINE_BTN_CONFIG(ZOOM_OUT),
    DEFINE_BTN_CONFIG(ROT_CLOCKW),
    DEFINE_BTN_CONFIG(ROT_COUNCW),
    DEFINE_BTN_CONFIG(ROT_CWSAVE),
    DEFINE_BTN_CONFIG(ROT_CCWSAVE),
    DEFINE_BTN_CONFIG(DELETE),
    DEFINE_BTN_CONFIG(PRINT),
    DEFINE_BTN_CONFIG(SAVEAS),
    DEFINE_BTN_CONFIG(MODIFY),
    DEFINE_BTN_CONFIG(HELP_TOC),
};

typedef struct tagPREVIEW_DATA
{
    HWND m_hwnd;
    HWND m_hwndZoom;
    HWND m_hwndToolBar;
    INT m_nZoomPercents;
    ANIME m_Anime; /* Animation */
    INT m_xScrollOffset;
    INT m_yScrollOffset;
    UINT m_nMouseDownMsg;
    UINT m_nTimerInterval;
    BOOL m_bHideCursor;
    POINT m_ptOrigin;
    /* Ribbon chrome (main window only). The pill toolbar and the ribbon are
     * two surfaces over the same command table; m_hwndToolBar is NULL in
     * ribbon chrome, m_pRibbon is NULL in pill chrome. */
    VRB_RIBBON *m_pRibbon;
    LeewayUIFrame *m_pFrame;     /* status bar, zoom slider, layout (ribbon chrome) */
    BOOL m_bFit;                 /* best-fit mode is active */
    /* Enabled state of the toolbar commands, as the toolbar had it. Both
     * surfaces read these; the verb checks and the file check set them. */
    BOOL m_bEnBestFit, m_bEnRealSize, m_bEnZoomIn, m_bEnZoomOut;
    BOOL m_bEnDelete, m_bEnSaveAs, m_bEnPrint, m_bEnModify;
} PREVIEW_DATA, *PPREVIEW_DATA;

static VOID Preview_ToggleSlideShowEx(PPREVIEW_DATA pData, BOOL StartTimer);

static inline PPREVIEW_DATA
Preview_GetData(HWND hwnd)
{
    return (PPREVIEW_DATA)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static inline BOOL
Preview_IsMainWnd(HWND hwnd)
{
    return hwnd == g_hMainWnd;
}

static VOID
Preview_RestartTimer(HWND hwnd)
{
    if (!Preview_IsMainWnd(hwnd))
    {
        PPREVIEW_DATA pData = Preview_GetData(hwnd);
        KillTimer(hwnd, SLIDESHOW_TIMER_ID);
        if (pData->m_nTimerInterval)
            SetTimer(hwnd, SLIDESHOW_TIMER_ID, pData->m_nTimerInterval, NULL);
    }
}

static VOID
Preview_ChangeSlideShowTimer(PPREVIEW_DATA pData, BOOL bSlower)
{
    BOOL IsFullscreen = !Preview_IsMainWnd(pData->m_hwnd);
    enum { mintime = 1000, maxtime = SLIDESHOW_TIMER_INTERVAL * 3, step = 1000 };
    UINT interval = pData->m_nTimerInterval ? pData->m_nTimerInterval : SLIDESHOW_TIMER_INTERVAL;
    if (IsFullscreen)
    {
        interval = bSlower ? min(interval + step, maxtime) : max(interval - step, mintime);
        if (pData->m_nTimerInterval != interval)
        {
            pData->m_nTimerInterval = interval;
            Preview_RestartTimer(pData->m_hwnd);
        }
    }
}

static VOID
ZoomWnd_UpdateScroll(PPREVIEW_DATA pData, BOOL bResetPos)
{
    HWND hwnd = pData->m_hwndZoom;
    RECT rcClient;
    UINT ImageWidth, ImageHeight, ZoomedWidth, ZoomedHeight;
    SCROLLINFO si;
    BOOL bShowHorz, bShowVert;

    if (bResetPos)
        pData->m_xScrollOffset = pData->m_yScrollOffset = 0;

    if (!g_pImage)
    {
        ShowScrollBar(hwnd, SB_BOTH, FALSE);
        InvalidateRect(hwnd, NULL, TRUE);
        return;
    }

    GdipGetImageWidth(g_pImage, &ImageWidth);
    GdipGetImageHeight(g_pImage, &ImageHeight);

    ZoomedWidth  = (ImageWidth  * pData->m_nZoomPercents) / 100;
    ZoomedHeight = (ImageHeight * pData->m_nZoomPercents) / 100;

    GetClientRect(hwnd, &rcClient);

    bShowHorz = (rcClient.right < ZoomedWidth);
    bShowVert = (rcClient.bottom < ZoomedHeight);
    ShowScrollBar(hwnd, SB_HORZ, bShowHorz);
    ShowScrollBar(hwnd, SB_VERT, bShowVert);

    GetClientRect(hwnd, &rcClient);

    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;

    if (bShowHorz)
    {
        GetScrollInfo(hwnd, SB_HORZ, &si);
        si.nPage = rcClient.right;
        si.nMin = 0;
        si.nMax = ZoomedWidth;
        si.nPos = (ZoomedWidth - rcClient.right) / 2 + pData->m_xScrollOffset;
        si.nPos = max(min(si.nPos, si.nMax - (INT)si.nPage), si.nMin);
        SetScrollInfo(hwnd, SB_HORZ, &si, TRUE);
        pData->m_xScrollOffset = si.nPos - (ZoomedWidth - rcClient.right) / 2;
    }
    else
    {
        pData->m_xScrollOffset = 0;
    }

    if (bShowVert)
    {
        GetScrollInfo(hwnd, SB_VERT, &si);
        si.nPage = rcClient.bottom;
        si.nMin = 0;
        si.nMax = ZoomedHeight;
        si.nPos = (ZoomedHeight - rcClient.bottom) / 2 + pData->m_yScrollOffset;
        si.nPos = max(min(si.nPos, si.nMax - (INT)si.nPage), si.nMin);
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        pData->m_yScrollOffset = si.nPos - (ZoomedHeight - rcClient.bottom) / 2;
    }
    else
    {
        pData->m_yScrollOffset = 0;
    }

    InvalidateRect(hwnd, NULL, TRUE);
}

/* ---- command state shared by the pill toolbar and the ribbon ---- */

static VOID
Preview_RibbonInvalidate(PPREVIEW_DATA pData, UINT CmdId)
{
    if (pData && pData->m_pRibbon)
        VRB_RibbonInvalidate(pData->m_pRibbon, CmdId);
}

static VOID
Preview_RibbonInvalidateAll(PPREVIEW_DATA pData)
{
    if (pData && pData->m_pRibbon)
        VRB_RibbonInvalidateAll(pData->m_pRibbon);
}

static BOOL *
Preview_EnabledFlag(PPREVIEW_DATA pData, UINT CmdId)
{
    switch (CmdId)
    {
        case IDC_BEST_FIT:  return &pData->m_bEnBestFit;
        case IDC_REAL_SIZE: return &pData->m_bEnRealSize;
        case IDC_ZOOM_IN:   return &pData->m_bEnZoomIn;
        case IDC_ZOOM_OUT:  return &pData->m_bEnZoomOut;
        case IDC_DELETE:    return &pData->m_bEnDelete;
        case IDC_SAVEAS:    return &pData->m_bEnSaveAs;
        case IDC_PRINT:     return &pData->m_bEnPrint;
        case IDC_MODIFY:    return &pData->m_bEnModify;
    }
    return NULL;
}

static VOID
Preview_SetEnabled(PPREVIEW_DATA pData, UINT CmdId, BOOL bEnable)
{
    BOOL *pFlag = Preview_EnabledFlag(pData, CmdId);
    if (pFlag)
        *pFlag = bEnable;
    if (pData->m_hwndToolBar)
        SendMessageW(pData->m_hwndToolBar, TB_ENABLEBUTTON, CmdId, bEnable);
    Preview_RibbonInvalidate(pData, CmdId);
}

/* S_OK: on the toolbar and enabled. S_FALSE: on the toolbar and disabled.
 * E_FAIL: not a toolbar command. The state is that of the main window, also
 * when the command comes from the slide show window. */
static HRESULT
IsCommandEnabled(UINT nCmdId)
{
    PPREVIEW_DATA pMain = Preview_GetData(g_hMainWnd);
    BOOL *pFlag = pMain ? Preview_EnabledFlag(pMain, nCmdId) : NULL;
    if (pFlag)
        return *pFlag ? S_OK : S_FALSE;
    return (nCmdId >= IDC_PREV_PIC && nCmdId <= IDC_HELP_TOC) ? S_OK : E_FAIL;
}

/* Status bar text and slider follow the zoom; the ribbon re-reads its state. */
static VOID
Preview_SyncZoomUi(PPREVIEW_DATA pData)
{
    if (pData->m_pFrame && pData->m_nZoomPercents > 0)
        LeewayFrameSetZoom(pData->m_pFrame, (UINT32)pData->m_nZoomPercents);
    Preview_RibbonInvalidate(pData, IDC_BEST_FIT);
    Preview_RibbonInvalidate(pData, IDC_REAL_SIZE);
    Preview_RibbonInvalidate(pData, IDC_ZOOM_IN);
    Preview_RibbonInvalidate(pData, IDC_ZOOM_OUT);
    Preview_RibbonInvalidate(pData, VRB_CMD_ZOOMLEVEL);
}

/* Status bar segment 0: the size of the image in pixels. */
static VOID
Preview_UpdateStatusInfo(PPREVIEW_DATA pData)
{
    WCHAR szText[64];
    UINT w = 0, h = 0;

    if (!pData->m_pFrame)
        return;
    szText[0] = UNICODE_NULL;
    if (g_pImage && GdipGetImageWidth(g_pImage, &w) == Ok && GdipGetImageHeight(g_pImage, &h) == Ok)
        StringCchPrintfW(szText, _countof(szText), L"%u x %u", w, h);
    LeewayFrameSetStatusText(pData->m_pFrame, 0, szText);
}

static VOID
Preview_UpdateZoom(PPREVIEW_DATA pData, UINT NewZoom, BOOL bEnableBestFit, BOOL bEnableRealSize)
{
    pData->m_nZoomPercents = NewZoom;
    pData->m_bFit = !bEnableBestFit;

    /* Zoom buttons are grayed at the ends of the zoom range, Fit while it
     * is the active mode, Actual size while the zoom is 100 %. */
    Preview_SetEnabled(pData, IDC_BEST_FIT, bEnableBestFit);
    Preview_SetEnabled(pData, IDC_REAL_SIZE, NewZoom != 100);
    Preview_SetEnabled(pData, IDC_ZOOM_IN,  NewZoom < MAX_ZOOM);
    Preview_SetEnabled(pData, IDC_ZOOM_OUT, NewZoom > MIN_ZOOM);
    Preview_SyncZoomUi(pData);

    /* Redraw the display window */
    InvalidateRect(pData->m_hwndZoom, NULL, TRUE);

    /* Restart timer if necessary */
    Preview_RestartTimer(pData->m_hwnd);

    /* Update scroll info */
    ZoomWnd_UpdateScroll(pData, FALSE);
}

static VOID
Preview_ZoomInOrOut(PPREVIEW_DATA pData, BOOL bZoomIn)
{
    UINT i, NewZoom;

    if (g_pImage == NULL)
        return;

    if (bZoomIn)    /* zoom in */
    {
        /* find next step */
        for (i = 0; i < _countof(s_ZoomSteps); ++i)
        {
            if (pData->m_nZoomPercents < s_ZoomSteps[i])
                break;
        }
        NewZoom = ((i >= _countof(s_ZoomSteps)) ? MAX_ZOOM : s_ZoomSteps[i]);
    }
    else            /* zoom out */
    {
        /* find previous step */
        for (i = _countof(s_ZoomSteps); i > 0; )
        {
            --i;
            if (s_ZoomSteps[i] < pData->m_nZoomPercents)
                break;
        }
        NewZoom = ((i < 0) ? MIN_ZOOM : s_ZoomSteps[i]);
    }

    /* Update toolbar and refresh screen */
    Preview_UpdateZoom(pData, NewZoom, TRUE, TRUE);
}

static VOID
Preview_ResetZoom(PPREVIEW_DATA pData)
{
    RECT Rect;
    UINT ImageWidth, ImageHeight, NewZoom;

    if (g_pImage == NULL)
        return;

    /* get disp window size and image size */
    GetClientRect(pData->m_hwndZoom, &Rect);
    GdipGetImageWidth(g_pImage, &ImageWidth);
    GdipGetImageHeight(g_pImage, &ImageHeight);

    /* compare two aspect rates. same as
       (ImageHeight / ImageWidth < Rect.bottom / Rect.right) in real */
    if (ImageHeight * Rect.right < Rect.bottom * ImageWidth)
    {
        if (Rect.right < ImageWidth)
        {
            /* it's large, shrink it */
            NewZoom = (Rect.right * 100) / ImageWidth;
        }
        else
        {
            /* it's small. show as original size */
            NewZoom = 100;
        }
    }
    else
    {
        if (Rect.bottom < ImageHeight)
        {
            /* it's large, shrink it */
            NewZoom = (Rect.bottom * 100) / ImageHeight;
        }
        else
        {
            /* it's small. show as original size */
            NewZoom = 100;
        }
    }

    Preview_UpdateZoom(pData, NewZoom, FALSE, TRUE);
}

static VOID
Preview_UpdateTitle(PPREVIEW_DATA pData, LPCWSTR FileName)
{
    WCHAR szText[MAX_PATH + 100];
    LPWSTR pchFileTitle;

    LoadStringW(g_hInstance, IDS_APPTITLE, szText, _countof(szText));

    pchFileTitle = PathFindFileNameW(FileName);
    if (pchFileTitle && *pchFileTitle)
    {
        StringCchCatW(szText, _countof(szText), L" - ");
        StringCchCatW(szText, _countof(szText), pchFileTitle);
    }

    SetWindowTextW(pData->m_hwnd, szText);
}

static VOID
Preview_pFreeImage(PPREVIEW_DATA pData)
{
    Anime_FreeInfo(&pData->m_Anime);

    if (g_pImage)
    {
        GdipDisposeImage(g_pImage);
        g_pImage = NULL;
    }
}

static VOID
Preview_pLoadImage(PPREVIEW_DATA pData, LPCWSTR szOpenFileName)
{
    HRESULT hr;
    BOOL bCanDel;
    Preview_pFreeImage(pData);
    InvalidateRect(pData->m_hwnd, NULL, FALSE); /* Schedule redraw in case we change to "No preview" */

    GetFullPathNameW(szOpenFileName, _countof(g_szFile), g_szFile, NULL);
    hr = LoadImageFromPath(szOpenFileName, &g_pImage);
    bCanDel = hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && hr != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    Preview_SetEnabled(Preview_GetData(g_hMainWnd), IDC_DELETE, bCanDel);
    if (FAILED(hr))
    {
        DPRINT1("GdipLoadImageFromStream() failed, %d\n", hr);
        Preview_pFreeImage(pData);
        Preview_UpdateTitle(pData, NULL);
        return;
    }

    Anime_LoadInfo(&pData->m_Anime);

    /* Reset zoom and redraw display */
    Preview_ResetZoom(pData);
    SHAddToRecentDocs(SHARD_PATHW, g_szFile);
    Preview_UpdateTitle(pData, g_szFile);

    ++g_ImageId;
    EnableCommandIfVerbExists(g_ImageId, g_hMainWnd, IDC_PRINT, L"print", g_szFile);
    EnableCommandIfVerbExists(g_ImageId, g_hMainWnd, IDC_MODIFY, L"edit", g_szFile);
}

static VOID
Preview_pLoadImageFromNode(PPREVIEW_DATA pData, SHIMGVW_FILENODE *pNode)
{
    Preview_pLoadImage(pData, (pNode ? pNode->FileName : NULL));
}

static BOOL
Preview_pSaveImage(PPREVIEW_DATA pData, LPCWSTR pszFile)
{
    ImageCodecInfo *codecInfo;
    GUID rawFormat;
    UINT j, num, nFilterIndex, size;
    BOOL ret = FALSE;

    if (g_pImage == NULL)
        return FALSE;

    GdipGetImageEncodersSize(&num, &size);
    codecInfo = QuickAlloc(size, FALSE);
    if (!codecInfo)
    {
        DPRINT1("QuickAlloc() failed in pSaveImage()\n");
        return FALSE;
    }
    GdipGetImageEncoders(num, size, codecInfo);

    GdipGetImageRawFormat(g_pImage, &rawFormat);
    if (IsEqualGUID(&rawFormat, &ImageFormatMemoryBMP))
        rawFormat = ImageFormatBMP;

    nFilterIndex = 0;
    for (j = 0; j < num; ++j)
    {
        if (IsEqualGUID(&rawFormat, &codecInfo[j].FormatID))
        {
            nFilterIndex = j + 1;
            break;
        }
    }

    Anime_Pause(&pData->m_Anime);

    ret = (nFilterIndex > 0) &&
          (GdipSaveImageToFile(g_pImage, pszFile, &codecInfo[nFilterIndex - 1].Clsid, NULL) == Ok);
    if (!ret)
        DPRINT1("GdipSaveImageToFile() failed\n");

    Anime_Start(&pData->m_Anime, 0);

    QuickFree(codecInfo);
    return ret;
}

static VOID
Preview_pSaveImageAs(PPREVIEW_DATA pData)
{
    OPENFILENAMEW sfn;
    ImageCodecInfo *codecInfo;
    WCHAR szSaveFileName[MAX_PATH];
    WCHAR *szFilterMask;
    GUID rawFormat;
    UINT num, size, j;
    size_t sizeRemain;
    WCHAR *c;
    HWND hwnd = pData->m_hwnd;

    if (g_pImage == NULL)
        return;

    GdipGetImageEncodersSize(&num, &size);
    codecInfo = QuickAlloc(size, FALSE);
    if (!codecInfo)
    {
        DPRINT1("QuickAlloc() failed in pSaveImageAs()\n");
        return;
    }

    GdipGetImageEncoders(num, size, codecInfo);

    GdipGetImageRawFormat(g_pImage, &rawFormat);
    if (IsEqualGUID(&rawFormat, &ImageFormatMemoryBMP))
        rawFormat = ImageFormatBMP;

    sizeRemain = 0;
    for (j = 0; j < num; ++j)
    {
        // Every pair needs space for the Description, twice the Extensions, 1 char for the space, 2 for the braces and 2 for the NULL terminators.
        sizeRemain = sizeRemain + (((wcslen(codecInfo[j].FormatDescription) + (wcslen(codecInfo[j].FilenameExtension) * 2) + 5) * sizeof(WCHAR)));
    }

    /* Add two more chars for the last terminator */
    sizeRemain += (sizeof(WCHAR) * 2);

    szFilterMask = QuickAlloc(sizeRemain, FALSE);
    if (!szFilterMask)
    {
        DPRINT1("cannot allocate memory for filter mask in pSaveImageAs()");
        QuickFree(codecInfo);
        return;
    }

    ZeroMemory(szSaveFileName, sizeof(szSaveFileName));
    ZeroMemory(szFilterMask, sizeRemain);
    ZeroMemory(&sfn, sizeof(sfn));
    sfn.lStructSize = sizeof(sfn);
    sfn.hwndOwner   = hwnd;
    sfn.lpstrFile   = szSaveFileName;
    sfn.lpstrFilter = szFilterMask;
    sfn.nMaxFile    = _countof(szSaveFileName);
    sfn.Flags       = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
    sfn.lpstrDefExt = L"png";

    c = szFilterMask;

    for (j = 0; j < num; ++j)
    {
        StringCbPrintfExW(c, sizeRemain, &c, &sizeRemain, 0, L"%ls (%ls)", codecInfo[j].FormatDescription, codecInfo[j].FilenameExtension);

        /* Skip the NULL character */
        c++;
        sizeRemain -= sizeof(*c);

        StringCbPrintfExW(c, sizeRemain, &c, &sizeRemain, 0, L"%ls", codecInfo[j].FilenameExtension);

        /* Skip the NULL character */
        c++;
        sizeRemain -= sizeof(*c);

        if (IsEqualGUID(&rawFormat, &codecInfo[j].FormatID))
        {
            sfn.nFilterIndex = j + 1;
        }
    }

    if (GetSaveFileNameW(&sfn) && sfn.nFilterIndex > 0)
    {
        Anime_Pause(&pData->m_Anime);

        if (GdipSaveImageToFile(g_pImage, szSaveFileName, &codecInfo[sfn.nFilterIndex - 1].Clsid, NULL) != Ok)
        {
            DPRINT1("GdipSaveImageToFile() failed\n");
        }

        Anime_Start(&pData->m_Anime, 0);
    }

    QuickFree(szFilterMask);
    QuickFree(codecInfo);
}

static VOID
Preview_pPrintImage(PPREVIEW_DATA pData)
{
    ShellExecuteVerb(g_hMainWnd, L"print", g_szFile, FALSE);
}

static VOID
Preview_UpdateUI(PPREVIEW_DATA pData)
{
    BOOL bEnable = (g_pImage != NULL);
    Preview_SetEnabled(pData, IDC_SAVEAS, bEnable);
    // These will be validated and enabled later by EnableCommandIfVerbExists
    Preview_SetEnabled(pData, IDC_PRINT, FALSE);
    Preview_SetEnabled(pData, IDC_MODIFY, FALSE);
    Preview_UpdateStatusInfo(pData);
    Preview_RibbonInvalidateAll(pData);
}

static VOID
Preview_UpdateImage(PPREVIEW_DATA pData)
{
    if (!Preview_IsMainWnd(pData->m_hwnd))
        Preview_ResetZoom(pData);

    ZoomWnd_UpdateScroll(pData, TRUE);
}

static VOID
Preview_LoadImage(PPREVIEW_DATA pData, SHIMGVW_FILENODE *pNode)
{
    Preview_pLoadImageFromNode(pData, pNode);
    Preview_UpdateImage(pData);
    Preview_UpdateUI(pData);
}

static SHIMGVW_FILENODE*
pBuildFileList(LPCWSTR szFirstFile)
{
    HANDLE hFindHandle;
    WCHAR *extension, *buffer;
    WCHAR szSearchPath[MAX_PATH];
    WCHAR szSearchMask[MAX_PATH];
    WCHAR szFileTypes[MAX_PATH];
    WIN32_FIND_DATAW findData;
    SHIMGVW_FILENODE *currentNode = NULL;
    SHIMGVW_FILENODE *root = NULL;
    SHIMGVW_FILENODE *conductor = NULL;
    ImageCodecInfo *codecInfo;
    UINT num = 0, size = 0, ExtraSize = 0;
    UINT j;

    const PCWSTR ExtraExtensions = GetExtraExtensionsGdipList();
    const UINT ExtraCount = ExtraExtensions[0] ? 1 : 0;
    if (ExtraCount)
        ExtraSize += sizeof(*codecInfo) + (wcslen(ExtraExtensions) + 1) * sizeof(WCHAR);

    StringCbCopyW(szSearchPath, sizeof(szSearchPath), szFirstFile);
    PathRemoveFileSpecW(szSearchPath);

    GdipGetImageDecodersSize(&num, &size);
    codecInfo = QuickAlloc(size + ExtraSize, FALSE);
    if (!codecInfo)
    {
        DPRINT1("QuickAlloc() failed in pLoadFileList()\n");
        return NULL;
    }

    GdipGetImageDecoders(num, size, codecInfo);
    buffer = (PWSTR)((UINT_PTR)codecInfo + size + (sizeof(*codecInfo) * ExtraCount));
    if (ExtraCount)
        codecInfo[num].FilenameExtension = wcscpy(buffer, ExtraExtensions);
    num += ExtraCount;

    root = QuickAlloc(sizeof(SHIMGVW_FILENODE), FALSE);
    if (!root)
    {
        DPRINT1("QuickAlloc() failed in pLoadFileList()\n");
        QuickFree(codecInfo);
        return NULL;
    }

    conductor = root;

    for (j = 0; j < num; ++j)
    {
        // FIXME: Parse each FilenameExtension list to bypass szFileTypes limit
        StringCbCopyW(szFileTypes, sizeof(szFileTypes), codecInfo[j].FilenameExtension);

        extension = wcstok(szFileTypes, L";");
        while (extension != NULL)
        {
            PathCombineW(szSearchMask, szSearchPath, extension);

            hFindHandle = FindFirstFileW(szSearchMask, &findData);
            if (hFindHandle != INVALID_HANDLE_VALUE)
            {
                do
                {
                    PathCombineW(conductor->FileName, szSearchPath, findData.cFileName);

                    // compare the name of the requested file with the one currently found.
                    // if the name matches, the current node is returned by the function.
                    if (_wcsicmp(szFirstFile, conductor->FileName) == 0)
                    {
                        currentNode = conductor;
                    }

                    conductor->Next = QuickAlloc(sizeof(SHIMGVW_FILENODE), FALSE);

                    // if QuickAlloc fails, make circular what we have and return it
                    if (!conductor->Next)
                    {
                        DPRINT1("QuickAlloc() failed in pLoadFileList()\n");

                        conductor->Next = root;
                        root->Prev = conductor;

                        FindClose(hFindHandle);
                        QuickFree(codecInfo);
                        return conductor;
                    }

                    conductor->Next->Prev = conductor;
                    conductor = conductor->Next;
                }
                while (FindNextFileW(hFindHandle, &findData) != 0);

                FindClose(hFindHandle);
            }

            extension = wcstok(NULL, L";");
        }
    }

    // we now have a node too much in the list. In case the requested file was not found,
    // we use this node to store the name of it, otherwise we free it.
    if (currentNode == NULL)
    {
        StringCchCopyW(conductor->FileName, MAX_PATH, szFirstFile);
        currentNode = conductor;
    }
    else
    {
        conductor = conductor->Prev;
        QuickFree(conductor->Next);
    }

    // link the last node with the first one to make the list circular
    conductor->Next = root;
    root->Prev = conductor;
    conductor = currentNode;

    QuickFree(codecInfo);

    return conductor;
}

static VOID
pFreeFileList(SHIMGVW_FILENODE *root)
{
    SHIMGVW_FILENODE *conductor;

    if (!root)
        return;

    root->Prev->Next = NULL;
    root->Prev = NULL;

    while (root)
    {
        conductor = root;
        root = conductor->Next;
        QuickFree(conductor);
    }
}

static HBRUSH CreateCheckerBoardBrush(VOID)
{
    static const CHAR pattern[] =
        "\x28\x00\x00\x00\x10\x00\x00\x00\x10\x00\x00\x00\x01\x00\x04\x00\x00\x00"
        "\x00\x00\x80\x00\x00\x00\x23\x2E\x00\x00\x23\x2E\x00\x00\x10\x00\x00\x00"
        "\x00\x00\x00\x00\x99\x99\x99\x00\xCC\xCC\xCC\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x11\x11\x11\x11"
        "\x00\x00\x00\x00\x11\x11\x11\x11\x00\x00\x00\x00\x11\x11\x11\x11\x00\x00"
        "\x00\x00\x11\x11\x11\x11\x00\x00\x00\x00\x11\x11\x11\x11\x00\x00\x00\x00"
        "\x11\x11\x11\x11\x00\x00\x00\x00\x11\x11\x11\x11\x00\x00\x00\x00\x11\x11"
        "\x11\x11\x00\x00\x00\x00\x00\x00\x00\x00\x11\x11\x11\x11\x00\x00\x00\x00"
        "\x11\x11\x11\x11\x00\x00\x00\x00\x11\x11\x11\x11\x00\x00\x00\x00\x11\x11"
        "\x11\x11\x00\x00\x00\x00\x11\x11\x11\x11\x00\x00\x00\x00\x11\x11\x11\x11"
        "\x00\x00\x00\x00\x11\x11\x11\x11\x00\x00\x00\x00\x11\x11\x11\x11";

    return CreateDIBPatternBrushPt(pattern, DIB_RGB_COLORS);
}

static VOID
ZoomWnd_OnDraw(
    PPREVIEW_DATA pData,
    HDC hdc,
    LPRECT prcPaint,
    LPRECT prcClient)
{
    GpGraphics *graphics;
    INT ZoomedWidth, ZoomedHeight;
    RECT rect, rcClient = *prcClient;
    HDC hdcMem;
    HBRUSH hBrush;
    HPEN hPen;
    HGDIOBJ hbrOld, hbmOld, hPenOld;
    UINT uFlags;
    HBITMAP hbmMem;
    SIZE paintSize = { prcPaint->right - prcPaint->left, prcPaint->bottom - prcPaint->top };
    COLORREF color0, color1;
    GpImageAttributes *imageAttributes;

    /* We use a memory bitmap to reduce flickering */
    hdcMem = CreateCompatibleDC(hdc);
    hbmMem = CreateCompatibleBitmap(hdc, paintSize.cx, paintSize.cy);
    hbmOld = SelectObject(hdcMem, hbmMem);

    /* Choose colors */
    if (Preview_IsMainWnd(pData->m_hwnd))
    {
        color0 = GetSysColor(COLOR_WINDOW);
        color1 = GetSysColor(COLOR_WINDOWTEXT);
    }
    else
    {
        color0 = RGB(0, 0, 0);
        color1 = RGB(255, 255, 255);
    }

    hBrush = CreateSolidBrush(color0);
    SetBkColor(hdcMem, color0);

    hPen = CreatePen(PS_SOLID, 1, color1);
    SetTextColor(hdcMem, color1);

    /* Fill background */
    SetRect(&rect, 0, 0, paintSize.cx, paintSize.cy);
    FillRect(hdcMem, &rect, hBrush);

    DeleteObject(hBrush);

    if (g_pImage == NULL)
    {
        WCHAR szText[128];
        LoadStringW(g_hInstance, IDS_NOPREVIEW, szText, _countof(szText));

        SelectObject(hdcMem, GetStockFont(DEFAULT_GUI_FONT));
        OffsetRect(&rcClient, -prcPaint->left, -prcPaint->top);
        DrawTextW(hdcMem, szText, -1, &rcClient, DT_SINGLELINE | DT_CENTER | DT_VCENTER |
                                                 DT_NOPREFIX);
    }
    else
    {
        UINT ImageWidth, ImageHeight;

        GdipGetImageWidth(g_pImage, &ImageWidth);
        GdipGetImageHeight(g_pImage, &ImageHeight);

        ZoomedWidth  = (ImageWidth  * pData->m_nZoomPercents) / 100;
        ZoomedHeight = (ImageHeight * pData->m_nZoomPercents) / 100;

        GdipCreateFromHDC(hdcMem, &graphics);
        if (!graphics)
        {
            DPRINT1("error: GdipCreateFromHDC\n");
            return;
        }

        GdipGetImageFlags(g_pImage, &uFlags);

        if (pData->m_nZoomPercents % 100 == 0)
        {
            GdipSetInterpolationMode(graphics, InterpolationModeNearestNeighbor);
            GdipSetSmoothingMode(graphics, SmoothingModeNone);
        }
        else
        {
            GdipSetInterpolationMode(graphics, InterpolationModeHighQualityBilinear);
            GdipSetSmoothingMode(graphics, SmoothingModeHighQuality);
        }

        rect.left   = (rcClient.right  - ZoomedWidth ) / 2;
        rect.top    = (rcClient.bottom - ZoomedHeight) / 2;
        rect.right  = rect.left + ZoomedWidth;
        rect.bottom = rect.top  + ZoomedHeight;
        OffsetRect(&rect,
                   -prcPaint->left - pData->m_xScrollOffset,
                   -prcPaint->top  - pData->m_yScrollOffset);

        InflateRect(&rect, +1, +1); /* Add Rectangle() pen width */

        /* Draw a rectangle. Fill by checker board if necessary */
        if (uFlags & (ImageFlagsHasAlpha | ImageFlagsHasTranslucent))
            hbrOld = SelectObject(hdcMem, CreateCheckerBoardBrush());
        else
            hbrOld = SelectObject(hdcMem, GetStockBrush(NULL_BRUSH));
        hPenOld = SelectObject(hdcMem, hPen);
        Rectangle(hdcMem, rect.left, rect.top, rect.right, rect.bottom);
        DeleteObject(SelectObject(hdcMem, hbrOld));
        DeleteObject(SelectObject(hdcMem, hPenOld));

        InflateRect(&rect, -1, -1); /* Subtract Rectangle() pen width */

        /* Image attributes are required to draw image correctly */
        GdipCreateImageAttributes(&imageAttributes);
        GdipSetImageAttributesWrapMode(imageAttributes, WrapModeTile,
                                       GetBkColor(hdcMem) | 0xFF000000, TRUE);

        /* Draw image. -0.5f is used for interpolation */
        GdipDrawImageRectRect(graphics, g_pImage,
                              rect.left, rect.top,
                              rect.right - rect.left, rect.bottom - rect.top,
                              -0.5f, -0.5f, ImageWidth, ImageHeight,
                              UnitPixel, imageAttributes, NULL, NULL);

        GdipDisposeImageAttributes(imageAttributes);
        GdipDeleteGraphics(graphics);
    }

    BitBlt(hdc, prcPaint->left, prcPaint->top, paintSize.cx, paintSize.cy, hdcMem, 0, 0, SRCCOPY);
    DeleteObject(SelectObject(hdcMem, hbmOld));
    DeleteDC(hdcMem);
}

static VOID
ZoomWnd_OnPaint(PPREVIEW_DATA pData, HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hDC;
    RECT rcClient;

    hDC = BeginPaint(hwnd, &ps);
    if (hDC)
    {
        GetClientRect(hwnd, &rcClient);
        ZoomWnd_OnDraw(pData, hDC, &ps.rcPaint, &rcClient);
        EndPaint(hwnd, &ps);
    }
}

static VOID
ImageView_ResetSettings(VOID)
{
    g_Settings.Maximized = FALSE;
    g_Settings.X         = CW_USEDEFAULT;
    g_Settings.Y         = CW_USEDEFAULT;
    g_Settings.Width     = 520;
    g_Settings.Height    = 400;
}

static BOOL
ImageView_LoadSettings(VOID)
{
    HKEY hKey;
    DWORD dwSize;
    LSTATUS nError;

    nError = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ReactOS\\shimgvw", 0, KEY_READ, &hKey);
    if (nError != ERROR_SUCCESS)
        return FALSE;

    dwSize = sizeof(g_Settings);
    nError = RegQueryValueExW(hKey, L"Settings", NULL, NULL, (LPBYTE)&g_Settings, &dwSize);
    RegCloseKey(hKey);

    return ((nError == ERROR_SUCCESS) && (dwSize == sizeof(g_Settings)));
}

static VOID
ImageView_SaveSettings(VOID)
{
    HKEY hKey;
    LSTATUS nError;

    nError = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ReactOS\\shimgvw",
                             0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (nError != ERROR_SUCCESS)
        return;

    RegSetValueExW(hKey, L"Settings", 0, REG_BINARY, (LPBYTE)&g_Settings, sizeof(g_Settings));
    RegCloseKey(hKey);
}

/* ---- ribbon chrome: host side of the command model ---- */


static BOOL
Preview_HasEncoders(VOID)
{
    static BOOL s_bKnown = FALSE, s_bHas = FALSE;
    if (!s_bKnown)
    {
        UINT num = 0, size = 0;
        s_bHas = (GdipGetImageEncodersSize(&num, &size) == Ok && num > 0);
        s_bKnown = TRUE;
    }
    return s_bHas;
}

static BOOL
RibbonPredicate(void *ctx, VRB_PRED pred)
{
    PPREVIEW_DATA pData = (PPREVIEW_DATA)ctx;
    PPREVIEW_DATA pFull;

    switch (pred)
    {
        case VRB_P_ALWAYS:          return TRUE;
        case VRB_P_HAS_IMAGE:       return g_pImage != NULL;
        case VRB_P_HAS_LIST:
            return g_pCurrentFile && g_pCurrentFile->Next && g_pCurrentFile->Next != g_pCurrentFile;
        case VRB_P_CAN_ZOOM_IN:     return pData->m_nZoomPercents < (INT)MAX_ZOOM;
        case VRB_P_CAN_ZOOM_OUT:    return pData->m_nZoomPercents > (INT)MIN_ZOOM;
        case VRB_P_IS_FIT:          return g_pImage && pData->m_bFit;
        case VRB_P_IS_ACTUAL:       return g_pImage && pData->m_nZoomPercents == 100;
        case VRB_P_IS_FULLSCREEN:   return IsWindowVisible(g_hwndFullscreen);
        case VRB_P_SLIDESHOW_RUNNING:
            pFull = IsWindowVisible(g_hwndFullscreen) ? Preview_GetData(g_hwndFullscreen) : NULL;
            return pFull && pFull->m_nTimerInterval != 0;
        case VRB_P_IS_ANIMATED:     return g_pImage && pData->m_Anime.m_nFrameCount > 1;
        case VRB_P_CAN_SAVE:        return g_pImage && Preview_HasEncoders();
        /* Same gates as the toolbar: the file is there, the shell verb exists. */
        case VRB_P_CAN_DELETE:      return g_szFile[0] && pData->m_bEnDelete;
        case VRB_P_CAN_PRINT:       return g_szFile[0] && pData->m_bEnPrint;
        case VRB_P_CAN_EDIT:        return g_szFile[0] && pData->m_bEnModify;
        case VRB_P_STATUSBAR_SHOWN: return LeewayFrameIsStatusBarShown(pData->m_pFrame);
        case VRB_P_RIBBON_MINIMIZED:return FALSE; /* the framework owns it */
        default:                    break;
    }
    return FALSE;
}

static HRESULT
RibbonExecute(void *ctx, UINT CmdId)
{
    PPREVIEW_DATA pData = (PPREVIEW_DATA)ctx;
    /* Every command goes through WM_COMMAND, as an accelerator does, so the
     * ribbon, the accelerators and the toolbar share one handler. */
    SendMessageW(pData->m_hwnd, WM_COMMAND, CmdId, 0);
    return S_OK;
}

static UINT
RibbonZoomPercent(void *ctx)
{
    return (UINT)((PPREVIEW_DATA)ctx)->m_nZoomPercents;
}

static VOID Preview_SetZoomLevel(PPREVIEW_DATA pData, UINT Percent);

/* The frame reports a slider drag here. */
static void
FrameZoom(UINT32 Percent, void *ctx)
{
    Preview_SetZoomLevel((PPREVIEW_DATA)ctx, Percent);
}

/* The ribbon changed height (minimise, QAT below, DPI): the frame re-ran the layout. */
static void
FrameRelayout(const LeewayUIFrameRects *rects, void *ctx)
{
    PPREVIEW_DATA pData = (PPREVIEW_DATA)ctx;
    MoveWindow(pData->m_hwndZoom, rects->work.left, rects->work.top,
               rects->work.right - rects->work.left, rects->work.bottom - rects->work.top, TRUE);
}

static BOOL
Preview_CreateRibbonChrome(PPREVIEW_DATA pData)
{
    VRB_HOST host;
    LeewayUIFrameDesc desc;

    host.ctx = pData;
    host.Predicate = RibbonPredicate;
    host.Execute = RibbonExecute;
    host.GetZoomPercent = RibbonZoomPercent;

    if (FAILED(VRB_RibbonCreate(pData->m_hwnd, g_hInstance, &host, &pData->m_pRibbon)))
    {
        pData->m_pRibbon = NULL;
        return FALSE;
    }

    /* The frame owns the status bar, the zoom slider and the layout; the
     * ribbon stays with the adapter and is lent to the frame. */
    ZeroMemory(&desc, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.owner = pData->m_hwnd;
    desc.instance = g_hInstance;
    desc.app_name = L"PhotoViewer";
    desc.flags = LWUI_FRAME_STATUSBAR | LWUI_FRAME_ZOOM_SLIDER;
    desc.default_chrome = LWUI_CHROME_RIBBON;
    desc.ribbon = VRB_RibbonFramework(pData->m_pRibbon);
    desc.zoom_levels = g_VrbZoomPercents;
    desc.zoom_level_count = VRB_ZOOM_LEVELS;
    desc.on_zoom = FrameZoom;
    desc.on_layout = FrameRelayout;
    desc.app_context = pData;
    if (FAILED(LeewayFrameCreate(&desc, &pData->m_pFrame)) ||
        LeewayFrameGetChrome(pData->m_pFrame) != LWUI_CHROME_RIBBON)
    {
        LeewayFrameDestroy(pData->m_pFrame);
        pData->m_pFrame = NULL;
        VRB_RibbonDestroy(pData->m_pRibbon);
        pData->m_pRibbon = NULL;
        return FALSE;
    }
    return TRUE;
}

/* Places the ribbon on top, the status bar or the toolbar at the bottom and
 * the image between them. */
/* Ribbon chrome: the frame lays out ribbon, status bar and slider and says
 * what is left. Pill chrome: the toolbar sits at the bottom. */
static VOID
Preview_LayoutMain(PPREVIEW_DATA pData, INT cx, INT cy)
{
    RECT rc;
    INT bottom = 0;

    if (pData->m_pFrame)
    {
        LeewayUIFrameRects fr;

        SetRect(&rc, 0, 0, cx, cy);
        LeewayFrameLayout(pData->m_pFrame, &rc, &fr);
        MoveWindow(pData->m_hwndZoom, fr.work.left, fr.work.top,
                   fr.work.right - fr.work.left, fr.work.bottom - fr.work.top, TRUE);
        return;
    }

    if (pData->m_hwndToolBar)
    {
        SendMessageW(pData->m_hwndToolBar, TB_AUTOSIZE, 0, 0);
        GetWindowRect(pData->m_hwndToolBar, &rc);
        bottom += rc.bottom - rc.top;
    }
    MoveWindow(pData->m_hwndZoom, 0, 0, cx, max(cy - bottom, 0), TRUE);
}

static BOOL
Preview_CreateToolBar(PPREVIEW_DATA pData)
{
    HWND hwndToolBar;
    HIMAGELIST hImageList, hOldImageList;
    DWORD style = WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS;

    if (!Preview_IsMainWnd(pData->m_hwnd))
        return TRUE; /* FIXME */

    /* ChromeStyle ribbon: the ribbon and the status bar replace the toolbar.
     * If the ribbon cannot be made the pill toolbar below is the fallback. */
    if (VRB_RibbonIsDefaultChrome() && Preview_CreateRibbonChrome(pData))
    {
        Preview_SyncZoomUi(pData);
        return TRUE;
    }

    style |= CCS_BOTTOM;
    hwndToolBar = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL, style,
                                  0, 0, 0, 0, pData->m_hwnd, (HMENU)IDC_TOOLBAR, g_hInstance, NULL);
    if (!hwndToolBar)
        return FALSE;

    pData->m_hwndToolBar = hwndToolBar;

    SendMessageW(hwndToolBar, TB_BUTTONSTRUCTSIZE, sizeof(s_Buttons[0]), 0);
    SendMessageW(hwndToolBar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_HIDECLIPPEDBUTTONS);

    hImageList = ImageList_Create(TB_IMAGE_WIDTH, TB_IMAGE_HEIGHT, ILC_MASK | ILC_COLOR24, 1, 1);
    if (hImageList == NULL)
        return FALSE;

    for (UINT n = 0; n < _countof(s_ButtonConfig); n++)
    {
        HBITMAP hBitmap = LoadBitmapW(g_hInstance, MAKEINTRESOURCEW(s_ButtonConfig[n].idb));
        ImageList_AddMasked(hImageList, hBitmap, RGB(255, 255, 255));
        DeleteObject(hBitmap);
    }

    hOldImageList = (HIMAGELIST)SendMessageW(hwndToolBar, TB_SETIMAGELIST, 0, (LPARAM)hImageList);
    ImageList_Destroy(hOldImageList);

    SendMessageW(hwndToolBar, TB_ADDBUTTONS, _countof(s_Buttons), (LPARAM)s_Buttons);

    return TRUE;
}

static VOID
Preview_EndSlideShow(HWND hwnd)
{
    if (Preview_IsMainWnd(hwnd))
        return;

    KillTimer(hwnd, SLIDESHOW_TIMER_ID);
    ShowWindow(g_hMainWnd, SW_SHOW);
    ShowWindow(hwnd, SW_HIDE);
    Preview_ResetZoom(Preview_GetData(g_hMainWnd));
    Preview_RibbonInvalidateAll(Preview_GetData(g_hMainWnd));
}

static VOID
GenerateSetCursor(HWND hwnd, UINT uMsg)
{
    SendMessage(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELONG(HTCLIENT, uMsg));
}

static VOID
ZoomWnd_StopHideCursor(PPREVIEW_DATA pData)
{
    pData->m_bHideCursor = FALSE;
    KillTimer(pData->m_hwndZoom, HIDECURSOR_TIMER_ID);
}

static VOID
ZoomWnd_OnButtonDown(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    PPREVIEW_DATA pData = Preview_GetData(hwnd);
    HWND hParent = GetParent(hwnd);
    if ((uMsg == WM_LBUTTONDOWN) || (uMsg == WM_RBUTTONDOWN))
    {
        if (!Preview_IsMainWnd(hParent))
            Preview_EndSlideShow(hParent);
        return;
    }

    ZoomWnd_StopHideCursor(pData);
    pData->m_nMouseDownMsg = uMsg;
    pData->m_ptOrigin.x = GET_X_LPARAM(lParam);
    pData->m_ptOrigin.y = GET_Y_LPARAM(lParam);
    SetCapture(hwnd);
    SetCursor(LoadCursorW(g_hInstance, MAKEINTRESOURCEW(IDC_HANDDRAG)));
}

static VOID
ZoomWnd_OnMouseMove(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    PPREVIEW_DATA pData = Preview_GetData(hwnd);
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

    if (!Preview_IsMainWnd(pData->m_hwnd))
    {
        ZoomWnd_StopHideCursor(pData);
        if (!pData->m_nMouseDownMsg)
            SetTimer(hwnd, HIDECURSOR_TIMER_ID, HIDECURSOR_TIMER_TIMEOUT, NULL);
    }

    if (pData->m_nMouseDownMsg == WM_MBUTTONDOWN)
    {
        INT x = GetScrollPos(hwnd, SB_HORZ) - (pt.x - pData->m_ptOrigin.x);
        INT y = GetScrollPos(hwnd, SB_VERT) - (pt.y - pData->m_ptOrigin.y);
        SendMessageW(hwnd, WM_HSCROLL, MAKEWPARAM(SB_THUMBPOSITION, x), 0);
        SendMessageW(hwnd, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, y), 0);
        pData->m_ptOrigin = pt;
    }
}

static BOOL
ZoomWnd_OnSetCursor(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    PPREVIEW_DATA pData = Preview_GetData(hwnd);
    if (pData->m_nMouseDownMsg == WM_MBUTTONDOWN)
    {
        SetCursor(LoadCursorW(g_hInstance, MAKEINTRESOURCEW(IDC_HANDDRAG)));
        return TRUE;
    }

    if (pData->m_bHideCursor)
    {
        SetCursor(NULL); /* Hide cursor in fullscreen */
        return TRUE;
    }
    return FALSE;
}

static VOID
ZoomWnd_OnButtonUp(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    PPREVIEW_DATA pData = Preview_GetData(hwnd);
    BOOL wasdrag = pData->m_nMouseDownMsg == WM_MBUTTONDOWN;

    pData->m_nMouseDownMsg = 0;
    if (wasdrag)
        GenerateSetCursor(hwnd, uMsg); /* Reset to default cursor */
    ReleaseCapture();

    if (!Preview_IsMainWnd(pData->m_hwnd))
        SetTimer(hwnd, HIDECURSOR_TIMER_ID, HIDECURSOR_TIMER_TIMEOUT, NULL);
}

static VOID
ZoomWnd_OnHVScroll(PPREVIEW_DATA pData, HWND hwnd, WPARAM wParam, BOOL bVertical)
{
    UINT ImageWidth, ImageHeight, ZoomedWidth, ZoomedHeight;
    RECT rcClient;
    UINT nBar = (bVertical ? SB_VERT : SB_HORZ);
    SCROLLINFO si = { sizeof(si), SIF_ALL };
    GetScrollInfo(hwnd, nBar, &si);

    if (!g_pImage)
        return;

    if (bVertical)
    {
        if (!(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VSCROLL))
            return;
    }
    else
    {
        if (!(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_HSCROLL))
            return;
    }

    switch (LOWORD(wParam))
    {
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            si.nPos = (SHORT)HIWORD(wParam);
            break;
        case SB_LINELEFT:
            si.nPos -= 48;
            break;
        case SB_LINERIGHT:
            si.nPos += 48;
            break;
        case SB_PAGELEFT:
            si.nPos -= si.nPage;
            break;
        case SB_PAGERIGHT:
            si.nPos += si.nPage;
            break;
    }

    si.fMask = SIF_POS;
    SetScrollInfo(hwnd, nBar, &si, TRUE);
    GetScrollInfo(hwnd, nBar, &si);

    GetClientRect(hwnd, &rcClient);

    if (bVertical)
    {
        GdipGetImageHeight(g_pImage, &ImageHeight);
        ZoomedHeight = (ImageHeight * pData->m_nZoomPercents) / 100;
        pData->m_yScrollOffset = si.nPos - (ZoomedHeight - rcClient.bottom) / 2;
    }
    else
    {
        GdipGetImageWidth(g_pImage, &ImageWidth);
        ZoomedWidth = (ImageWidth  * pData->m_nZoomPercents) / 100;
        pData->m_xScrollOffset = si.nPos - (ZoomedWidth - rcClient.right) / 2;
    }

    InvalidateRect(hwnd, NULL, TRUE);
}

static VOID
ZoomWnd_OnMouseWheel(HWND hwnd, INT x, INT y, INT zDelta, UINT fwKeys)
{
    PPREVIEW_DATA pData = Preview_GetData(hwnd);
    if (zDelta == 0)
        return;

    if (GetKeyState(VK_CONTROL) < 0)
    {
        Preview_ZoomInOrOut(pData, zDelta > 0);
    }
    else if (GetKeyState(VK_SHIFT) < 0)
    {
        if (zDelta > 0)
            SendMessageW(hwnd, WM_HSCROLL, SB_LINELEFT, 0);
        else
            SendMessageW(hwnd, WM_HSCROLL, SB_LINERIGHT, 0);
    }
    else
    {
        if (zDelta > 0)
            SendMessageW(hwnd, WM_VSCROLL, SB_LINEUP, 0);
        else
            SendMessageW(hwnd, WM_VSCROLL, SB_LINEDOWN, 0);
    }
}

LRESULT CALLBACK
ZoomWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    PPREVIEW_DATA pData = Preview_GetData(hwnd);
    switch (uMsg)
    {
        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_RBUTTONDOWN:
        {
            ZoomWnd_OnButtonDown(hwnd, uMsg, wParam, lParam);
            break;
        }
        case WM_MOUSEMOVE:
        {
            ZoomWnd_OnMouseMove(hwnd, uMsg, wParam, lParam);
            break;
        }
        case WM_SETCURSOR:
        {
            if (!ZoomWnd_OnSetCursor(hwnd, uMsg, wParam, lParam))
                return DefWindowProcW(hwnd, uMsg, wParam, lParam);
        }
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
        case WM_RBUTTONUP:
        {
            ZoomWnd_OnButtonUp(hwnd, uMsg, wParam, lParam);
            goto doDefault;
        }
        case WM_LBUTTONDBLCLK:
        {
            if (Preview_IsMainWnd(pData->m_hwnd))
                Preview_ToggleSlideShowEx(pData, FALSE);
            break;
        }
        case WM_PAINT:
        {
            ZoomWnd_OnPaint(pData, hwnd);
            break;
        }
        case WM_MOUSEWHEEL:
        {
            ZoomWnd_OnMouseWheel(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam),
                                 (SHORT)HIWORD(wParam), (UINT)LOWORD(wParam));
            break;
        }
        case WM_CONTEXTMENU:
            if (Preview_IsMainWnd(pData->m_hwnd) && *g_szFile)
                DoShellContextMenuOnFile(hwnd, g_szFile, lParam);
            break;
        case WM_HSCROLL:
        case WM_VSCROLL:
            ZoomWnd_OnHVScroll(pData, hwnd, wParam, uMsg == WM_VSCROLL);
            break;
        case WM_TIMER:
        {
            if (wParam == HIDECURSOR_TIMER_ID)
            {
                ZoomWnd_StopHideCursor(pData);
                if (IsWindowVisible(hwnd))
                {
                    pData->m_bHideCursor = TRUE;
                    GenerateSetCursor(hwnd, uMsg);
                }
            }
            if (Anime_OnTimer(&pData->m_Anime, wParam))
            {
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        }
        default: doDefault:
        {
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
        }
    }
    return 0;
}

static BOOL
Preview_OnCreate(HWND hwnd, LPCREATESTRUCT pCS)
{
    DWORD exstyle = 0;
    HWND hwndZoom;
    PPREVIEW_DATA pData = QuickAlloc(sizeof(PREVIEW_DATA), TRUE);
    pData->m_hwnd = hwnd;
    /* The toolbar starts with every button enabled. */
    pData->m_bEnBestFit = pData->m_bEnRealSize = pData->m_bEnZoomIn = pData->m_bEnZoomOut = TRUE;
    pData->m_bEnDelete = pData->m_bEnSaveAs = pData->m_bEnPrint = pData->m_bEnModify = TRUE;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pData);

    DragAcceptFiles(hwnd, TRUE);

    if (g_hMainWnd == NULL)
    {
        g_hMainWnd = hwnd;
        exstyle |= WS_EX_CLIENTEDGE;
    }
    else if (g_hwndFullscreen == NULL)
    {
        g_hwndFullscreen = hwnd;
    }
    else
    {
        return FALSE;
    }

    hwndZoom = CreateWindowExW(exstyle, WC_ZOOM, NULL, WS_CHILD | WS_VISIBLE,
                               0, 0, 0, 0, hwnd, NULL, g_hInstance, NULL);
    if (!hwndZoom)
    {
        QuickFree(pData);
        return FALSE;
    }

    pData->m_hwndZoom = hwndZoom;
    SetWindowLongPtrW(hwndZoom, GWLP_USERDATA, (LONG_PTR)pData);
    Anime_SetTimerWnd(&pData->m_Anime, pData->m_hwndZoom);

    if (!Preview_CreateToolBar(pData))
    {
        QuickFree(pData);
        return FALSE;
    }

    if (pCS && pCS->lpCreateParams)
    {
        LPCWSTR pszFileName = (LPCWSTR)pCS->lpCreateParams;
        WCHAR szFile[MAX_PATH];

        /* Make sure the path has no quotes on it */
        StringCchCopyW(szFile, _countof(szFile), pszFileName);
        PathUnquoteSpacesW(szFile);

        g_pCurrentFile = pBuildFileList(szFile);
        Preview_LoadImage(pData, g_pCurrentFile);
    }

    return TRUE;
}

static VOID
Preview_OnMoveSize(HWND hwnd)
{
    WINDOWPLACEMENT wp;
    RECT *prc;

    if (IsIconic(hwnd) || !Preview_IsMainWnd(hwnd))
        return;

    wp.length = sizeof(WINDOWPLACEMENT);
    GetWindowPlacement(hwnd, &wp);

    /* Remember window position and size */
    prc = &wp.rcNormalPosition;
    g_Settings.X = prc->left;
    g_Settings.Y = prc->top;
    g_Settings.Width = prc->right - prc->left;
    g_Settings.Height = prc->bottom - prc->top;
    g_Settings.Maximized = IsZoomed(hwnd);
}

static VOID
Preview_OnSize(HWND hwnd)
{
    RECT rcClient;
    PPREVIEW_DATA pData = Preview_GetData(hwnd);
    INT cx, cy;

    /* We want 32-bit values. Don't use WM_SIZE lParam */
    GetClientRect(hwnd, &rcClient);
    cx = rcClient.right;
    cy = rcClient.bottom;

    if (Preview_IsMainWnd(pData->m_hwnd))
    {
        Preview_LayoutMain(pData, cx, cy);

        if (pData->m_nZoomPercents > 100)
            ZoomWnd_UpdateScroll(pData, FALSE);
        else if (!IsIconic(hwnd)) /* Is it not minimized? */
            Preview_ResetZoom(pData);

        Preview_OnMoveSize(hwnd);
    }
    else
    {
        MoveWindow(pData->m_hwndZoom, 0, 0, cx, cy, TRUE);
    }
}

static VOID
Preview_Delete(PPREVIEW_DATA pData)
{
    WCHAR szCurFile[MAX_PATH + 1], szNextFile[MAX_PATH];
    SHFILEOPSTRUCTW FileOp = { pData->m_hwnd, FO_DELETE };
    UINT error;

    /* FileOp.pFrom must be double-null-terminated */
    GetFullPathNameW(g_szFile, _countof(szCurFile) - 1, szCurFile, NULL);
    szCurFile[_countof(szCurFile) - 2] = UNICODE_NULL; /* Avoid buffer overrun */
    szCurFile[lstrlenW(szCurFile) + 1] = UNICODE_NULL;

    szNextFile[0] = UNICODE_NULL;
    if (g_pCurrentFile)
    {
        GetFullPathNameW(g_pCurrentFile->Next->FileName, _countof(szNextFile), szNextFile, NULL);
        szNextFile[_countof(szNextFile) - 1] = UNICODE_NULL; /* Avoid buffer overrun */
    }

    /* Confirm file deletion and delete if allowed */
    FileOp.pFrom = szCurFile;
    FileOp.fFlags = GetKeyState(VK_SHIFT) < 0 ? 0 : FOF_ALLOWUNDO;
    error = g_szFile[0] ? SHFileOperationW(&FileOp) : ERROR_FILE_NOT_FOUND;
    if (error)
        return;

    /* Reload the file list and go next file */
    pFreeFileList(g_pCurrentFile);
    g_pCurrentFile = pBuildFileList(szNextFile);
    Preview_LoadImage(pData, g_pCurrentFile);
}

static VOID
Preview_Edit(HWND hwnd)
{
    PPREVIEW_DATA pData = Preview_GetData(g_hMainWnd);
    ShellExecuteVerb(pData->m_hwnd, L"edit", g_szFile, TRUE);
}

static VOID
Preview_ToggleSlideShowEx(PPREVIEW_DATA pData, BOOL StartTimer)
{
    if (!IsWindow(g_hwndFullscreen))
    {
        DWORD style = WS_POPUP | WS_CLIPSIBLINGS, exstyle = WS_EX_TOPMOST;
        WCHAR szTitle[256];
        LoadStringW(g_hInstance, IDS_APPTITLE, szTitle, _countof(szTitle));
        g_hwndFullscreen = CreateWindowExW(exstyle, WC_PREVIEW, szTitle, style,
                                           0, 0, 0, 0, NULL, NULL, g_hInstance, NULL);
    }

    if (IsWindowVisible(g_hwndFullscreen))
    {
        Preview_EndSlideShow(g_hwndFullscreen);
    }
    else
    {
        PPREVIEW_DATA pSlideData = Preview_GetData(g_hwndFullscreen);
        pSlideData->m_nTimerInterval = StartTimer ? SLIDESHOW_TIMER_INTERVAL : 0;
        ShowWindow(g_hwndFullscreen, SW_SHOWMAXIMIZED);
        ShowWindow(g_hMainWnd, SW_HIDE);
        Preview_ResetZoom(pSlideData);
        Preview_RestartTimer(g_hwndFullscreen);
        PostMessage(pSlideData->m_hwndZoom, WM_MOUSEMOVE, 0, 0); /* Start hide cursor */
        Preview_RibbonInvalidateAll(Preview_GetData(g_hMainWnd));
    }
}

static inline VOID
Preview_ToggleSlideShow(PPREVIEW_DATA pData)
{
    Preview_ToggleSlideShowEx(pData, TRUE);
}

static VOID
Preview_GoNextPic(PPREVIEW_DATA pData, BOOL bNext)
{
    Preview_RestartTimer(pData->m_hwnd);
    if (g_pCurrentFile)
    {
        if (bNext)
            g_pCurrentFile = g_pCurrentFile->Next;
        else
            g_pCurrentFile = g_pCurrentFile->Prev;
        Preview_LoadImage(pData, g_pCurrentFile);
    }
}

/* Absolute zoom, the setter behind the zoom gallery and the slider. */
static VOID
Preview_SetZoomLevel(PPREVIEW_DATA pData, UINT Percent)
{
    if (g_pImage == NULL)
        return;
    Preview_UpdateZoom(pData, Percent, TRUE, TRUE);
}

/* Open...: the codec filter is the one of the folder list, plus "all". */
static VOID
Preview_Open(PPREVIEW_DATA pData)
{
    OPENFILENAMEW ofn;
    ImageCodecInfo *codecInfo;
    WCHAR szFile[MAX_PATH], szAll[64];
    WCHAR *szFilter, *szAllExts, *c;
    UINT num = 0, size = 0, j;
    size_t cchFilter, cchAllExts; /* size_t: StringCchPrintfExW takes size_t* */
    PCWSTR pszExtra = GetExtraExtensionsGdipList();

    if (GdipGetImageDecodersSize(&num, &size) != Ok || !num)
        return;
    codecInfo = QuickAlloc(size, FALSE);
    if (!codecInfo)
        return;
    GdipGetImageDecoders(num, size, codecInfo);

    cchFilter = 128;
    cchAllExts = wcslen(pszExtra) + 2;
    for (j = 0; j < num; ++j)
    {
        cchFilter += wcslen(codecInfo[j].FormatDescription) + wcslen(codecInfo[j].FilenameExtension) * 2 + 8;
        cchAllExts += wcslen(codecInfo[j].FilenameExtension) + 1;
    }
    cchFilter += cchAllExts;
    szFilter = QuickAlloc(cchFilter * sizeof(WCHAR), TRUE);
    szAllExts = QuickAlloc(cchAllExts * sizeof(WCHAR), TRUE);
    if (!szFilter || !szAllExts)
    {
        QuickFree(szFilter);
        QuickFree(szAllExts);
        QuickFree(codecInfo);
        return;
    }

    for (j = 0; j < num; ++j)
    {
        if (szAllExts[0])
            StringCchCatW(szAllExts, cchAllExts, L";");
        StringCchCatW(szAllExts, cchAllExts, codecInfo[j].FilenameExtension);
    }
    if (pszExtra[0])
    {
        if (szAllExts[0])
            StringCchCatW(szAllExts, cchAllExts, L";");
        StringCchCatW(szAllExts, cchAllExts, pszExtra);
    }

    szAll[0] = UNICODE_NULL;
    LoadStringW(g_hInstance, IDS_OPEN_ALLPICTURES, szAll, _countof(szAll));

    /* "All pictures", then one pair per decoder; each part ends in a NUL. */
    c = szFilter;
    StringCchPrintfExW(c, cchFilter, &c, &cchFilter, 0, L"%ls", szAll);
    ++c; --cchFilter;
    StringCchPrintfExW(c, cchFilter, &c, &cchFilter, 0, L"%ls", szAllExts);
    ++c; --cchFilter;
    for (j = 0; j < num; ++j)
    {
        StringCchPrintfExW(c, cchFilter, &c, &cchFilter, 0, L"%ls (%ls)",
                           codecInfo[j].FormatDescription, codecInfo[j].FilenameExtension);
        ++c; --cchFilter;
        StringCchPrintfExW(c, cchFilter, &c, &cchFilter, 0, L"%ls", codecInfo[j].FilenameExtension);
        ++c; --cchFilter;
    }

    ZeroMemory(szFile, sizeof(szFile));
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = pData->m_hwnd;
    ofn.lpstrFile   = szFile;
    ofn.nMaxFile    = _countof(szFile);
    ofn.lpstrFilter = szFilter;
    ofn.nFilterIndex = 1;
    ofn.Flags       = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (GetOpenFileNameW(&ofn))
    {
        pFreeFileList(g_pCurrentFile);
        g_pCurrentFile = pBuildFileList(szFile);
        Preview_LoadImage(pData, g_pCurrentFile);
    }

    QuickFree(szFilter);
    QuickFree(szAllExts);
    QuickFree(codecInfo);
}

/* Set as desktop background: the desktop takes a bitmap, so anything that is
 * not one is written out as a BMP under %APPDATA%\Leeway\PhotoViewer. */
static VOID
Preview_SetWallpaper(PPREVIEW_DATA pData)
{
    WCHAR szPath[MAX_PATH];
    PCWSTR pszSource = g_szFile;
    PCWSTR pszExt = PathFindExtensionW(g_szFile);

    if (g_pImage == NULL || !g_szFile[0])
        return;

    if (_wcsicmp(pszExt, L".bmp") != 0)
    {
        ImageCodecInfo *codecInfo;
        UINT num = 0, size = 0, j;
        BOOL bSaved = FALSE;

        if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL,
                                    SHGFP_TYPE_CURRENT, szPath)) ||
            !PathAppendW(szPath, L"Leeway"))
        {
            ErrorBox(pData->m_hwnd, ERROR_PATH_NOT_FOUND);
            return;
        }
        CreateDirectoryW(szPath, NULL);
        if (!PathAppendW(szPath, L"PhotoViewer"))
            return;
        CreateDirectoryW(szPath, NULL);
        if (!PathAppendW(szPath, L"wallpaper.bmp"))
            return;

        if (GdipGetImageEncodersSize(&num, &size) != Ok || !num)
            return;
        codecInfo = QuickAlloc(size, FALSE);
        if (!codecInfo)
            return;
        GdipGetImageEncoders(num, size, codecInfo);
        for (j = 0; j < num; ++j)
        {
            if (IsEqualGUID(&ImageFormatBMP, &codecInfo[j].FormatID))
            {
                Anime_Pause(&pData->m_Anime);
                bSaved = (GdipSaveImageToFile(g_pImage, szPath, &codecInfo[j].Clsid, NULL) == Ok);
                Anime_Start(&pData->m_Anime, 0);
                break;
            }
        }
        QuickFree(codecInfo);
        if (!bSaved)
        {
            ErrorBox(pData->m_hwnd, ERROR_WRITE_FAULT);
            return;
        }
        pszSource = szPath;
    }

    if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)pszSource,
                               SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
        ErrorBox(pData->m_hwnd, GetLastError());
}

static VOID
Preview_SetStatusBarShown(PPREVIEW_DATA pData, BOOL bShow)
{
    if (!pData->m_pFrame)
        return;
    LeewayFrameShowStatusBar(pData->m_pFrame, bShow, TRUE);
    Preview_OnSize(pData->m_hwnd);
    Preview_RibbonInvalidate(pData, VRB_LWUI_STATUSBAR_TOGGLE);
}

static VOID
Preview_OnCommand(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
    PPREVIEW_DATA pData = Preview_GetData(hwnd);
    UINT nCommandID = LOWORD(wParam);
    if ((!HIWORD(wParam) || !lParam) && IsCommandEnabled(nCommandID) == S_FALSE)
    {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    switch (nCommandID)
    {
        case IDC_PREV_PIC:
            Preview_GoNextPic(pData, FALSE);
            break;

        case IDC_NEXT_PIC:
            Preview_GoNextPic(pData, TRUE);
            break;

        case IDC_BEST_FIT:
            Preview_ResetZoom(pData);
            break;

        case IDC_REAL_SIZE:
            Preview_UpdateZoom(pData, 100, TRUE, FALSE);
            break;

        case IDC_SLIDE_SHOW:
            Preview_ToggleSlideShow(pData);
            break;

        case IDC_ZOOM_IN:
            Preview_ZoomInOrOut(pData, TRUE);
            break;

        case IDC_ZOOM_OUT:
            Preview_ZoomInOrOut(pData, FALSE);
            break;

        case IDC_ENDSLIDESHOW:
            Preview_EndSlideShow(hwnd);
            break;

        case IDC_TOGGLEFULLSCREEN:
            Preview_ToggleSlideShowEx(pData, FALSE);
            break;

        case IDC_INCTIMER:
        case IDC_DECTIMER:
            Preview_ChangeSlideShowTimer(pData, nCommandID == IDC_INCTIMER);
            break;

        default:
            break;
    }

    if (!Preview_IsMainWnd(hwnd))
        return;

    // The following commands are for main window only:
    switch (nCommandID)
    {
        case IDC_SAVEAS:
            Preview_pSaveImageAs(pData);
            break;

        case IDC_PRINT:
            Preview_pPrintImage(pData);
            break;

        case IDC_ROT_CLOCKW:
            if (g_pImage)
            {
                GdipImageRotateFlip(g_pImage, Rotate270FlipNone);
                Preview_UpdateImage(pData);
            }
            break;

        case IDC_ROT_COUNCW:
            if (g_pImage)
            {
                GdipImageRotateFlip(g_pImage, Rotate90FlipNone);
                Preview_UpdateImage(pData);
            }
            break;

        case IDC_ROT_CWSAVE:
            if (g_pImage)
            {
                GdipImageRotateFlip(g_pImage, Rotate270FlipNone);
                Preview_pSaveImage(pData, g_szFile);
                Preview_UpdateImage(pData);
            }
            break;

        case IDC_ROT_CCWSAVE:
            if (g_pImage)
            {
                GdipImageRotateFlip(g_pImage, Rotate90FlipNone);
                Preview_pSaveImage(pData, g_szFile);
                Preview_UpdateImage(pData);
            }
            break;

        case IDC_DELETE:
            Preview_Delete(pData);
            break;

        case IDC_MODIFY:
            Preview_Edit(hwnd);
            break;

        case IDC_HELP_TOC:
        case VRB_LWUI_HELP:
            DisplayHelp(hwnd);
            break;

        case VRB_CMD_OPEN:
            Preview_Open(pData);
            break;

        case VRB_CMD_WALLPAPER:
            Preview_SetWallpaper(pData);
            break;

        case VRB_CMD_EXIT:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;

        case VRB_LWUI_STATUSBAR_TOGGLE:
            Preview_SetStatusBarShown(pData, !LeewayFrameIsStatusBarShown(pData->m_pFrame));
            break;

        case VRB_CMD_ZOOM_FIT:
            Preview_ResetZoom(pData);
            break;

        case VRB_CMD_ZOOM_ACTUAL:
            Preview_UpdateZoom(pData, 100, TRUE, FALSE);
            break;

        default:
        {
            UINT Percent;
            if (VRB_ZoomPercentOfCommand(nCommandID, &Percent))
                Preview_SetZoomLevel(pData, Percent);
            break;
        }
    }
}

static LRESULT
Preview_OnNotify(HWND hwnd, LPNMHDR pnmhdr)
{
    switch (pnmhdr->code)
    {
        case TTN_GETDISPINFOW:
        {
            LPTOOLTIPTEXTW lpttt = (LPTOOLTIPTEXTW)pnmhdr;
            lpttt->hinst = g_hInstance;
            lpttt->lpszText = MAKEINTRESOURCEW(s_ButtonConfig[lpttt->hdr.idFrom - IDC_TOOL_BASE].ids);
            break;
        }
    }
    return 0;
}

static VOID
Preview_OnDestroy(HWND hwnd)
{
    PPREVIEW_DATA pData = Preview_GetData(hwnd);

    KillTimer(hwnd, SLIDESHOW_TIMER_ID);
    KillTimer(hwnd, HIDECURSOR_TIMER_ID);

    pFreeFileList(g_pCurrentFile);
    g_pCurrentFile = NULL;

    Preview_pFreeImage(pData);

    SetWindowLongPtrW(pData->m_hwndZoom, GWLP_USERDATA, 0);
    DestroyWindow(pData->m_hwndZoom);
    pData->m_hwndZoom = NULL;

    DestroyWindow(pData->m_hwndToolBar);
    pData->m_hwndToolBar = NULL;

    LeewayFrameDestroy(pData->m_pFrame);
    pData->m_pFrame = NULL;
    VRB_RibbonDestroy(pData->m_pRibbon);
    pData->m_pRibbon = NULL;

    SetWindowLongPtrW(pData->m_hwnd, GWLP_USERDATA, 0);
    QuickFree(pData);

    PostQuitMessage(0);
}

static VOID
Preview_OnDropFiles(HWND hwnd, HDROP hDrop)
{
    WCHAR szFile[MAX_PATH];
    PPREVIEW_DATA pData = Preview_GetData(hwnd);

    DragQueryFileW(hDrop, 0, szFile, _countof(szFile));

    pFreeFileList(g_pCurrentFile);
    g_pCurrentFile = pBuildFileList(szFile);
    Preview_LoadImage(pData, g_pCurrentFile);

    DragFinish(hDrop);
}

LRESULT CALLBACK
PreviewWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_CREATE:
        {
            if (!Preview_OnCreate(hwnd, (LPCREATESTRUCT)lParam))
                return -1;
            break;
        }
        case WM_COMMAND:
        {
            Preview_OnCommand(hwnd, wParam, lParam);
            break;
        }
        case WM_NOTIFY:
        {
            /* The ribbon's height change goes to the frame first. */
            PPREVIEW_DATA pData = Preview_GetData(hwnd);
            LRESULT lr = 0;
            if (pData && pData->m_pFrame &&
                LeewayFrameHandleMessage(pData->m_pFrame, uMsg, wParam, lParam, &lr))
                return lr;
            return Preview_OnNotify(hwnd, (LPNMHDR)lParam);
        }
        case WM_DRAWITEM:
        case WM_CTLCOLORSTATIC:
        {
            /* Status bar parts and the zoom slider are drawn in the theme's colours. */
            PPREVIEW_DATA pData = Preview_GetData(hwnd);
            LRESULT lr = 0;
            if (pData && pData->m_pFrame &&
                LeewayFrameHandleMessage(pData->m_pFrame, uMsg, wParam, lParam, &lr))
                return lr;
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
        }
        case WM_GETMINMAXINFO:
        {
            MINMAXINFO *pMMI = (MINMAXINFO*)lParam;
            pMMI->ptMinTrackSize.x = 350;
            pMMI->ptMinTrackSize.y = 290;
            break;
        }
        case WM_MOVE:
        {
            Preview_OnMoveSize(hwnd);
            break;
        }
        case WM_SIZE:
        {
            Preview_OnSize(hwnd);
            break;
        }
        case WM_DROPFILES:
        {
            Preview_OnDropFiles(hwnd, (HDROP)wParam);
            break;
        }
        case WM_SYSCOLORCHANGE:
        {
            PPREVIEW_DATA pData = Preview_GetData(hwnd);
            InvalidateRect(pData->m_hwnd, NULL, TRUE);
            InvalidateRect(pData->m_hwndZoom, NULL, TRUE);
            VRB_RibbonOnThemeChanged(pData->m_pRibbon);
            LeewayFrameHandleMessage(pData->m_pFrame, uMsg, wParam, lParam, NULL);
            break;
        }
        case WM_DPICHANGED:
        {
            PPREVIEW_DATA pData = Preview_GetData(hwnd);
            if (!LeewayFrameHandleMessage(pData->m_pFrame, uMsg, wParam, lParam, NULL))
                VRB_RibbonOnDpiChanged(pData->m_pRibbon, HIWORD(wParam));
            Preview_OnSize(hwnd);
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
        }
        case WM_HSCROLL:
        {
            /* The status bar zoom slider (the frame reports through FrameZoom);
             * the zoom window has its own. */
            PPREVIEW_DATA pData = Preview_GetData(hwnd);
            LRESULT lr;
            if (pData && pData->m_pFrame)
                LeewayFrameHandleMessage(pData->m_pFrame, uMsg, wParam, lParam, &lr);
            break;
        }
        case WM_DESTROY:
        {
            Preview_OnDestroy(hwnd);
            break;
        }
        case WM_CONTEXTMENU:
        {
            PPREVIEW_DATA pData = Preview_GetData(hwnd);
            if ((int)lParam == -1)
                return ZoomWndProc(pData->m_hwndZoom, uMsg, wParam, lParam);
            break;
        }
        case WM_TIMER:
        {
            if (wParam == SLIDESHOW_TIMER_ID)
            {
                PPREVIEW_DATA pData = Preview_GetData(hwnd);
                Preview_GoNextPic(pData, TRUE);
            }
            break;
        }
        case WM_UPDATECOMMANDSTATE:
        {
            PPREVIEW_DATA pData = Preview_GetData(g_hMainWnd);
            if (g_ImageId == lParam)
                Preview_SetEnabled(pData, LOWORD(wParam), HIWORD(wParam));
            break;
        }
        default:
        {
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
        }
    }

    return 0;
}

LONG
ImageView_Main(HWND hwnd, LPCWSTR szFileName)
{
    struct GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    WNDCLASSW WndClass;
    WCHAR szTitle[256];
    HWND hMainWnd;
    MSG msg;
    HACCEL hAccel;
    HRESULT hrCoInit;
    INITCOMMONCONTROLSEX Icc = { .dwSize = sizeof(Icc), .dwICC = ICC_WIN95_CLASSES };
    g_szFile[0] = UNICODE_NULL;

    InitCommonControlsEx(&Icc);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); // Give UI higher priority than background threads

    /* Initialize COM */
    hrCoInit = OleInitialize(NULL);
    if (FAILED(hrCoInit))
        DPRINT1("Warning, OleInitialize failed with code=%08X\n", (int)hrCoInit);

    if (!ImageView_LoadSettings())
        ImageView_ResetSettings();

    /* Initialize GDI+ */
    ZeroMemory(&gdiplusStartupInput, sizeof(gdiplusStartupInput));
    gdiplusStartupInput.GdiplusVersion = 1;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    /* Register window classes */
    ZeroMemory(&WndClass, sizeof(WndClass));
    WndClass.lpszClassName  = WC_PREVIEW;
    WndClass.lpfnWndProc    = PreviewWndProc;
    WndClass.hInstance      = g_hInstance;
    WndClass.style          = CS_HREDRAW | CS_VREDRAW;
    WndClass.hIcon          = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    WndClass.hCursor        = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    WndClass.hbrBackground  = GetStockBrush(NULL_BRUSH); /* less flicker */
    if (!RegisterClassW(&WndClass))
        return -1;
    WndClass.lpszClassName  = WC_ZOOM;
    WndClass.lpfnWndProc    = ZoomWndProc;
    WndClass.style          = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    WndClass.hbrBackground  = GetStockBrush(NULL_BRUSH); /* less flicker */
    if (!RegisterClassW(&WndClass))
        return -1;

    /* Create the main window */
    LoadStringW(g_hInstance, IDS_APPTITLE, szTitle, _countof(szTitle));
    hMainWnd = CreateWindowExW(WS_EX_WINDOWEDGE, WC_PREVIEW, szTitle,
                               WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS,
                               g_Settings.X, g_Settings.Y, g_Settings.Width, g_Settings.Height,
                               NULL, NULL, g_hInstance, (LPVOID)szFileName);

    /* Create accelerator table for keystrokes */
    hAccel = LoadAcceleratorsW(g_hInstance, MAKEINTRESOURCEW(IDR_ACCELERATOR));

    /* Show the main window now */
    if (g_Settings.Maximized)
        ShowWindow(hMainWnd, SW_SHOWMAXIMIZED);
    else
        ShowWindow(hMainWnd, SW_SHOWNORMAL);

    UpdateWindow(hMainWnd);

    /* Message Loop */
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        const HWND hwndFull = g_hwndFullscreen;
        /* Alt / F10 keytips and Ctrl+F1 of the ribbon, when there is one. */
        if (IsWindow(hMainWnd))
        {
            PPREVIEW_DATA pMain = Preview_GetData(hMainWnd);
            if (pMain && pMain->m_pRibbon &&
                LeewayRibbonPreTranslateMessage(VRB_RibbonFramework(pMain->m_pRibbon), &msg))
                continue;
        }
        if (IsWindowVisible(hwndFull) && TranslateAcceleratorW(hwndFull, hAccel, &msg))
            continue;
        if (TranslateAcceleratorW(hMainWnd, hAccel, &msg))
            continue;

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* Destroy accelerator table */
    DestroyAcceleratorTable(hAccel);

    ImageView_SaveSettings();

    GdiplusShutdown(gdiplusToken);

    /* Release COM resources */
    if (SUCCEEDED(hrCoInit))
        OleUninitialize();

    return 0;
}

VOID WINAPI
ImageView_FullscreenW(HWND hwnd, HINSTANCE hInst, LPCWSTR path, int nShow)
{
    ImageView_Main(hwnd, path);
}

VOID WINAPI
ImageView_Fullscreen(HWND hwnd, HINSTANCE hInst, LPCWSTR path, int nShow)
{
    ImageView_Main(hwnd, path);
}

VOID WINAPI
ImageView_FullscreenA(HWND hwnd, HINSTANCE hInst, LPCSTR path, int nShow)
{
    WCHAR szFile[MAX_PATH];

    if (MultiByteToWideChar(CP_ACP, 0, path, -1, szFile, _countof(szFile)))
    {
        ImageView_Main(hwnd, szFile);
    }
}

VOID WINAPI
ImageView_PrintTo(HWND hwnd, HINSTANCE hInst, LPCWSTR path, int nShow)
{
    DPRINT("ImageView_PrintTo() not implemented\n");
}

VOID WINAPI
ImageView_PrintToA(HWND hwnd, HINSTANCE hInst, LPCSTR path, int nShow)
{
    DPRINT("ImageView_PrintToA() not implemented\n");
}

VOID WINAPI
ImageView_PrintToW(HWND hwnd, HINSTANCE hInst, LPCWSTR path, int nShow)
{
    DPRINT("ImageView_PrintToW() not implemented\n");
}

BOOL WINAPI
DllMain(IN HINSTANCE hinstDLL,
        IN DWORD dwReason,
        IN LPVOID lpvReserved)
{
    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH:
            g_hInstance = hinstDLL;
            break;
    }

    return TRUE;
}
