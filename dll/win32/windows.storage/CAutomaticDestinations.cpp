/*
 * PROJECT:     LiberNT Windows Storage
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Recent and frequent document lists of applications
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

/*
 * File: DWORD magic, DWORD version, DWORD count, then per entry
 * FILETIME last use (two DWORDs), DWORD use count, [v2: DWORD flags],
 * and the item's ID list (ILSaveToStream). Version 1 files load with flags 0.
 */
#define AUTODEST_MAGIC   0x4C4A4441
#define AUTODEST_VERSION 2
#define AUTODEST_FOLDER  L"AutomaticDestinations"
#define AUTODEST_EXT     L"automaticDestinations-ms"
#define AUTODEST_PINNED  0x00000001
/* Unpinned entries kept per app; the least recently used go first. */
#define AUTODEST_MAX_UNPINNED 50

struct AUTODEST_ENTRY
{
    FILETIME LastUsed;
    DWORD UseCount;
    DWORD Flags;
    PIDLIST_ABSOLUTE Pidl;
};

static HRESULT
ResolveAppId(LPCWSTR pszAppID, CStringW &AppId, BOOL *pbExplicit)
{
    WCHAR Module[MAX_PATH];
    PWSTR Explicit = NULL;

    *pbExplicit = TRUE;
    if (pszAppID && *pszAppID)
    {
        AppId = pszAppID;
    }
    else if (SUCCEEDED(GetCurrentProcessExplicitAppUserModelID(&Explicit)) && Explicit && *Explicit)
    {
        AppId = Explicit;
    }
    else if (GetModuleFileNameW(NULL, Module, _countof(Module)))
    {
        AppId = Module;
        *pbExplicit = FALSE;
    }
    else
    {
        AppId.Empty();
    }

    CoTaskMemFree(Explicit);
    return AppId.IsEmpty() ? E_FAIL : S_OK;
}

HRESULT
JumpListPath(LPCWSTR pszAppID, LPCWSTR pszFolder, LPCWSTR pszExtension, BOOL bCreate, CStringW &Path)
{
    WCHAR Recent[MAX_PATH];
    BYTE Hash[8];
    CStringW AppId;
    BOOL bExplicit;
    HRESULT hr;

    hr = ResolveAppId(pszAppID, AppId, &bExplicit);
    if (FAILED(hr))
        return hr;
    AppId.MakeLower();

    hr = SHGetFolderPathW(NULL, CSIDL_RECENT | (bCreate ? CSIDL_FLAG_CREATE : 0), NULL, SHGFP_TYPE_CURRENT, Recent);
    if (FAILED(hr))
        return hr;
    Path = Recent;
    Path.AppendFormat(L"\\%s", pszFolder);
    if (bCreate && !CreateDirectoryW(Path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return HRESULT_FROM_WIN32(GetLastError());

    hr = HashData((LPBYTE)AppId.GetString(), AppId.GetLength() * sizeof(WCHAR), Hash, sizeof(Hash));
    if (FAILED(hr))
        return hr;
    Path.AppendFormat(L"\\%02x%02x%02x%02x%02x%02x%02x%02x.%s",
                      Hash[0], Hash[1], Hash[2], Hash[3], Hash[4], Hash[5], Hash[6], Hash[7], pszExtension);
    return S_OK;
}

static HRESULT
OpenStore(LPCWSTR pszPath, BOOL bWrite, BOOL bCreate, IStream **ppStream)
{
    DWORD Mode = bWrite ? (STGM_READWRITE | STGM_SHARE_EXCLUSIVE) : (STGM_READ | STGM_SHARE_DENY_WRITE);
    HRESULT hr;

    for (UINT Try = 0; ; Try++)
    {
        hr = SHCreateStreamOnFileEx(pszPath, Mode, FILE_ATTRIBUTE_NORMAL, FALSE, NULL, ppStream);
        if (bCreate && hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        {
            hr = SHCreateStreamOnFileEx(pszPath, Mode, FILE_ATTRIBUTE_NORMAL, TRUE, NULL, ppStream);
            if (hr == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) && Try < 100)
                continue;
        }
        if ((hr != STG_E_SHAREVIOLATION && hr != HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) || Try >= 100)
            return hr;
        Sleep(10);
    }
}

static HRESULT
ReadDword(IStream *Stream, DWORD *Value)
{
    ULONG Read = 0;
    HRESULT hr = Stream->Read(Value, sizeof(*Value), &Read);

    if (SUCCEEDED(hr) && Read != sizeof(*Value))
        hr = STG_E_READFAULT;
    return hr;
}

static HRESULT
WriteDword(IStream *Stream, DWORD Value)
{
    ULONG Written = 0;
    HRESULT hr = Stream->Write(&Value, sizeof(Value), &Written);

    if (SUCCEEDED(hr) && Written != sizeof(Value))
        hr = STG_E_MEDIUMFULL;
    return hr;
}

class CAutoDestList
{
public:
    CSimpleArray<AUTODEST_ENTRY> m_Entries;

    ~CAutoDestList()
    {
        for (int i = 0; i < m_Entries.GetSize(); i++)
            ILFree(m_Entries[i].Pidl);
    }

    void Load(IStream *Stream)
    {
        DWORD Magic, Version, Count;

        if (FAILED(ReadDword(Stream, &Magic)) || Magic != AUTODEST_MAGIC ||
            FAILED(ReadDword(Stream, &Version)) || (Version != 1 && Version != AUTODEST_VERSION) ||
            FAILED(ReadDword(Stream, &Count)))
        {
            return;
        }

        for (DWORD i = 0; i < Count; i++)
        {
            AUTODEST_ENTRY Entry;
            LPITEMIDLIST Pidl = NULL;

            Entry.Flags = 0;
            if (FAILED(ReadDword(Stream, &Entry.LastUsed.dwLowDateTime)) ||
                FAILED(ReadDword(Stream, &Entry.LastUsed.dwHighDateTime)) ||
                FAILED(ReadDword(Stream, &Entry.UseCount)) ||
                (Version >= 2 && FAILED(ReadDword(Stream, &Entry.Flags))) ||
                FAILED(ILLoadFromStream(Stream, &Pidl)) || !Pidl)
            {
                ILFree(Pidl);
                return;
            }
            Entry.Pidl = (PIDLIST_ABSOLUTE)Pidl;
            if (!m_Entries.Add(Entry))
            {
                ILFree(Entry.Pidl);
                return;
            }
        }
    }

    HRESULT Save(IStream *Stream)
    {
        LARGE_INTEGER Zero = { { 0, 0 } };
        ULARGE_INTEGER End;
        HRESULT hr;

        hr = Stream->Seek(Zero, STREAM_SEEK_SET, NULL);
        if (SUCCEEDED(hr))
            hr = WriteDword(Stream, AUTODEST_MAGIC);
        if (SUCCEEDED(hr))
            hr = WriteDword(Stream, AUTODEST_VERSION);
        if (SUCCEEDED(hr))
            hr = WriteDword(Stream, m_Entries.GetSize());
        for (int i = 0; SUCCEEDED(hr) && i < m_Entries.GetSize(); i++)
        {
            hr = WriteDword(Stream, m_Entries[i].LastUsed.dwLowDateTime);
            if (SUCCEEDED(hr))
                hr = WriteDword(Stream, m_Entries[i].LastUsed.dwHighDateTime);
            if (SUCCEEDED(hr))
                hr = WriteDword(Stream, m_Entries[i].UseCount);
            if (SUCCEEDED(hr))
                hr = WriteDword(Stream, m_Entries[i].Flags);
            if (SUCCEEDED(hr))
                hr = ILSaveToStream(Stream, m_Entries[i].Pidl);
        }
        if (SUCCEEDED(hr))
            hr = Stream->Seek(Zero, STREAM_SEEK_CUR, &End);
        if (SUCCEEDED(hr))
            hr = Stream->SetSize(End);
        if (SUCCEEDED(hr))
            hr = Stream->Commit(STGC_DEFAULT);
        return hr;
    }

    int Find(PCIDLIST_ABSOLUTE Pidl)
    {
        CComPtr<IShellFolder> Desktop;
        WCHAR Path[MAX_PATH], EntryPath[MAX_PATH];
        BOOL bPath = SHGetPathFromIDListW(Pidl, Path);

        SHGetDesktopFolder(&Desktop);
        for (int i = 0; i < m_Entries.GetSize(); i++)
        {
            if (ILIsEqual(m_Entries[i].Pidl, Pidl))
                return i;
            if (Desktop && Desktop->CompareIDs(SHCIDS_CANONICALONLY, m_Entries[i].Pidl, Pidl) == S_OK)
                return i;
            if (bPath && SHGetPathFromIDListW(m_Entries[i].Pidl, EntryPath) && !_wcsicmp(EntryPath, Path))
                return i;
        }
        return -1;
    }

    /* Drop the least recently used unpinned entries beyond the cap. */
    void Trim()
    {
        for (;;)
        {
            int Unpinned = 0, Oldest = -1;

            for (int i = 0; i < m_Entries.GetSize(); i++)
            {
                if (m_Entries[i].Flags & AUTODEST_PINNED)
                    continue;
                Unpinned++;
                if (Oldest < 0 || CompareFileTime(&m_Entries[i].LastUsed, &m_Entries[Oldest].LastUsed) < 0)
                    Oldest = i;
            }
            if (Unpinned <= AUTODEST_MAX_UNPINNED || Oldest < 0)
                return;
            ILFree(m_Entries[Oldest].Pidl);
            m_Entries.RemoveAt(Oldest);
        }
    }

    BOOL Before(int First, int Second, APPDOCLISTTYPE ListType)
    {
        const AUTODEST_ENTRY &A = m_Entries[First], &B = m_Entries[Second];

        if (ListType == ADLT_FREQUENT && A.UseCount != B.UseCount)
            return A.UseCount > B.UseCount;
        return CompareFileTime(&A.LastUsed, &B.LastUsed) > 0;
    }
};

static BOOL
ProgIdHandlesApp(LPCWSTR pszProgId, LPCWSTR pszAppId, BOOL bExplicit)
{
    WCHAR Value[MAX_PATH];
    DWORD Size = sizeof(Value);

    if (bExplicit)
    {
        return RegGetValueW(HKEY_CLASSES_ROOT, pszProgId, L"AppUserModelID", RRF_RT_REG_SZ, NULL,
                            Value, &Size) == ERROR_SUCCESS && !_wcsicmp(Value, pszAppId);
    }

    Size = _countof(Value);
    return SUCCEEDED(AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_EXECUTABLE, pszProgId, NULL, Value, &Size)) &&
           !_wcsicmp(Value, pszAppId);
}

static BOOL
IsRegisteredHandler(PCIDLIST_ABSOLUTE Pidl, LPCWSTR pszAppId, BOOL bExplicit)
{
    WCHAR Path[MAX_PATH], ProgId[MAX_PATH];
    CStringW Key;
    LPCWSTR Ext;
    DWORD Size = sizeof(ProgId);
    HKEY hKey;
    BOOL bFound = FALSE;

    if (!SHGetPathFromIDListW(Pidl, Path))
        return FALSE;
    Ext = PathFindExtensionW(Path);
    if (!*Ext)
        return FALSE;

    if (RegGetValueW(HKEY_CLASSES_ROOT, Ext, NULL, RRF_RT_REG_SZ, NULL, ProgId, &Size) == ERROR_SUCCESS &&
        ProgIdHandlesApp(ProgId, pszAppId, bExplicit))
    {
        return TRUE;
    }

    Key.Format(L"%s\\OpenWithProgids", Ext);
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, Key, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return FALSE;
    for (DWORD i = 0; !bFound; i++)
    {
        DWORD cch = _countof(ProgId);
        if (RegEnumValueW(hKey, i, ProgId, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        bFound = ProgIdHandlesApp(ProgId, pszAppId, bExplicit);
    }
    RegCloseKey(hKey);
    return bFound;
}

/* The app that opens the item by default: its ProgID's AppUserModelID, else
   its executable. Used when the caller (for example Explorer opening a file)
   is not itself a handler of the item. */
static BOOL
DefaultHandlerAppId(PCIDLIST_ABSOLUTE Pidl, CStringW &AppId)
{
    WCHAR Path[MAX_PATH], ProgId[MAX_PATH], Value[MAX_PATH];
    DWORD Size = sizeof(ProgId);
    LPCWSTR Ext;

    if (!SHGetPathFromIDListW(Pidl, Path))
        return FALSE;
    Ext = PathFindExtensionW(Path);
    if (!*Ext)
        return FALSE;

    if (RegGetValueW(HKEY_CLASSES_ROOT, Ext, NULL, RRF_RT_REG_SZ, NULL, ProgId, &Size) == ERROR_SUCCESS)
    {
        Size = sizeof(Value);
        if (RegGetValueW(HKEY_CLASSES_ROOT, ProgId, L"AppUserModelID", RRF_RT_REG_SZ, NULL,
                         Value, &Size) == ERROR_SUCCESS && Value[0])
        {
            AppId = Value;
            return TRUE;
        }
    }

    Size = _countof(Value);
    if (FAILED(AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_EXECUTABLE, Ext, NULL, Value, &Size)) || !Value[0])
        return FALSE;
    AppId = Value;
    return TRUE;
}

EXTERN_C HRESULT
SHELL_AddAutomaticDestination(LPCWSTR pszAppID, PCIDLIST_ABSOLUTE pidl)
{
    CComPtr<IStream> Stream;
    CAutoDestList List;
    CStringW AppId, Path;
    AUTODEST_ENTRY Entry;
    BOOL bExplicit;
    int Index;
    HRESULT hr;

    hr = ResolveAppId(pszAppID, AppId, &bExplicit);
    if (FAILED(hr))
        return hr;
    if (!IsRegisteredHandler(pidl, AppId, bExplicit))
    {
        /* Only a caller that named no AppID is re-attributed. */
        if ((pszAppID && *pszAppID) || !DefaultHandlerAppId(pidl, AppId))
            return S_FALSE;
        TRACE("Recent item attributed to its default handler %S\n", AppId.GetString());
    }

    hr = JumpListPath(AppId, AUTODEST_FOLDER, AUTODEST_EXT, TRUE, Path);
    if (SUCCEEDED(hr))
        hr = OpenStore(Path, TRUE, TRUE, &Stream);
    if (FAILED(hr))
        return hr;
    List.Load(Stream);

    Index = List.Find(pidl);
    if (Index >= 0)
    {
        Entry = List.m_Entries[Index];
        List.m_Entries.RemoveAt(Index);
    }
    else
    {
        Entry.UseCount = 0;
        Entry.Flags = 0;
        Entry.Pidl = ILClone(pidl);
        if (!Entry.Pidl)
            return E_OUTOFMEMORY;
    }
    Entry.UseCount++;
    GetSystemTimeAsFileTime(&Entry.LastUsed);
    if (!List.m_Entries.Add(Entry))
    {
        ILFree(Entry.Pidl);
        return E_OUTOFMEMORY;
    }
    List.Trim();
    return List.Save(Stream);
}

static HRESULT
AutoDestPidlFromObject(IUnknown *punk, PIDLIST_ABSOLUTE *ppidl)
{
    CComPtr<IShellLinkW> Link;

    *ppidl = NULL;
    if (!punk)
        return E_INVALIDARG;
    if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARG(IShellLinkW, &Link))))
        return Link->GetIDList(ppidl);
    return SHGetIDListFromObject(punk, ppidl);
}

/* The taskbar's view of an automatic list: pinned entries in pin order, or
   unpinned ones by recency or use count. Items are IShellItem objects. */
EXTERN_C HRESULT
SHELL_GetAutomaticDestinations(LPCWSTR pszAppID, DWORD dwList, UINT cMax, REFIID riid, void **ppv)
{
    CComPtr<IObjectCollection> Collection;
    CComPtr<IStream> Stream;
    CSimpleArray<int> Order;
    CAutoDestList List;
    CStringW Path;
    APPDOCLISTTYPE SortType = (dwList == JUMPLIST_AUTO_FREQUENT) ? ADLT_FREQUENT : ADLT_RECENT;
    HRESULT hr;

    if (!ppv)
        return E_POINTER;
    *ppv = NULL;
    if (!pszAppID || !*pszAppID || dwList > JUMPLIST_AUTO_PINNED)
        return E_INVALIDARG;

    hr = ShellObjectCreator<CEnumerableObjectCollection>(IID_PPV_ARG(IObjectCollection, &Collection));
    if (FAILED(hr))
        return hr;
    if (SUCCEEDED(JumpListPath(pszAppID, AUTODEST_FOLDER, AUTODEST_EXT, FALSE, Path)) &&
        SUCCEEDED(OpenStore(Path, FALSE, FALSE, &Stream)))
    {
        List.Load(Stream);
    }

    for (int i = 0; i < List.m_Entries.GetSize(); i++)
    {
        BOOL bPinned = (List.m_Entries[i].Flags & AUTODEST_PINNED) != 0;
        int Pos = Order.GetSize();

        if (bPinned != (dwList == JUMPLIST_AUTO_PINNED))
            continue;
        if (!Order.Add(i))
            return E_OUTOFMEMORY;
        while (dwList != JUMPLIST_AUTO_PINNED && Pos > 0 && List.Before(i, Order[Pos - 1], SortType))
        {
            Order[Pos] = Order[Pos - 1];
            Pos--;
        }
        Order[Pos] = i;
    }

    for (int i = 0; i < Order.GetSize() && (!cMax || (UINT)i < cMax); i++)
    {
        CComPtr<IShellItem> Item;

        if (SUCCEEDED(SHCreateItemFromIDList(List.m_Entries[Order[i]].Pidl, IID_PPV_ARG(IShellItem, &Item))))
        {
            hr = Collection->AddObject(Item);
            if (FAILED(hr))
                return hr;
        }
    }
    return Collection->QueryInterface(riid, ppv);
}

/* Pin to / unpin from an app's list. Pinning an item that is not listed
   adds it; pinned entries are never trimmed. Unpinning keeps it as recent. */
EXTERN_C HRESULT
SHELL_PinAutomaticDestination(LPCWSTR pszAppID, IUnknown *punk, BOOL bPin)
{
    CComHeapPtr<ITEMIDLIST_ABSOLUTE> Pidl;
    CComPtr<IStream> Stream;
    CAutoDestList List;
    CStringW Path;
    AUTODEST_ENTRY Entry;
    int Index;
    HRESULT hr;

    if (!pszAppID || !*pszAppID)
        return E_INVALIDARG;
    hr = AutoDestPidlFromObject(punk, &Pidl);
    if (FAILED(hr))
        return hr;
    hr = JumpListPath(pszAppID, AUTODEST_FOLDER, AUTODEST_EXT, bPin, Path);
    if (SUCCEEDED(hr))
        hr = OpenStore(Path, TRUE, bPin, &Stream);
    if (!bPin && (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)))
        return S_FALSE;
    if (FAILED(hr))
        return hr;
    List.Load(Stream);

    Index = List.Find(Pidl);
    if (Index < 0)
    {
        if (!bPin)
            return S_FALSE;
        Entry.UseCount = 1;
        Entry.Flags = 0;
        GetSystemTimeAsFileTime(&Entry.LastUsed);
        Entry.Pidl = ILClone(Pidl);
        if (!Entry.Pidl)
            return E_OUTOFMEMORY;
    }
    else
    {
        /* Re-append so that pin order is the order of pinning. */
        Entry = List.m_Entries[Index];
        List.m_Entries.RemoveAt(Index);
    }
    if (bPin)
        Entry.Flags |= AUTODEST_PINNED;
    else
        Entry.Flags &= ~AUTODEST_PINNED;
    if (!List.m_Entries.Add(Entry))
    {
        ILFree(Entry.Pidl);
        return E_OUTOFMEMORY;
    }
    List.Trim();
    return List.Save(Stream);
}

STDMETHODIMP
CApplicationDocumentLists::SetAppID(LPCWSTR pszAppID)
{
    if (!pszAppID || !*pszAppID)
        return E_INVALIDARG;
    m_AppId = pszAppID;
    return S_OK;
}

STDMETHODIMP
CApplicationDocumentLists::GetList(APPDOCLISTTYPE listtype, UINT cItemsDesired, REFIID riid, void **ppv)
{
    CComPtr<IObjectCollection> Collection;
    CComPtr<IStream> Stream;
    CSimpleArray<int> Order;
    CAutoDestList List;
    CStringW Path;
    HRESULT hr;

    if (!ppv)
        return E_POINTER;
    *ppv = NULL;
    if (listtype != ADLT_RECENT && listtype != ADLT_FREQUENT)
        return E_INVALIDARG;

    hr = ShellObjectCreator<CEnumerableObjectCollection>(IID_PPV_ARG(IObjectCollection, &Collection));
    if (FAILED(hr))
        return hr;
    if (SUCCEEDED(JumpListPath(m_AppId, AUTODEST_FOLDER, AUTODEST_EXT, FALSE, Path)) &&
        SUCCEEDED(OpenStore(Path, FALSE, FALSE, &Stream)))
    {
        List.Load(Stream);
    }

    for (int i = 0; i < List.m_Entries.GetSize(); i++)
    {
        int Pos = Order.GetSize();

        if (!Order.Add(i))
            return E_OUTOFMEMORY;
        while (Pos > 0 && List.Before(i, Order[Pos - 1], listtype))
        {
            Order[Pos] = Order[Pos - 1];
            Pos--;
        }
        Order[Pos] = i;
    }

    for (int i = 0; i < Order.GetSize() && (!cItemsDesired || (UINT)i < cItemsDesired); i++)
    {
        CComPtr<IShellItem> Item;

        if (SUCCEEDED(SHCreateItemFromIDList(List.m_Entries[Order[i]].Pidl, IID_PPV_ARG(IShellItem, &Item))))
        {
            hr = Collection->AddObject(Item);
            if (FAILED(hr))
                return hr;
        }
    }
    return Collection->QueryInterface(riid, ppv);
}

STDMETHODIMP
CApplicationDestinations::SetAppID(LPCWSTR pszAppID)
{
    if (!pszAppID || !*pszAppID)
        return E_INVALIDARG;
    m_AppId = pszAppID;
    return S_OK;
}

STDMETHODIMP
CApplicationDestinations::RemoveDestination(IUnknown *punk)
{
    CComHeapPtr<ITEMIDLIST_ABSOLUTE> Pidl;
    CComPtr<IShellLinkW> Link;
    CComPtr<IStream> Stream;
    CAutoDestList List;
    CStringW Path;
    int Index;
    HRESULT hr;

    if (!punk)
        return E_INVALIDARG;
    if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARG(IShellLinkW, &Link))))
        hr = Link->GetIDList(&Pidl);
    else
        hr = SHGetIDListFromObject(punk, &Pidl);
    if (FAILED(hr))
        return hr;

    hr = JumpListPath(m_AppId, AUTODEST_FOLDER, AUTODEST_EXT, FALSE, Path);
    if (FAILED(hr))
        return hr;
    hr = OpenStore(Path, TRUE, FALSE, &Stream);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (FAILED(hr))
        return hr;
    List.Load(Stream);

    Index = List.Find(Pidl);
    if (Index < 0)
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    ILFree(List.m_Entries[Index].Pidl);
    List.m_Entries.RemoveAt(Index);
    return List.Save(Stream);
}

STDMETHODIMP
CApplicationDestinations::RemoveAllDestinations()
{
    CStringW Path;
    HRESULT hr;

    hr = JumpListPath(m_AppId, AUTODEST_FOLDER, AUTODEST_EXT, FALSE, Path);
    if (FAILED(hr))
        return hr;
    if (!DeleteFileW(Path) &&
        GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}
