/*
 * PROJECT:     LiberNT Explorer
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Jump list section of the task button menu
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The lists themselves live in windows.storage: ICustomDestinationList writes
 * an app's custom list, SHAddToRecentDocs its automatic (recent/frequent/
 * pinned) list. Nothing here indexes or watches anything: the store is read
 * when the menu opens.
 */

#include "precomp.h"
#include "jumplist.h"

#define JUMPLIST_DEFAULT_SLOTS 10
#define JUMPLIST_MAX_TEXT      64

/* PKEY_Title and PKEY_AppUserModel_IsDestListSeparator */
static const PROPERTYKEY JlPkeyTitle =
    { { 0xF29F85E0, 0x4FF9, 0x1068, { 0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9 } }, 2 };
static const PROPERTYKEY JlPkeyIsSeparator =
    { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 6 };

enum
{
    IDM_ITEM_OPEN = 1,
    IDM_ITEM_PIN,
    IDM_ITEM_UNPIN,
    IDM_ITEM_REMOVE
};

CTaskJumpList *CTaskJumpList::s_Tracking = NULL;

CTaskJumpList::CTaskJumpList() :
    m_bExplicit(FALSE),
    m_hMenu(NULL),
    m_hHook(NULL),
    m_ContextCmd(0),
    m_Pos(0)
{
}

CTaskJumpList::~CTaskJumpList()
{
    EndTrack();
    for (INT i = 0; i < m_Entries.GetSize(); i++)
        m_Entries[i].Object->Release();
}

static UINT
JumpListSlots()
{
    DWORD Slots = JUMPLIST_DEFAULT_SLOTS, Size = sizeof(Slots);

    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                     L"JumpListItems_Maximum", RRF_RT_REG_DWORD, NULL, &Slots, &Size) != ERROR_SUCCESS ||
        Slots == 0 || Slots > 60)
    {
        Slots = JUMPLIST_DEFAULT_SLOTS;
    }
    return Slots;
}

/* Menu text: '&' doubled, long names shortened. */
static VOID
JumpListMenuText(LPCWSTR pszIn, CStringW &Out)
{
    CStringW Text = pszIn ? pszIn : L"";

    if (Text.GetLength() > JUMPLIST_MAX_TEXT)
        Text = Text.Left(JUMPLIST_MAX_TEXT - 3) + L"...";
    Text.Replace(L"&", L"&&");
    Out = Text;
}

static BOOL
JumpListLinkTitle(IShellLinkW *Link, CStringW &Title)
{
    CComPtr<IPropertyStore> Store;
    WCHAR szBuf[MAX_PATH];

    Title.Empty();
    if (SUCCEEDED(Link->QueryInterface(IID_PPV_ARG(IPropertyStore, &Store))))
    {
        PROPVARIANT Value;
        PropVariantInit(&Value);
        if (SUCCEEDED(Store->GetValue(JlPkeyTitle, &Value)) && Value.vt == VT_LPWSTR && Value.pwszVal)
            Title = Value.pwszVal;
        PropVariantClear(&Value);
    }
    if (Title.IsEmpty() && SUCCEEDED(Link->GetDescription(szBuf, _countof(szBuf))) && szBuf[0])
        Title = szBuf;
    if (Title.IsEmpty() && SUCCEEDED(Link->GetPath(szBuf, _countof(szBuf), NULL, 0)) && szBuf[0])
    {
        Title = PathFindFileNameW(szBuf);
        PathRemoveExtensionW(Title.GetBuffer());
        Title.ReleaseBuffer();
    }
    return !Title.IsEmpty();
}

static BOOL
JumpListIsSeparator(IShellLinkW *Link)
{
    CComPtr<IPropertyStore> Store;
    PROPVARIANT Value;
    BOOL bSeparator = FALSE;

    if (FAILED(Link->QueryInterface(IID_PPV_ARG(IPropertyStore, &Store))))
        return FALSE;
    PropVariantInit(&Value);
    if (SUCCEEDED(Store->GetValue(JlPkeyIsSeparator, &Value)) && Value.vt == VT_BOOL)
        bSeparator = (Value.boolVal != VARIANT_FALSE);
    PropVariantClear(&Value);
    return bSeparator;
}

/* A file-system item whose file is gone is not shown. */
static BOOL
JumpListItemUsable(IShellItem *Item, CStringW &Name)
{
    LPWSTR psz = NULL;
    SFGAOF Attributes = 0;

    if (SUCCEEDED(Item->GetAttributes(SFGAO_FILESYSTEM, &Attributes)) && (Attributes & SFGAO_FILESYSTEM) &&
        SUCCEEDED(Item->GetDisplayName(SIGDN_FILESYSPATH, &psz)))
    {
        BOOL bExists = PathFileExistsW(psz);
        CoTaskMemFree(psz);
        psz = NULL;
        if (!bExists)
            return FALSE;
    }
    if (FAILED(Item->GetDisplayName(SIGDN_NORMALDISPLAY, &psz)) || !psz)
        return FALSE;
    Name = psz;
    CoTaskMemFree(psz);
    return !Name.IsEmpty();
}

VOID CTaskJumpList::InsertHeader(LPCWSTR pszText)
{
    CStringW Text;

    JumpListMenuText(pszText, Text);
    if (InsertMenuW(m_hMenu, m_Pos, MF_BYPOSITION | MF_STRING | MF_GRAYED | MF_DISABLED, 0, Text))
        m_Pos++;
}

VOID CTaskJumpList::InsertSeparator()
{
    if (m_Pos > 0 && InsertMenuW(m_hMenu, m_Pos, MF_BYPOSITION | MF_SEPARATOR, 0, NULL))
        m_Pos++;
}

BOOL CTaskJumpList::InsertObject(IUnknown *Object, SOURCE Source)
{
    CComPtr<IShellLinkW> Link;
    CComPtr<IShellItem> Item;
    CStringW Name, Text;
    ENTRY Entry;
    UINT Id = IDM_JUMPLIST_FIRST + m_Entries.GetSize();

    if (Id > IDM_JUMPLIST_LAST)
        return FALSE;
    if (SUCCEEDED(Object->QueryInterface(IID_PPV_ARG(IShellLinkW, &Link))))
    {
        if (Source == SRC_TASK && JumpListIsSeparator(Link))
        {
            InsertSeparator();
            return TRUE;
        }
        if (!JumpListLinkTitle(Link, Name))
            return FALSE;
        Entry.Object = Link;
        Entry.bDocument = FALSE;
    }
    else if (SUCCEEDED(Object->QueryInterface(IID_PPV_ARG(IShellItem, &Item))))
    {
        if (!JumpListItemUsable(Item, Name))
            return FALSE;
        Entry.Object = Item;
        Entry.bDocument = TRUE;
    }
    else
    {
        return FALSE;
    }

    JumpListMenuText(Name, Text);
    if (!InsertMenuW(m_hMenu, m_Pos, MF_BYPOSITION | MF_STRING, Id, Text))
        return FALSE;
    Entry.Source = Source;
    Entry.Object->AddRef();
    if (!m_Entries.Add(Entry))
    {
        Entry.Object->Release();
        DeleteMenu(m_hMenu, m_Pos, MF_BYPOSITION);
        return FALSE;
    }
    m_Pos++;
    return TRUE;
}

VOID CTaskJumpList::InsertArray(IObjectArray *Array, SOURCE Source, LPCWSTR pszHeader, UINT cMax)
{
    UINT Count = 0, Shown = 0, HeaderPos = m_Pos;
    BOOL bHeader = FALSE;

    if (!Array || FAILED(Array->GetCount(&Count)))
        return;
    for (UINT i = 0; i < Count && (!cMax || Shown < cMax); i++)
    {
        CComPtr<IUnknown> Object;

        if (FAILED(Array->GetAt(i, IID_PPV_ARG(IUnknown, &Object))))
            continue;
        if (!bHeader && pszHeader)
        {
            InsertSeparator();
            InsertHeader(pszHeader);
            HeaderPos = m_Pos;
            bHeader = TRUE;
        }
        if (InsertObject(Object, Source))
            Shown++;
    }
    /* A header with nothing under it goes again. */
    if (bHeader && m_Pos == HeaderPos)
    {
        DeleteMenu(m_hMenu, --m_Pos, MF_BYPOSITION);
        if (m_Pos > 0)
        {
            MENUITEMINFOW mii = { sizeof(mii), MIIM_FTYPE };
            if (GetMenuItemInfoW(m_hMenu, m_Pos - 1, TRUE, &mii) && (mii.fType & MFT_SEPARATOR))
                DeleteMenu(m_hMenu, --m_Pos, MF_BYPOSITION);
        }
    }
}

VOID CTaskJumpList::InsertAuto(DWORD dwList, LPCWSTR pszHeader, UINT cMax)
{
    CComPtr<IObjectArray> Array;

    if (SUCCEEDED(SHELL_GetAutomaticDestinations(m_Key, dwList, cMax, IID_PPV_ARG(IObjectArray, &Array))))
        InsertArray(Array, dwList == JUMPLIST_AUTO_PINNED ? SRC_PINNED : SRC_AUTO, pszHeader, cMax);
}

UINT CTaskJumpList::Build(HMENU hMenu, LPCWSTR pszAppId, LPCWSTR pszExePath)
{
    PJUMPLIST_SECTION Sections = NULL;
    UINT cSections = 0, i, Slots = JumpListSlots();
    BOOL bKnown = FALSE;

    m_hMenu = hMenu;
    m_Pos = 0;
    m_bExplicit = pszAppId && *pszAppId;
    m_Key = m_bExplicit ? pszAppId : pszExePath;
    m_ExePath = pszExePath ? pszExePath : L"";
    if (!hMenu || m_Key.IsEmpty())
        return 0;

    if (SHELL_LoadCustomDestinations(m_Key, &Sections, &cSections) != S_OK)
        cSections = 0;
    TRACE("Jump list for %S: %u custom section(s)\n", m_Key.GetString(), cSections);

    InsertAuto(JUMPLIST_AUTO_PINNED, L"Pinned", 0);
    for (i = 0; i < cSections; i++)
    {
        if (Sections[i].Type == JUMPLIST_SECTION_CUSTOM && Sections[i].pszName)
        {
            InsertArray(Sections[i].Items, SRC_CUSTOM, Sections[i].pszName, Slots);
        }
        else if (Sections[i].Type == JUMPLIST_SECTION_KNOWN)
        {
            BOOL bFrequent = (Sections[i].Category == KDC_FREQUENT);
            InsertAuto(bFrequent ? JUMPLIST_AUTO_FREQUENT : JUMPLIST_AUTO_RECENT,
                       bFrequent ? L"Frequent" : L"Recent", Slots);
            bKnown = TRUE;
        }
    }
    /* An app that customized nothing gets its Recent list, as on Windows. */
    if (!cSections && !bKnown)
        InsertAuto(JUMPLIST_AUTO_RECENT, L"Recent", Slots);
    for (i = 0; i < cSections; i++)
    {
        if (Sections[i].Type == JUMPLIST_SECTION_TASKS)
            InsertArray(Sections[i].Items, SRC_TASK, L"Tasks", 0);
    }
    SHELL_FreeCustomDestinations(Sections, cSections);
    return m_Pos;
}

/* A right click on a document closes the menu; Invoke then offers its own
   menu. The menu loop of this thread calls MSGF_MENU filters for every
   message, so this works without changes to the owner window. */
LRESULT CALLBACK CTaskJumpList::MsgFilterProc(INT Code, WPARAM wParam, LPARAM lParam)
{
    CTaskJumpList *This = s_Tracking;
    MSG *pMsg = (MSG *)lParam;

    if (Code == MSGF_MENU && This && pMsg &&
        (pMsg->message == WM_RBUTTONDOWN || pMsg->message == WM_RBUTTONUP))
    {
        INT Pos = MenuItemFromPoint(NULL, This->m_hMenu, pMsg->pt);
        INT Cmd = (Pos >= 0) ? (INT)GetMenuItemID(This->m_hMenu, Pos) : -1;
        ENTRY *Entry = This->EntryFromCmd(Cmd);

        if (Entry && Entry->bDocument)
        {
            if (pMsg->message == WM_RBUTTONUP)
            {
                This->m_ContextCmd = Cmd;
                EndMenu();
            }
            return TRUE; /* not a selection */
        }
    }
    return CallNextHookEx(This ? This->m_hHook : NULL, Code, wParam, lParam);
}

VOID CTaskJumpList::BeginTrack(HMENU hMenu)
{
    m_hMenu = hMenu;
    m_ContextCmd = 0;
    if (!m_Entries.GetSize() || s_Tracking)
        return;
    m_hHook = SetWindowsHookExW(WH_MSGFILTER, MsgFilterProc, NULL, GetCurrentThreadId());
    if (m_hHook)
        s_Tracking = this;
}

VOID CTaskJumpList::EndTrack()
{
    if (m_hHook)
    {
        UnhookWindowsHookEx(m_hHook);
        m_hHook = NULL;
    }
    if (s_Tracking == this)
        s_Tracking = NULL;
}

CTaskJumpList::ENTRY *CTaskJumpList::EntryFromCmd(INT Cmd)
{
    if (Cmd < IDM_JUMPLIST_FIRST || Cmd > IDM_JUMPLIST_LAST)
        return NULL;
    Cmd -= IDM_JUMPLIST_FIRST;
    return (Cmd < m_Entries.GetSize()) ? &m_Entries[Cmd] : NULL;
}

VOID CTaskJumpList::Launch(ENTRY *Entry)
{
    CComPtr<IShellLinkW> Link;
    CComPtr<IShellItem> Item;
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    WCHAR szPath[MAX_PATH], szArgs[MAX_PATH * 2], szDir[MAX_PATH];
    CComHeapPtr<ITEMIDLIST> Pidl;
    CStringW Params;
    INT nShow = SW_SHOWNORMAL;

    sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    sei.nShow = SW_SHOWNORMAL;

    if (SUCCEEDED(Entry->Object->QueryInterface(IID_PPV_ARG(IShellLinkW, &Link))))
    {
        szPath[0] = szArgs[0] = szDir[0] = 0;
        Link->GetPath(szPath, _countof(szPath), NULL, 0);
        Link->GetArguments(szArgs, _countof(szArgs));
        Link->GetWorkingDirectory(szDir, _countof(szDir));
        if (SUCCEEDED(Link->GetShowCmd(&nShow)))
            sei.nShow = nShow;
        if (szPath[0])
        {
            sei.lpFile = szPath;
        }
        else if (SUCCEEDED(Link->GetIDList(&Pidl)) && Pidl)
        {
            sei.fMask |= SEE_MASK_IDLIST;
            sei.lpIDList = Pidl;
        }
        else
        {
            return;
        }
        sei.lpParameters = szArgs[0] ? szArgs : NULL;
        sei.lpDirectory = szDir[0] ? szDir : NULL;
    }
    else if (SUCCEEDED(Entry->Object->QueryInterface(IID_PPV_ARG(IShellItem, &Item))) &&
             SUCCEEDED(SHGetIDListFromObject(Item, &Pidl)))
    {
        /* A document opens in the app whose list it is when that is an exe;
           an AppUserModelID's documents use their default verb. */
        if (!m_bExplicit && !m_ExePath.IsEmpty() && SHGetPathFromIDListW(Pidl, szPath))
        {
            Params.Format(L"\"%s\"", szPath);
            sei.lpFile = m_ExePath;
            sei.lpParameters = Params;
        }
        else
        {
            sei.fMask |= SEE_MASK_INVOKEIDLIST;
            sei.lpIDList = Pidl;
        }
    }
    else
    {
        return;
    }

    if (!ShellExecuteExW(&sei))
    {
        WARN("Jump list launch failed: %lu\n", GetLastError());
        return;
    }

    /* A document used again moves up its list. */
    if (Item)
    {
        if (m_bExplicit)
        {
            SHARDAPPIDINFO Info = { Item, m_Key };
            SHAddToRecentDocs(SHARD_APPIDINFO, &Info);
        }
        else if (SHGetPathFromIDListW(Pidl, szPath))
        {
            SHAddToRecentDocs(SHARD_PATHW, szPath);
        }
    }
}

VOID CTaskJumpList::ShowItemMenu(ENTRY *Entry, HWND hwndOwner)
{
    HMENU hMenu = CreatePopupMenu();
    POINT pt;
    INT Cmd;

    if (!hMenu)
        return;
    AppendMenuW(hMenu, MF_STRING, IDM_ITEM_OPEN, L"Open");
    SetMenuDefaultItem(hMenu, IDM_ITEM_OPEN, FALSE);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    if (Entry->Source == SRC_PINNED)
        AppendMenuW(hMenu, MF_STRING, IDM_ITEM_UNPIN, L"Unpin from this list");
    else
        AppendMenuW(hMenu, MF_STRING, IDM_ITEM_PIN, L"Pin to this list");
    if (Entry->Source == SRC_PINNED || Entry->Source == SRC_AUTO)
        AppendMenuW(hMenu, MF_STRING, IDM_ITEM_REMOVE, L"Remove from this list");

    GetCursorPos(&pt);
    SetForegroundWindow(hwndOwner);
    Cmd = TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, hwndOwner, NULL);
    PostMessageW(hwndOwner, WM_NULL, 0, 0);
    DestroyMenu(hMenu);

    switch (Cmd)
    {
    case IDM_ITEM_OPEN:
        Launch(Entry);
        break;

    case IDM_ITEM_PIN:
    case IDM_ITEM_UNPIN:
    {
        HRESULT hr = SHELL_PinAutomaticDestination(m_Key, Entry->Object, Cmd == IDM_ITEM_PIN);
        if (FAILED(hr))
            WARN("Jump list %s failed: %08lx\n", Cmd == IDM_ITEM_PIN ? "pin" : "unpin", hr);
        break;
    }

    case IDM_ITEM_REMOVE:
    {
        CComPtr<IApplicationDestinations> Destinations;
        HRESULT hr = CoCreateInstance(CLSID_ApplicationDestinations, NULL, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARG(IApplicationDestinations, &Destinations));
        if (SUCCEEDED(hr))
            hr = Destinations->SetAppID(m_Key);
        if (SUCCEEDED(hr))
            hr = Destinations->RemoveDestination(Entry->Object);
        if (FAILED(hr))
            WARN("Jump list remove failed: %08lx\n", hr);
        break;
    }
    }
}

BOOL CTaskJumpList::Invoke(INT Cmd, HWND hwndOwner)
{
    ENTRY *Entry;

    EndTrack();
    if (Cmd == 0 && m_ContextCmd)
    {
        Entry = EntryFromCmd(m_ContextCmd);
        m_ContextCmd = 0;
        if (Entry)
            ShowItemMenu(Entry, hwndOwner);
        return TRUE;
    }

    Entry = EntryFromCmd(Cmd);
    if (!Entry)
        return FALSE;
    Launch(Entry);
    return TRUE;
}
