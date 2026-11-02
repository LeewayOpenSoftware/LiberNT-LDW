/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Control Panel category view (home page, category pages, icon views)
 *
 * The data model is the documented one: categories 1-9 of System.ControlPanel.Category
 * read from the registered Control Panel items. The window is drawn with GDI and
 * wraps the classic list view, which stays the "Large icons / Small icons" view.
 */

#include "precomp.h"
#include "CCplCategoryView.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

#define CPL_NAMESPACE_KEY  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ControlPanel\\NameSpace"
#define CPL_ROOT_PATH      L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\::{21EC2020-3AEA-1069-A2DD-08002B30309D}"
#define CPL_MAX_ITEMS      128
#define CPL_MAX_HITS       256
#define CPL_MAX_DECO       192
#define CPL_MAX_TASKS      4

/* Category ids of System.ControlPanel.Category and the item that stands for the category */
struct CplCategory
{
    UINT Id;
    PCWSTR Name;
    PCWSTR IconClsid;
};

/* Order of the cards on the home page */
static const CplCategory g_Categories[] =
{
    { 5, L"System and Security",             L"{BB06C0E4-D293-4f75-8A90-CB05B6477EEE}" },
    { 3, L"Network and Internet",            L"{A3DD4F92-658A-410F-84FD-6FBBBEF2FFFE}" },
    { 2, L"Hardware and Sound",              L"{F2DDFC82-8F12-4CDD-B7DC-D4FE1425AA4D}" },
    { 8, L"Programs",                        L"{7b81be6a-ce2b-4676-a29e-eb907a5126c5}" },
    { 9, L"User Accounts",                   L"{60632754-c523-4b62-b45c-4172da012619}" },
    { 1, L"Appearance and Personalization",  L"{ED834ED6-4B5A-4bfe-8F11-A626DCB6A921}" },
    { 6, L"Clock and Region",                L"{E2E7934B-DCE5-43C4-9576-7FE4F75E7480}" },
    { 7, L"Ease of Access",                  L"{D555645E-D4F8-4c29-A827-D93C859C4F2A}" },
};

/* The links shown under a card: the items that are registered, in this order */
struct CplTask
{
    UINT Category;
    PCWSTR Clsid;
};

static const CplTask g_Tasks[] =
{
    { 5, L"{BB06C0E4-D293-4f75-8A90-CB05B6477EEE}" }, /* System */
    { 5, L"{025A5937-A6BE-4686-A844-36FE4BEC8B6D}" }, /* Power Options */
    { 5, L"{74246bfc-4c96-11d0-abef-0020af6b0b7a}" }, /* Device Manager */
    { 5, L"{9FE63AFD-59CF-4419-9775-ABCC3849F861}" }, /* Recovery */
    { 3, L"{8E908FC9-BECC-40f6-915B-F4CA0E70D03D}" }, /* Network and Sharing Center */
    { 3, L"{A3DD4F92-658A-410F-84FD-6FBBBEF2FFFE}" }, /* Internet Options */
    { 2, L"{A8A91A66-3A7D-4424-8D24-04E180695C7A}" }, /* Devices and Printers */
    { 2, L"{7A979262-40CE-46ff-AEEE-7884AC3B6136}" }, /* Add Hardware */
    { 2, L"{F2DDFC82-8F12-4CDD-B7DC-D4FE1425AA4D}" }, /* Sound */
    { 2, L"{6C8EEC18-8D75-41B2-A177-8831D59D2D50}" }, /* Mouse */
    { 8, L"{7b81be6a-ce2b-4676-a29e-eb907a5126c5}" }, /* Programs and Features */
    { 8, L"{17cd9488-1228-4b2f-88ce-4298e93e0966}" }, /* Default Programs */
    { 9, L"{60632754-c523-4b62-b45c-4172da012619}" }, /* User Accounts */
    { 1, L"{ED834ED6-4B5A-4bfe-8F11-A626DCB6A921}" }, /* Personalization */
    { 1, L"{C555438B-3C23-4769-A71F-B6D3D9B6053A}" }, /* Display */
    { 1, L"{0DF44EAA-FF21-4412-828E-260A8728E7F1}" }, /* Taskbar and Start Menu */
    { 6, L"{E2E7934B-DCE5-43C4-9576-7FE4F75E7480}" }, /* Date and Time */
    { 6, L"{62D8ED13-C9D0-4CE8-A914-47DD628FB1B0}" }, /* Region and Language */
    { 7, L"{D555645E-D4F8-4c29-A827-D93C859C4F2A}" }, /* Ease of Access Center */
};

struct CplItem
{
    WCHAR Clsid[40];
    WCHAR Name[96];
    WCHAR Tip[200];
    UINT Categories;    /* bit n set: item is in category n */
    HICON Icon;
    BOOL IconTried;
};

UINT CplGetStartupPage(VOID)
{
    DWORD value = CPL_STARTUP_CATEGORY, cb = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, CPL_VIEW_STATE_KEY, CPL_VALUE_STARTUP, RRF_RT_REG_DWORD, NULL, &value, &cb) != ERROR_SUCCESS)
        return CPL_STARTUP_CATEGORY;
    return value <= CPL_STARTUP_CLASSIC ? (UINT)value : CPL_STARTUP_CATEGORY;
}

BOOL CplGetSmallIcons(VOID)
{
    DWORD value = 0, cb = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, CPL_VIEW_STATE_KEY, CPL_VALUE_SMALLICON, RRF_RT_REG_DWORD, NULL, &value, &cb) != ERROR_SUCCESS)
        return FALSE;
    return value != 0;
}

VOID CplSetViewChoice(UINT startupPage, BOOL smallIcons)
{
    HKEY hKey;
    DWORD value;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, CPL_VIEW_STATE_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return;
    value = startupPage;
    RegSetValueExW(hKey, CPL_VALUE_STARTUP, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    value = smallIcons ? 1 : 0;
    RegSetValueExW(hKey, CPL_VALUE_SMALLICON, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    RegCloseKey(hKey);
}

static BOOL CplIsDark(VOID)
{
    DWORD value = 1, cb = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &value, &cb);
    return value == 0;
}

/* Text of a value of HKCR\CLSID\<clsid>[\subkey], resolving "@module,-id" strings */
static BOOL CplReadClsidText(PCWSTR clsid, PCWSTR subkey, PCWSTR value, PWSTR out, DWORD cch)
{
    WCHAR path[128], text[256];
    DWORD cb = sizeof(text), type = 0;

    if (subkey)
        StringCchPrintfW(path, _countof(path), L"CLSID\\%s\\%s", clsid, subkey);
    else
        StringCchPrintfW(path, _countof(path), L"CLSID\\%s", clsid);
    /* RRF_RT_ANY: this advapi32 rejects RRF_RT_REG_EXPAND_SZ without RRF_NOEXPAND, and a type of
     * REG_EXPAND_SZ comes back already expanded as REG_SZ */
    if (RegGetValueW(HKEY_CLASSES_ROOT, path, value, RRF_RT_ANY, &type, text, &cb) != ERROR_SUCCESS ||
        type != REG_SZ || !text[0])
        return FALSE;
    if (text[0] == L'@')
        return SUCCEEDED(SHLoadIndirectString(text, out, cch, NULL)) && out[0];
    return SUCCEEDED(StringCchCopyW(out, cch, text));
}

/* "1,8" -> bits 1 and 8; the documented unused ids move to the category that shows them */
static UINT CplParseCategories(PCWSTR text)
{
    UINT bits = 0;

    while (*text)
    {
        UINT id = 0;
        BOOL any = FALSE;
        while (*text >= L'0' && *text <= L'9')
        {
            id = id * 10 + (*text++ - L'0');
            any = TRUE;
        }
        if (any)
        {
            if (id == 4)
                id = 2;
            else if (id == 10)
                id = 5;
            else if (id == 11)
                id = 0;
            if (id <= 11)
                bits |= 1u << id;
        }
        else
        {
            text++;
        }
        while (*text == L',' || *text == L' ')
            text++;
    }
    return bits;
}

/* Opens a Control Panel item the way a double click does */
static VOID CplLaunchItem(HWND hwnd, PCWSTR clsid)
{
    WCHAR path[200];
    PIDLIST_ABSOLUTE pidl = NULL;
    SHELLEXECUTEINFOW sei = { sizeof(sei) };

    StringCchPrintfW(path, _countof(path), L"%s\\::%s", CPL_ROOT_PATH, clsid);
    if (FAILED(SHParseDisplayName(path, NULL, &pidl, 0, NULL)) || !pidl)
        return;
    sei.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = hwnd;
    sei.lpIDList = pidl;
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
    ILFree(pidl);
}

enum HitKind
{
    HK_VIEWBY,
    HK_CATEGORY,    /* arg: index in g_Categories */
    HK_ITEM,        /* arg: index in the item list */
    HK_HOME,
    HK_ALLITEMS
};

enum DecoKind
{
    DK_CARD,
    DK_TITLE,       /* page heading */
    DK_TEXT,        /* secondary text */
    DK_CATICON,     /* arg: index in g_Categories */
    DK_ITEMICON     /* arg: index in the item list */
};

enum LinkStyle
{
    LS_NORMAL,
    LS_CARDTITLE,
    LS_GRID
};

struct CplHit
{
    RECT Rect;
    int Kind;
    int Arg;
    int Style;
    WCHAR Text[100];
};

struct CplDeco
{
    RECT Rect;
    int Kind;
    int Arg;
    WCHAR Text[200];
};

class CCplCategoryView :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public CWindowImpl<CCplCategoryView, CWindow, CControlWinTraits>,
    public IShellView
{
private:
    enum Mode
    {
        MODE_HOME,
        MODE_PAGE,
        MODE_ICONS,     /* all items as a grid of large or small icons */
        MODE_CLASSIC    /* the classic list view, hosted */
    };

    CComPtr<IShellFolder> m_psf;
    CComPtr<IShellView> m_inner;
    CComPtr<IShellBrowser> m_psb;
    HWND m_hwndInner;
    FOLDERSETTINGS m_fs;
    Mode m_mode;
    int m_page;             /* index in g_Categories while in MODE_PAGE */
    BOOL m_smallIcons;
    BOOL m_dark;
    int m_dpi;
    int m_scroll;
    int m_contentHeight;
    int m_hot;
    int m_focus;
    BOOL m_tracking;
    HFONT m_fontNormal;
    HFONT m_fontLink;
    HFONT m_fontCard;
    HFONT m_fontCardLink;
    HFONT m_fontTitle;
    HICON m_catIcons[_countof(g_Categories)];
    CplItem *m_items;
    int m_itemCount;
    CplHit *m_hits;
    int m_hitCount;
    CplDeco *m_deco;
    int m_decoCount;

    int S(int value) { return MulDiv(value, m_dpi, 96); }
    BOOL IsClassic() { return m_mode == MODE_CLASSIC; }

    VOID LoadCatalog();
    VOID FreeCatalog();
    int FindItem(PCWSTR clsid);
    HICON LoadClsidIcon(PCWSTR clsid, int size);
    VOID CreateFonts();
    VOID DeleteFonts();
    int TextWidth(HDC hdc, HFONT font, PCWSTR text);
    VOID AddHit(RECT rc, int kind, int arg, int style, PCWSTR text);
    VOID AddDeco(RECT rc, int kind, int arg, PCWSTR text);
    VOID Layout();
    VOID LayoutViewBy(HDC hdc, int width);
    VOID LayoutHome(HDC hdc, int width);
    VOID LayoutPage(HDC hdc, int width);
    VOID LayoutGrid(HDC hdc, int width);
    int HeaderHeight() { return S(48); }
    VOID InnerRect(RECT *prc);
    VOID PlaceInner();
    HRESULT ShowInner();
    VOID HideInner();
    VOID ApplyIconMode();
    VOID SetView(UINT startupPage, BOOL smallIcons);
    VOID ShowViewMenu();
    VOID Activate(int hit);
    VOID SetHot(int hot);
    int HitTestPoint(POINT pt);
    VOID Paint(HDC hdc, const RECT *prcClient);
    VOID EnsureFocusVisible();

    /* While the icon view is active the folder-view interfaces belong to the wrapped list view */
    static HRESULT WINAPI ForwardInterface(void *pv, REFIID riid, LPVOID *ppv, DWORD_PTR dw)
    {
        CCplCategoryView *pThis = static_cast<CCplCategoryView *>(pv);
        UNREFERENCED_PARAMETER(dw);

        if (!pThis->m_inner || !pThis->m_hwndInner || !pThis->IsClassic() ||
            IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IShellView) || IsEqualIID(riid, IID_IOleWindow))
            return S_FALSE;
        if (FAILED(pThis->m_inner->QueryInterface(riid, ppv)))
            return S_FALSE;
        return S_OK;
    }

public:
    DECLARE_WND_CLASS_EX(L"CplCategoryView", CS_DBLCLKS, COLOR_WINDOW)

    CCplCategoryView();
    ~CCplCategoryView();

    HRESULT Initialize(IShellFolder *psf, IShellView *pInner);

    // IOleWindow
    STDMETHOD(GetWindow)(HWND *lphwnd) override;
    STDMETHOD(ContextSensitiveHelp)(BOOL fEnterMode) override;

    // IShellView
    STDMETHOD(TranslateAccelerator)(MSG *pmsg) override;
    STDMETHOD(EnableModeless)(BOOL fEnable) override;
    STDMETHOD(UIActivate)(UINT uState) override;
    STDMETHOD(Refresh)() override;
    STDMETHOD(CreateViewWindow)(IShellView *lpPrevView, LPCFOLDERSETTINGS lpfs, IShellBrowser *psb, RECT *prcView, HWND *phWnd) override;
    STDMETHOD(DestroyViewWindow)() override;
    STDMETHOD(GetCurrentInfo)(LPFOLDERSETTINGS lpfs) override;
    STDMETHOD(AddPropertySheetPages)(DWORD dwReserved, LPFNSVADDPROPSHEETPAGE lpfn, LPARAM lParam) override;
    STDMETHOD(SaveViewState)() override;
    STDMETHOD(SelectItem)(PCUITEMID_CHILD pidlItem, SVSIF uFlags) override;
    STDMETHOD(GetItemObject)(UINT uItem, REFIID riid, LPVOID *ppv) override;

    LRESULT OnCreate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled);
    LRESULT OnDestroy(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled);
    LRESULT OnSize(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled);
    LRESULT OnEraseBkgnd(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled);
    LRESULT OnPaint(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled);
    LRESULT OnMouseMove(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled);
    LRESULT OnMouseLeave(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled);
    LRESULT OnLButtonDown(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled);
    LRESULT OnLButtonUp(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled);
    LRESULT OnMouseWheel(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled);
    LRESULT OnKeyDown(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled);
    LRESULT OnGetDlgCode(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled);
    LRESULT OnSetCursor(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled);
    LRESULT OnFocus(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled);
    LRESULT OnSettingChange(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled);

    BEGIN_COM_MAP(CCplCategoryView)
    COM_INTERFACE_ENTRY_IID(IID_IShellView, IShellView)
    COM_INTERFACE_ENTRY_IID(IID_IOleWindow, IShellView)
    COM_INTERFACE_ENTRY_FUNC_BLIND(0, ForwardInterface)
    END_COM_MAP()

    BEGIN_MSG_MAP(CCplCategoryView)
    MESSAGE_HANDLER(WM_CREATE, OnCreate)
    MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    MESSAGE_HANDLER(WM_SIZE, OnSize)
    MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
    MESSAGE_HANDLER(WM_PAINT, OnPaint)
    MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
    MESSAGE_HANDLER(WM_MOUSELEAVE, OnMouseLeave)
    MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
    MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
    MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
    MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
    MESSAGE_HANDLER(WM_GETDLGCODE, OnGetDlgCode)
    MESSAGE_HANDLER(WM_SETCURSOR, OnSetCursor)
    MESSAGE_HANDLER(WM_SETFOCUS, OnFocus)
    MESSAGE_HANDLER(WM_KILLFOCUS, OnFocus)
    MESSAGE_HANDLER(WM_SETTINGCHANGE, OnSettingChange)
    MESSAGE_HANDLER(WM_THEMECHANGED, OnSettingChange)
    END_MSG_MAP()
};

CCplCategoryView::CCplCategoryView() :
    m_hwndInner(NULL),
    m_mode(MODE_HOME),
    m_page(0),
    m_smallIcons(FALSE),
    m_dark(FALSE),
    m_dpi(96),
    m_scroll(0),
    m_contentHeight(0),
    m_hot(-1),
    m_focus(-1),
    m_tracking(FALSE),
    m_fontNormal(NULL),
    m_fontLink(NULL),
    m_fontCard(NULL),
    m_fontCardLink(NULL),
    m_fontTitle(NULL),
    m_items(NULL),
    m_itemCount(0),
    m_hits(NULL),
    m_hitCount(0),
    m_deco(NULL),
    m_decoCount(0)
{
    ZeroMemory(&m_fs, sizeof(m_fs));
    ZeroMemory(m_catIcons, sizeof(m_catIcons));
}

CCplCategoryView::~CCplCategoryView()
{
    FreeCatalog();
    DeleteFonts();
    HeapFree(GetProcessHeap(), 0, m_hits);
    HeapFree(GetProcessHeap(), 0, m_deco);
}

HRESULT CCplCategoryView::Initialize(IShellFolder *psf, IShellView *pInner)
{
    m_psf = psf;
    m_inner = pInner;
    m_items = (CplItem *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, CPL_MAX_ITEMS * sizeof(CplItem));
    m_hits = (CplHit *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, CPL_MAX_HITS * sizeof(CplHit));
    m_deco = (CplDeco *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, CPL_MAX_DECO * sizeof(CplDeco));
    return (m_items && m_hits && m_deco) ? S_OK : E_OUTOFMEMORY;
}

/* The items are the namespace items registered under the Control Panel key */
VOID CCplCategoryView::LoadCatalog()
{
    HKEY hKey;
    WCHAR clsid[40];

    FreeCatalog();
    LONG openResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, CPL_NAMESPACE_KEY, 0, KEY_READ, &hKey);
    if (openResult != ERROR_SUCCESS)
    {
        ERR("Control Panel category view: cannot open the NameSpace key (error %ld)\n", openResult);
        return;
    }
    for (DWORD index = 0; m_itemCount < CPL_MAX_ITEMS; index++)
    {
        DWORD cch = _countof(clsid);
        CplItem *item = &m_items[m_itemCount];
        WCHAR categories[64];

        if (RegEnumKeyExW(hKey, index, clsid, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (clsid[0] != L'{')
            continue;
        /* Network Connections is the adapter list, reached from the Network and Sharing Center and
         * from ncpa.cpl: the all-items view does not list it next to the Center */
        if (!StrCmpIW(clsid, L"{7007ACC7-3202-11D1-AAD2-00805FC1270E}"))
            continue;
        StringCchCopyW(item->Clsid, _countof(item->Clsid), clsid);
        if (!CplReadClsidText(clsid, NULL, L"LocalizedString", item->Name, _countof(item->Name)) &&
            !CplReadClsidText(clsid, NULL, NULL, item->Name, _countof(item->Name)))
        {
            ERR("Control Panel category view: no name for %S\n", clsid);
            continue;
        }
        CplReadClsidText(clsid, NULL, L"InfoTip", item->Tip, _countof(item->Tip));
        categories[0] = UNICODE_NULL;
        CplReadClsidText(clsid, NULL, L"System.ControlPanel.Category", categories, _countof(categories));
        item->Categories = CplParseCategories(categories);
        if (!item->Categories)
            item->Categories = 1u << 0;
        m_itemCount++;
    }
    RegCloseKey(hKey);
    ERR("Control Panel category view: %d items loaded\n", m_itemCount);

    /* Sort by name */
    for (int i = 1; i < m_itemCount; i++)
    {
        CplItem tmp = m_items[i];
        int j = i - 1;
        while (j >= 0 && StrCmpLogicalW(m_items[j].Name, tmp.Name) > 0)
        {
            m_items[j + 1] = m_items[j];
            j--;
        }
        m_items[j + 1] = tmp;
    }
}

VOID CCplCategoryView::FreeCatalog()
{
    for (int i = 0; i < m_itemCount; i++)
    {
        if (m_items[i].Icon)
            DestroyIcon(m_items[i].Icon);
    }
    m_itemCount = 0;
    for (UINT i = 0; i < _countof(m_catIcons); i++)
    {
        if (m_catIcons[i])
            DestroyIcon(m_catIcons[i]);
        m_catIcons[i] = NULL;
    }
}

int CCplCategoryView::FindItem(PCWSTR clsid)
{
    for (int i = 0; i < m_itemCount; i++)
    {
        if (!StrCmpIW(m_items[i].Clsid, clsid))
            return i;
    }
    return -1;
}

HICON CCplCategoryView::LoadClsidIcon(PCWSTR clsid, int size)
{
    WCHAR path[128], spec[MAX_PATH + 16];
    DWORD cb = sizeof(spec), type = 0;
    HICON hLarge = NULL, hSmall = NULL;
    PWSTR comma;
    int index;

    StringCchPrintfW(path, _countof(path), L"CLSID\\%s\\DefaultIcon", clsid);
    LONG status = RegGetValueW(HKEY_CLASSES_ROOT, path, NULL, RRF_RT_ANY, &type, spec, &cb);
    if (status == ERROR_SUCCESS && type != REG_SZ)
        status = ERROR_UNSUPPORTED_TYPE;
    if (status != ERROR_SUCCESS)
    {
        ERR("Control Panel category view: no DefaultIcon for %S (error %ld)\n", clsid, status);
        return NULL;
    }
    comma = StrRChrW(spec, NULL, L',');
    index = 0;
    if (comma)
    {
        *comma = UNICODE_NULL;
        index = StrToIntW(comma + 1);
    }
    HRESULT hrIcon = SHDefExtractIconW(spec, index, 0, &hLarge, &hSmall, MAKELONG(size, size));
    if (FAILED(hrIcon))
    {
        ERR("Control Panel category view: cannot extract %S,%d (hr %lx)\n", spec, index, hrIcon);
        return NULL;
    }
    if (hSmall)
        DestroyIcon(hSmall);
    return hLarge;
}

VOID CCplCategoryView::CreateFonts()
{
    LOGFONTW base, lf;

    DeleteFonts();
    if (!SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(base), &base, 0))
        return;
    lf = base;
    lf.lfQuality = CLEARTYPE_QUALITY;
    m_fontNormal = CreateFontIndirectW(&lf);
    lf.lfUnderline = TRUE;
    m_fontLink = CreateFontIndirectW(&lf);
    lf = base;
    lf.lfHeight = MulDiv(lf.lfHeight, 5, 4);
    m_fontCard = CreateFontIndirectW(&lf);
    lf.lfUnderline = TRUE;
    m_fontCardLink = CreateFontIndirectW(&lf);
    lf = base;
    lf.lfHeight = MulDiv(lf.lfHeight, 9, 4);
    m_fontTitle = CreateFontIndirectW(&lf);
}

VOID CCplCategoryView::DeleteFonts()
{
    HFONT *fonts[] = { &m_fontNormal, &m_fontLink, &m_fontCard, &m_fontCardLink, &m_fontTitle };

    for (UINT i = 0; i < _countof(fonts); i++)
    {
        if (*fonts[i])
            DeleteObject(*fonts[i]);
        *fonts[i] = NULL;
    }
}

int CCplCategoryView::TextWidth(HDC hdc, HFONT font, PCWSTR text)
{
    SIZE size = { 0, 0 };
    HGDIOBJ old = SelectObject(hdc, font);
    GetTextExtentPoint32W(hdc, text, lstrlenW(text), &size);
    SelectObject(hdc, old);
    return size.cx;
}

VOID CCplCategoryView::AddHit(RECT rc, int kind, int arg, int style, PCWSTR text)
{
    if (m_hitCount >= CPL_MAX_HITS)
        return;
    CplHit *hit = &m_hits[m_hitCount++];
    hit->Rect = rc;
    hit->Kind = kind;
    hit->Arg = arg;
    hit->Style = style;
    StringCchCopyW(hit->Text, _countof(hit->Text), text ? text : L"");
}

VOID CCplCategoryView::AddDeco(RECT rc, int kind, int arg, PCWSTR text)
{
    if (m_decoCount >= CPL_MAX_DECO)
        return;
    CplDeco *deco = &m_deco[m_decoCount++];
    deco->Rect = rc;
    deco->Kind = kind;
    deco->Arg = arg;
    StringCchCopyW(deco->Text, _countof(deco->Text), text ? text : L"");
}

/* "View by: Category" in the upper right corner, shown on every page */
VOID CCplCategoryView::LayoutViewBy(HDC hdc, int width)
{
    WCHAR text[64];
    PCWSTR current = IsClassic() ? L"Classic list" : (m_mode == MODE_ICONS ? (m_smallIcons ? L"Small icons" : L"Large icons") : L"Category");
    int w;
    RECT rc;

    StringCchPrintfW(text, _countof(text), L"%s \x25BE", current);
    w = TextWidth(hdc, m_fontNormal, text);
    SetRect(&rc, width - S(24) - w, S(14), width - S(24), S(14) + S(22));
    AddHit(rc, HK_VIEWBY, 0, LS_NORMAL, text);
    w = TextWidth(hdc, m_fontNormal, L"View by:  ");
    SetRect(&rc, width - S(24) - TextWidth(hdc, m_fontNormal, text) - w, S(14), width - S(24) - TextWidth(hdc, m_fontNormal, text), S(14) + S(22));
    AddDeco(rc, DK_TEXT, 1, L"View by:");
}

VOID CCplCategoryView::LayoutHome(HDC hdc, int width)
{
    int margin = S(32), gap = S(22), top = HeaderHeight() + S(12);
    int columns = width >= S(760) ? 2 : 1;
    int colW = (width - 2 * margin) / columns;
    int perColumn = (_countof(g_Categories) + columns - 1) / columns;
    int lineH = S(20);
    int colY[2] = { top, top };
    RECT rc;
    WCHAR title[] = L"Adjust your computer's settings";

    SetRect(&rc, margin, S(10), width / 2, S(10) + S(32));
    AddDeco(rc, DK_TITLE, 0, title);

    for (UINT c = 0; c < _countof(g_Categories); c++)
    {
        int col = (int)c / perColumn;
        int x = margin + col * colW, y = colY[col];
        int shown = 0, tw, textH;

        SetRect(&rc, x, y, x + S(48), y + S(48));
        AddDeco(rc, DK_CATICON, c, NULL);

        tw = TextWidth(hdc, m_fontCard, g_Categories[c].Name);
        SetRect(&rc, x + S(64), y, x + S(64) + min(tw, colW - S(80)), y + S(26));
        AddHit(rc, HK_CATEGORY, c, LS_CARDTITLE, g_Categories[c].Name);

        for (UINT t = 0; t < _countof(g_Tasks) && shown < CPL_MAX_TASKS; t++)
        {
            int item;

            if (g_Tasks[t].Category != g_Categories[c].Id)
                continue;
            item = FindItem(g_Tasks[t].Clsid);
            if (item < 0)
                continue;
            tw = TextWidth(hdc, m_fontNormal, m_items[item].Name);
            SetRect(&rc, x + S(64), y + S(28) + shown * lineH, x + S(64) + min(tw, colW - S(80)),
                    y + S(28) + (shown + 1) * lineH);
            AddHit(rc, HK_ITEM, item, LS_NORMAL, m_items[item].Name);
            shown++;
        }
        textH = S(28) + shown * lineH;
        colY[col] = y + max(S(48), textH) + gap;
    }

    int y = max(colY[0], colY[1]);
    int tw = TextWidth(hdc, m_fontNormal, L"All Control Panel Items");
    SetRect(&rc, margin, y, margin + tw, y + S(22));
    AddHit(rc, HK_ALLITEMS, 0, LS_NORMAL, L"All Control Panel Items");
    m_contentHeight = y + S(22) + margin;
}

/* All items: a grid of icon and name, sorted by name, five columns on a wide window */
VOID CCplCategoryView::LayoutGrid(HDC hdc, int width)
{
    int margin = S(16), icon = m_smallIcons ? S(16) : S(32);
    int cellW = m_smallIcons ? S(220) : S(196), cellH = m_smallIcons ? S(28) : S(44);
    int columns = max(1, (width - 2 * margin) / cellW);
    int top = HeaderHeight() + S(8);
    RECT rc;
    WCHAR title[] = L"Adjust your computer's settings";

    SetRect(&rc, S(24), S(10), width / 2, S(10) + S(32));
    AddDeco(rc, DK_TITLE, 0, title);
    for (int i = 0; i < m_itemCount; i++)
    {
        int x = margin + (i % columns) * cellW, y = top + (i / columns) * cellH;

        SetRect(&rc, x + S(4), y + (cellH - icon) / 2, x + S(4) + icon, y + (cellH - icon) / 2 + icon);
        AddDeco(rc, DK_ITEMICON, i, NULL);
        SetRect(&rc, x + S(4) + icon + S(10), y + S(2), x + cellW - S(6), y + cellH - S(2));
        AddHit(rc, HK_ITEM, i, LS_GRID, m_items[i].Name);
    }
    m_contentHeight = top + ((m_itemCount + columns - 1) / columns) * cellH + margin;
}

VOID CCplCategoryView::LayoutPage(HDC hdc, int width)
{
    int margin = S(24), rowH = S(60), y;
    const CplCategory *category = &g_Categories[m_page];
    RECT rc;
    int tw = TextWidth(hdc, m_fontNormal, L"Control Panel Home");

    SetRect(&rc, margin, S(10), margin + tw, S(10) + S(22));
    AddHit(rc, HK_HOME, 0, LS_NORMAL, L"Control Panel Home");
    SetRect(&rc, margin, S(36), width - margin, S(36) + S(32));
    AddDeco(rc, DK_TITLE, 0, category->Name);

    y = HeaderHeight() + S(36);
    for (int i = 0; i < m_itemCount; i++)
    {
        if (!(m_items[i].Categories & (1u << category->Id)))
            continue;
        SetRect(&rc, margin, y + S(8), margin + S(32), y + S(8) + S(32));
        AddDeco(rc, DK_ITEMICON, i, NULL);
        tw = TextWidth(hdc, m_fontCard, m_items[i].Name);
        SetRect(&rc, margin + S(48), y + S(4), margin + S(48) + min(tw, width - 2 * margin - S(48)), y + S(4) + S(26));
        AddHit(rc, HK_ITEM, i, LS_CARDTITLE, m_items[i].Name);
        if (m_items[i].Tip[0])
        {
            SetRect(&rc, margin + S(48), y + S(30), width - margin, y + S(30) + S(22));
            AddDeco(rc, DK_TEXT, 0, m_items[i].Tip);
        }
        y += rowH;
    }
    tw = TextWidth(hdc, m_fontNormal, L"All Control Panel Items");
    SetRect(&rc, margin, y + S(8), margin + tw, y + S(8) + S(22));
    AddHit(rc, HK_ALLITEMS, 0, LS_NORMAL, L"All Control Panel Items");
    m_contentHeight = y + S(8) + S(22) + margin;
}

VOID CCplCategoryView::Layout()
{
    RECT rc;
    HDC hdc;
    int width;

    m_hitCount = 0;
    m_decoCount = 0;
    m_contentHeight = 0;
    if (!m_hWnd)
        return;
    GetClientRect(&rc);
    width = rc.right;
    hdc = GetDC();
    if (hdc)
    {
        m_dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        if (m_dpi <= 0)
            m_dpi = 96;
        LayoutViewBy(hdc, width);
        if (IsClassic())
        {
            WCHAR title[] = L"All Control Panel Items";
            SetRect(&rc, S(24), S(10), width / 2, S(10) + S(32));
            AddDeco(rc, DK_TITLE, 0, title);
        }
        else if (m_mode == MODE_ICONS)
        {
            LayoutGrid(hdc, width);
        }
        else if (m_mode == MODE_PAGE)
        {
            LayoutPage(hdc, width);
        }
        else
        {
            LayoutHome(hdc, width);
        }
        ReleaseDC(hdc);
    }
    GetClientRect(&rc);
    m_scroll = max(0, min(m_scroll, m_contentHeight - rc.bottom));
    if (m_hot >= m_hitCount)
        m_hot = -1;
    if (m_focus >= m_hitCount)
        m_focus = -1;
}

VOID CCplCategoryView::InnerRect(RECT *prc)
{
    GetWindowRect(prc);
    ::MapWindowPoints(NULL, GetParent(), (LPPOINT)prc, 2);
    prc->top += HeaderHeight();
}

VOID CCplCategoryView::PlaceInner()
{
    RECT rc;

    if (!m_hwndInner || !IsClassic())
        return;
    InnerRect(&rc);
    ::SetWindowPos(m_hwndInner, HWND_TOP, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

VOID CCplCategoryView::ApplyIconMode()
{
    CComPtr<IFolderView> pfv;

    if (m_inner && SUCCEEDED(m_inner->QueryInterface(IID_PPV_ARG(IFolderView, &pfv))))
        pfv->SetCurrentViewMode(m_smallIcons ? FVM_SMALLICON : FVM_ICON);
}

HRESULT CCplCategoryView::ShowInner()
{
    if (!m_inner)
        return E_FAIL;
    if (!m_hwndInner)
    {
        RECT rc;
        FOLDERSETTINGS fs = m_fs;

        InnerRect(&rc);
        fs.ViewMode = m_smallIcons ? FVM_SMALLICON : FVM_ICON;
        HRESULT hr = m_inner->CreateViewWindow(NULL, &fs, m_psb, &rc, &m_hwndInner);
        if (FAILED_UNEXPECTEDLY(hr))
        {
            m_hwndInner = NULL;
            return hr;
        }
    }
    else
    {
        ApplyIconMode();
    }
    PlaceInner();
    m_inner->UIActivate(SVUIA_ACTIVATE_FOCUS);
    return S_OK;
}

VOID CCplCategoryView::HideInner()
{
    if (m_inner && m_hwndInner)
    {
        m_inner->UIActivate(SVUIA_DEACTIVATE);
        ::ShowWindow(m_hwndInner, SW_HIDE);
    }
    if (m_hWnd)
        SetFocus();
}

VOID CCplCategoryView::SetView(UINT startupPage, BOOL smallIcons)
{
    BOOL wasClassic = IsClassic();

    m_smallIcons = smallIcons;
    CplSetViewChoice(startupPage, smallIcons);
    if (startupPage == CPL_STARTUP_CLASSIC)
    {
        m_mode = MODE_CLASSIC;
        if (FAILED(ShowInner()))
        {
            m_mode = MODE_HOME;
            CplSetViewChoice(CPL_STARTUP_CATEGORY, smallIcons);
        }
    }
    else
    {
        m_mode = startupPage == CPL_STARTUP_ICONS ? MODE_ICONS : MODE_HOME;
        m_page = 0;
        if (wasClassic)
            HideInner();
    }
    m_scroll = 0;
    m_hot = m_focus = -1;
    Layout();
    InvalidateRect(NULL, TRUE);
}

VOID CCplCategoryView::ShowViewMenu()
{
    HMENU hMenu = CreatePopupMenu();
    RECT rc;
    POINT pt;
    UINT cmd;
    BOOL grid = (m_mode == MODE_ICONS);

    if (!hMenu || m_hot < 0)
        return;
    AppendMenuW(hMenu, MF_STRING | (!grid && !IsClassic() ? MF_CHECKED : 0), 1, L"Category");
    AppendMenuW(hMenu, MF_STRING | (grid && !m_smallIcons ? MF_CHECKED : 0), 2, L"Large icons");
    AppendMenuW(hMenu, MF_STRING | (grid && m_smallIcons ? MF_CHECKED : 0), 3, L"Small icons");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING | (IsClassic() ? MF_CHECKED : 0), 4, L"Classic list");
    rc = m_hits[m_hot].Rect;
    pt.x = rc.left;
    pt.y = rc.bottom;
    ClientToScreen(&pt);
    cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, m_hWnd, NULL);
    DestroyMenu(hMenu);
    if (cmd == 1)
        SetView(CPL_STARTUP_CATEGORY, m_smallIcons);
    else if (cmd == 2)
        SetView(CPL_STARTUP_ICONS, FALSE);
    else if (cmd == 3)
        SetView(CPL_STARTUP_ICONS, TRUE);
    else if (cmd == 4)
        SetView(CPL_STARTUP_CLASSIC, m_smallIcons);
}

VOID CCplCategoryView::Activate(int hit)
{
    if (hit < 0 || hit >= m_hitCount)
        return;
    switch (m_hits[hit].Kind)
    {
        case HK_VIEWBY:
            m_hot = hit;
            ShowViewMenu();
            break;
        case HK_CATEGORY:
            m_mode = MODE_PAGE;
            m_page = m_hits[hit].Arg;
            m_scroll = 0;
            m_hot = m_focus = -1;
            Layout();
            InvalidateRect(NULL, TRUE);
            break;
        case HK_HOME:
            SetView(CPL_STARTUP_CATEGORY, m_smallIcons);
            break;
        case HK_ALLITEMS:
            SetView(CPL_STARTUP_ICONS, m_smallIcons);
            break;
        case HK_ITEM:
            CplLaunchItem(m_hWnd, m_items[m_hits[hit].Arg].Clsid);
            break;
    }
}

VOID CCplCategoryView::SetHot(int hot)
{
    if (hot == m_hot)
        return;
    m_hot = hot;
    InvalidateRect(NULL, FALSE);
}

int CCplCategoryView::HitTestPoint(POINT pt)
{
    pt.y += m_scroll;
    for (int i = 0; i < m_hitCount; i++)
    {
        if (PtInRect(&m_hits[i].Rect, pt))
            return i;
    }
    return -1;
}

VOID CCplCategoryView::EnsureFocusVisible()
{
    RECT client;

    if (m_focus < 0 || m_focus >= m_hitCount)
        return;
    GetClientRect(&client);
    if (m_hits[m_focus].Rect.top - m_scroll < 0)
        m_scroll = m_hits[m_focus].Rect.top - S(16);
    else if (m_hits[m_focus].Rect.bottom - m_scroll > client.bottom)
        m_scroll = m_hits[m_focus].Rect.bottom - client.bottom + S(16);
    m_scroll = max(0, min(m_scroll, m_contentHeight - client.bottom));
}

VOID CCplCategoryView::Paint(HDC hdc, const RECT *prcClient)
{
    COLORREF back = m_dark ? RGB(32, 32, 32) : RGB(255, 255, 255);
    COLORREF heading = m_dark ? RGB(140, 190, 255) : RGB(0, 51, 153);
    COLORREF green = m_dark ? RGB(110, 205, 150) : RGB(30, 115, 75);
    COLORREF second = m_dark ? RGB(180, 180, 180) : RGB(90, 90, 90);
    COLORREF link = m_dark ? RGB(102, 178, 255) : RGB(0, 102, 204);
    HBRUSH brush = CreateSolidBrush(back);

    FillRect(hdc, prcClient, brush);
    DeleteObject(brush);
    SetBkMode(hdc, TRANSPARENT);

    for (int i = 0; i < m_decoCount; i++)
    {
        CplDeco *deco = &m_deco[i];
        RECT rc = deco->Rect;

        if (!(deco->Kind == DK_TEXT && deco->Arg == 1))
            OffsetRect(&rc, 0, -m_scroll);
        if (rc.bottom < 0 || rc.top > prcClient->bottom)
            continue;
        switch (deco->Kind)
        {
            case DK_CARD:
                break;
            case DK_TITLE:
                SelectObject(hdc, m_fontTitle);
                SetTextColor(hdc, heading);
                DrawTextW(hdc, deco->Text, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                break;
            case DK_TEXT:
                SelectObject(hdc, m_fontNormal);
                SetTextColor(hdc, second);
                DrawTextW(hdc, deco->Text, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                break;
            case DK_CATICON:
            {
                HICON *icon = &m_catIcons[deco->Arg];
                if (!*icon)
                {
                    int item = FindItem(g_Categories[deco->Arg].IconClsid);
                    *icon = LoadClsidIcon(g_Categories[deco->Arg].IconClsid, rc.right - rc.left);
                    UNREFERENCED_PARAMETER(item);
                }
                if (*icon)
                    DrawIconEx(hdc, rc.left, rc.top, *icon, rc.right - rc.left, rc.bottom - rc.top, 0, NULL, DI_NORMAL);
                break;
            }
            case DK_ITEMICON:
            {
                CplItem *item = &m_items[deco->Arg];
                if (!item->Icon && !item->IconTried)
                {
                    item->IconTried = TRUE;
                    item->Icon = LoadClsidIcon(item->Clsid, rc.right - rc.left);
                }
                if (item->Icon)
                    DrawIconEx(hdc, rc.left, rc.top, item->Icon, rc.right - rc.left, rc.bottom - rc.top, 0, NULL, DI_NORMAL);
                break;
            }
        }
    }

    for (int i = 0; i < m_hitCount; i++)
    {
        CplHit *hit = &m_hits[i];
        RECT rc = hit->Rect;
        BOOL hot = (i == m_hot);
        HFONT font;

        OffsetRect(&rc, 0, -m_scroll);
        if (hit->Kind == HK_VIEWBY)
            OffsetRect(&rc, 0, m_scroll);
        if (i == m_hot && hit->Style == LS_GRID)
        {
            HBRUSH hb = CreateSolidBrush(m_dark ? RGB(55, 55, 55) : RGB(229, 243, 255));
            RECT cell = rc;
            InflateRect(&cell, S(4), S(1));
            cell.left -= S(46);
            FillRect(hdc, &cell, hb);
            DeleteObject(hb);
        }
        if (rc.bottom < 0 || rc.top > prcClient->bottom)
            continue;
        if (hit->Style == LS_CARDTITLE)
            font = hot ? m_fontCardLink : m_fontCard;
        else if (hit->Style == LS_GRID)
            font = hot ? m_fontLink : m_fontNormal;
        else
            font = (hot && hit->Kind != HK_VIEWBY) ? m_fontLink : m_fontNormal;
        SelectObject(hdc, font);
        SetTextColor(hdc, (hit->Style == LS_CARDTITLE || hit->Style == LS_GRID) ? green : link);
        if (hit->Style == LS_GRID)
            DrawTextW(hdc, hit->Text, -1, &rc, DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
        else
            DrawTextW(hdc, hit->Text, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (i == m_focus && GetFocus() == m_hWnd)
        {
            RECT focus = rc;
            InflateRect(&focus, 2, 1);
            DrawFocusRect(hdc, &focus);
        }
    }
}

LRESULT CCplCategoryView::OnCreate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled)
{
    HDC hdc = GetDC();

    if (hdc)
    {
        m_dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(hdc);
    }
    if (m_dpi <= 0)
        m_dpi = 96;
    m_dark = CplIsDark();
    CreateFonts();
    return 0;
}

LRESULT CCplCategoryView::OnDestroy(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled)
{
    DeleteFonts();
    return 0;
}

LRESULT CCplCategoryView::OnSize(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled)
{
    Layout();
    PlaceInner();
    InvalidateRect(NULL, TRUE);
    return 0;
}

LRESULT CCplCategoryView::OnEraseBkgnd(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled)
{
    return 1;
}

LRESULT CCplCategoryView::OnPaint(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled)
{
    PAINTSTRUCT ps;
    RECT client;
    HDC hdc = BeginPaint(&ps);

    if (hdc)
    {
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bitmap = NULL;

        GetClientRect(&client);
        if (mem)
            bitmap = CreateCompatibleBitmap(hdc, client.right, client.bottom);
        if (mem && bitmap)
        {
            HGDIOBJ old = SelectObject(mem, bitmap);
            Paint(mem, &client);
            BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
        }
        else
        {
            Paint(hdc, &client);
        }
        if (bitmap)
            DeleteObject(bitmap);
        if (mem)
            DeleteDC(mem);
    }
    EndPaint(&ps);
    return 0;
}

LRESULT CCplCategoryView::OnMouseMove(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled)
{
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

    if (!m_tracking)
    {
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, m_hWnd, 0 };
        m_tracking = TrackMouseEvent(&track);
    }
    SetHot(HitTestPoint(pt));
    return 0;
}

LRESULT CCplCategoryView::OnMouseLeave(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled)
{
    m_tracking = FALSE;
    SetHot(-1);
    return 0;
}

LRESULT CCplCategoryView::OnLButtonDown(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled)
{
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    int hit = HitTestPoint(pt);

    SetFocus();
    if (hit >= 0)
    {
        m_focus = hit;
        InvalidateRect(NULL, FALSE);
    }
    return 0;
}

LRESULT CCplCategoryView::OnLButtonUp(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled)
{
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    int hit = HitTestPoint(pt);

    if (hit >= 0 && hit == m_focus)
        Activate(hit);
    return 0;
}

LRESULT CCplCategoryView::OnMouseWheel(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled)
{
    RECT client;
    int delta = GET_WHEEL_DELTA_WPARAM(wParam);

    GetClientRect(&client);
    m_scroll -= MulDiv(delta, S(48), WHEEL_DELTA);
    m_scroll = max(0, min(m_scroll, m_contentHeight - client.bottom));
    InvalidateRect(NULL, FALSE);
    return 0;
}

LRESULT CCplCategoryView::OnKeyDown(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled)
{
    int next = m_focus;
    BOOL shift = GetKeyState(VK_SHIFT) < 0;

    if (!m_hitCount)
        return 0;
    switch (wParam)
    {
        case VK_TAB:
            next = shift ? m_focus - 1 : m_focus + 1;
            break;
        case VK_DOWN:
        case VK_RIGHT:
            next = m_focus + 1;
            break;
        case VK_UP:
        case VK_LEFT:
            next = m_focus - 1;
            break;
        case VK_HOME:
            next = 0;
            break;
        case VK_END:
            next = m_hitCount - 1;
            break;
        case VK_RETURN:
        case VK_SPACE:
            Activate(m_focus);
            return 0;
        case VK_BACK:
            if (m_mode == MODE_PAGE)
                SetView(CPL_STARTUP_CATEGORY, m_smallIcons);
            return 0;
        default:
            return 0;
    }
    if (next < 0)
        next = m_hitCount - 1;
    if (next >= m_hitCount)
        next = 0;
    m_focus = next;
    EnsureFocusVisible();
    InvalidateRect(NULL, FALSE);
    return 0;
}

LRESULT CCplCategoryView::OnGetDlgCode(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled)
{
    return DLGC_WANTARROWS | DLGC_WANTTAB | DLGC_WANTCHARS;
}

LRESULT CCplCategoryView::OnSetCursor(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled)
{
    if (LOWORD(lParam) == HTCLIENT && m_hot >= 0)
    {
        SetCursor(LoadCursorW(NULL, IDC_HAND));
        return TRUE;
    }
    bHandled = FALSE;
    return 0;
}

LRESULT CCplCategoryView::OnFocus(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled)
{
    if (uMsg == WM_SETFOCUS && m_focus < 0 && m_hitCount > 1)
        m_focus = 1;
    InvalidateRect(NULL, FALSE);
    return 0;
}

LRESULT CCplCategoryView::OnSettingChange(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled)
{
    m_dark = CplIsDark();
    CreateFonts();
    Layout();
    InvalidateRect(NULL, TRUE);
    return 0;
}

STDMETHODIMP CCplCategoryView::GetWindow(HWND *lphwnd)
{
    if (!lphwnd)
        return E_POINTER;
    *lphwnd = m_hWnd;
    return S_OK;
}

STDMETHODIMP CCplCategoryView::ContextSensitiveHelp(BOOL fEnterMode)
{
    return E_NOTIMPL;
}

STDMETHODIMP CCplCategoryView::TranslateAccelerator(MSG *pmsg)
{
    if (IsClassic() && m_inner)
        return m_inner->TranslateAccelerator(pmsg);
    return S_FALSE;
}

STDMETHODIMP CCplCategoryView::EnableModeless(BOOL fEnable)
{
    if (IsClassic() && m_inner)
        return m_inner->EnableModeless(fEnable);
    return S_OK;
}

STDMETHODIMP CCplCategoryView::UIActivate(UINT uState)
{
    if (IsClassic() && m_inner)
        return m_inner->UIActivate(uState);
    if (uState != SVUIA_DEACTIVATE && m_hWnd)
        SetFocus();
    return S_OK;
}

STDMETHODIMP CCplCategoryView::Refresh()
{
    if (IsClassic() && m_inner)
        return m_inner->Refresh();
    LoadCatalog();
    Layout();
    if (m_hWnd)
        InvalidateRect(NULL, TRUE);
    return S_OK;
}

STDMETHODIMP CCplCategoryView::CreateViewWindow(IShellView *lpPrevView, LPCFOLDERSETTINGS lpfs, IShellBrowser *psb,
                                                RECT *prcView, HWND *phWnd)
{
    HWND hParent = NULL;
    RECT rc = { 0, 0, 0, 0 };

    if (m_hWnd)
        return E_UNEXPECTED;
    if (!lpfs || !psb || !phWnd)
        return E_INVALIDARG;
    *phWnd = NULL;
    if (FAILED_UNEXPECTEDLY(psb->GetWindow(&hParent)) || !hParent)
        return E_FAIL;
    if (prcView)
        rc = *prcView;
    m_fs = *lpfs;
    m_psb = psb;
    m_smallIcons = CplGetSmallIcons();
    m_mode = CplGetStartupPage() == CPL_STARTUP_CLASSIC ? MODE_CLASSIC : (CplGetStartupPage() == CPL_STARTUP_ICONS ? MODE_ICONS : MODE_HOME);
    LoadCatalog();
    Create(hParent, rc, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP);
    if (!m_hWnd)
        return E_FAIL;
    Layout();
    if (IsClassic() && FAILED(ShowInner()))
    {
        m_mode = MODE_HOME;
        Layout();
    }
    *phWnd = m_hWnd;
    return S_OK;
}

STDMETHODIMP CCplCategoryView::DestroyViewWindow()
{
    if (m_inner && m_hwndInner)
        m_inner->DestroyViewWindow();
    m_hwndInner = NULL;
    if (m_hWnd)
        DestroyWindow();
    m_psb.Release();
    return S_OK;
}

STDMETHODIMP CCplCategoryView::GetCurrentInfo(LPFOLDERSETTINGS lpfs)
{
    if (!lpfs)
        return E_POINTER;
    *lpfs = m_fs;
    return S_OK;
}

STDMETHODIMP CCplCategoryView::AddPropertySheetPages(DWORD dwReserved, LPFNSVADDPROPSHEETPAGE lpfn, LPARAM lParam)
{
    return E_NOTIMPL;
}

STDMETHODIMP CCplCategoryView::SaveViewState()
{
    if (IsClassic() && m_inner)
        return m_inner->SaveViewState();
    return S_OK;
}

STDMETHODIMP CCplCategoryView::SelectItem(PCUITEMID_CHILD pidlItem, SVSIF uFlags)
{
    if (IsClassic() && m_inner)
        return m_inner->SelectItem(pidlItem, uFlags);
    return E_NOTIMPL;
}

STDMETHODIMP CCplCategoryView::GetItemObject(UINT uItem, REFIID riid, LPVOID *ppv)
{
    if (IsClassic() && m_inner)
        return m_inner->GetItemObject(uItem, riid, ppv);
    if (ppv)
        *ppv = NULL;
    return E_NOINTERFACE;
}

HRESULT CplCreateCategoryView(IShellFolder *psf, IShellView *pInner, IShellView **ppView)
{
    CComPtr<CCplCategoryView> pView;
    HRESULT hr;

    hr = ShellObjectCreator(pView);
    if (FAILED_UNEXPECTEDLY(hr))
        return hr;
    hr = pView->Initialize(psf, pInner);
    if (FAILED_UNEXPECTEDLY(hr))
        return hr;
    return pView->QueryInterface(IID_PPV_ARG(IShellView, ppView));
}
