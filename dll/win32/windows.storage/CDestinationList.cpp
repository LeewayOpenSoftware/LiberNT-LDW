/*
 * PROJECT:     LiberNT Windows Storage
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Custom jump list builder
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

/*
 * File: DWORD magic, DWORD version, DWORD section count, then per section
 * DWORD type (1 custom, 2 known, 3 tasks); custom: DWORD name length + WCHARs;
 * known: DWORD KNOWNDESTCATEGORY; then (all types) DWORD item count and per
 * item DWORD kind (1 ID list via ILSaveToStream, 2 IShellLink via IPersistStream).
 */
#define DESTLIST_MAGIC          0x4C4A4443
#define DESTLIST_VERSION        1
#define DESTLIST_SECTION_CUSTOM 1
#define DESTLIST_SECTION_KNOWN  2
#define DESTLIST_SECTION_TASKS  3
#define DESTLIST_ITEM_IDLIST    1
#define DESTLIST_ITEM_LINK      2
#define DESTLIST_DEFAULT_SLOTS  10

static HRESULT
DestListWrite(IStream *Stream, const void *Data, ULONG Size)
{
    ULONG Written = 0;
    HRESULT hr = Stream->Write(Data, Size, &Written);

    if (SUCCEEDED(hr) && Written != Size)
        hr = STG_E_MEDIUMFULL;
    return hr;
}

static HRESULT
DestListWriteDword(IStream *Stream, DWORD Value)
{
    return DestListWrite(Stream, &Value, sizeof(Value));
}

static HRESULT
DestListCreateEmpty(REFIID riid, void **ppv)
{
    return ShellObjectCreator<CEnumerableObjectCollection>(riid, ppv);
}

HRESULT
CDestinationList::ListPath(LPCWSTR pszAppID, BOOL Create, CStringW &Path)
{
    if (!pszAppID || !*pszAppID)
        pszAppID = m_AppId;
    return JumpListPath(pszAppID, L"CustomDestinations", L"customDestinations-ms", Create, Path);
}

HRESULT
CDestinationList::WriteSection(DWORD Type, LPCWSTR Name, DWORD Category, IObjectArray *poa)
{
    ULARGE_INTEGER Start;
    LARGE_INTEGER Zero = { { 0, 0 } };
    UINT Count = 0, i;
    HRESULT hr;

    if (!m_Building)
        return E_UNEXPECTED;

    if (poa)
    {
        hr = poa->GetCount(&Count);
        if (FAILED(hr))
            return hr;
        for (i = 0; i < Count; i++)
        {
            CComPtr<IShellLinkW> Link;
            CComPtr<IShellItem> Item;

            if (FAILED(poa->GetAt(i, IID_PPV_ARG(IShellLinkW, &Link))) &&
                FAILED(poa->GetAt(i, IID_PPV_ARG(IShellItem, &Item))))
            {
                return E_INVALIDARG;
            }
        }
    }

    hr = m_Pending->Seek(Zero, STREAM_SEEK_CUR, &Start);
    if (FAILED(hr))
        return hr;

    hr = DestListWriteDword(m_Pending, Type);
    if (SUCCEEDED(hr) && Type == DESTLIST_SECTION_CUSTOM)
    {
        DWORD Length = (DWORD)wcslen(Name);
        hr = DestListWriteDword(m_Pending, Length);
        if (SUCCEEDED(hr))
            hr = DestListWrite(m_Pending, Name, Length * sizeof(WCHAR));
    }
    else if (SUCCEEDED(hr) && Type == DESTLIST_SECTION_KNOWN)
    {
        hr = DestListWriteDword(m_Pending, Category);
    }
    if (SUCCEEDED(hr))
        hr = DestListWriteDword(m_Pending, Count);

    for (i = 0; SUCCEEDED(hr) && i < Count; i++)
    {
        CComPtr<IShellLinkW> Link;
        CComPtr<IPersistStream> Persist;
        CComPtr<IShellItem> Item;
        CComHeapPtr<ITEMIDLIST> Pidl;

        if (SUCCEEDED(poa->GetAt(i, IID_PPV_ARG(IShellLinkW, &Link))) &&
            SUCCEEDED(Link->QueryInterface(IID_PPV_ARG(IPersistStream, &Persist))))
        {
            hr = DestListWriteDword(m_Pending, DESTLIST_ITEM_LINK);
            if (SUCCEEDED(hr))
                hr = Persist->Save(m_Pending, FALSE);
        }
        else
        {
            hr = poa->GetAt(i, IID_PPV_ARG(IShellItem, &Item));
            if (SUCCEEDED(hr))
                hr = SHGetIDListFromObject(Item, &Pidl);
            if (SUCCEEDED(hr))
                hr = DestListWriteDword(m_Pending, DESTLIST_ITEM_IDLIST);
            if (SUCCEEDED(hr))
                hr = ILSaveToStream(m_Pending, Pidl);
        }
    }

    if (FAILED(hr))
    {
        LARGE_INTEGER Back;

        Back.QuadPart = (LONGLONG)Start.QuadPart;
        m_Pending->Seek(Back, STREAM_SEEK_SET, NULL);
        m_Pending->SetSize(Start);
        return hr;
    }

    m_Sections++;
    return S_OK;
}

/* Reader for the taskbar. Bounded, so a damaged file cannot make it read
   forever or allocate without limit; a damaged tail is dropped. */
#define DESTLIST_MAX_SECTIONS 64
#define DESTLIST_MAX_ITEMS    100

static HRESULT
DestListRead(IStream *Stream, void *Data, ULONG Size)
{
    ULONG Read = 0;
    HRESULT hr = Stream->Read(Data, Size, &Read);

    if (SUCCEEDED(hr) && Read != Size)
        hr = STG_E_READFAULT;
    return hr;
}

EXTERN_C VOID
SHELL_FreeCustomDestinations(PJUMPLIST_SECTION pSections, UINT cSections)
{
    if (!pSections)
        return;
    for (UINT i = 0; i < cSections; i++)
    {
        CoTaskMemFree(pSections[i].pszName);
        if (pSections[i].Items)
            pSections[i].Items->Release();
    }
    CoTaskMemFree(pSections);
}

EXTERN_C HRESULT
SHELL_LoadCustomDestinations(LPCWSTR pszAppID, PJUMPLIST_SECTION *ppSections, UINT *pcSections)
{
    CComPtr<IStream> File;
    PJUMPLIST_SECTION Sections;
    CStringW Path;
    DWORD Magic, Version, Count, i;
    UINT Loaded = 0;
    HRESULT hr;

    if (!ppSections || !pcSections)
        return E_POINTER;
    *ppSections = NULL;
    *pcSections = 0;
    if (!pszAppID || !*pszAppID)
        return E_INVALIDARG;

    hr = JumpListPath(pszAppID, L"CustomDestinations", L"customDestinations-ms", FALSE, Path);
    if (SUCCEEDED(hr))
        hr = SHCreateStreamOnFileEx(Path, STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE, NULL, &File);
    if (FAILED(hr))
        return (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) ? S_FALSE : hr;

    if (FAILED(DestListRead(File, &Magic, sizeof(Magic))) || Magic != DESTLIST_MAGIC ||
        FAILED(DestListRead(File, &Version, sizeof(Version))) || Version != DESTLIST_VERSION ||
        FAILED(DestListRead(File, &Count, sizeof(Count))))
    {
        return HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
    }
    if (Count > DESTLIST_MAX_SECTIONS)
        Count = DESTLIST_MAX_SECTIONS;
    Sections = (PJUMPLIST_SECTION)CoTaskMemAlloc((Count ? Count : 1) * sizeof(*Sections));
    if (!Sections)
        return E_OUTOFMEMORY;
    ZeroMemory(Sections, (Count ? Count : 1) * sizeof(*Sections));

    for (i = 0; i < Count; i++)
    {
        PJUMPLIST_SECTION Section = &Sections[Loaded];
        CComPtr<IObjectCollection> Items;
        DWORD Type, Value, ItemCount, j;

        if (FAILED(DestListRead(File, &Type, sizeof(Type))))
            break;
        Section->Type = Type;
        if (Type == DESTLIST_SECTION_CUSTOM)
        {
            if (FAILED(DestListRead(File, &Value, sizeof(Value))) || Value == 0 || Value >= MAX_PATH)
                break;
            Section->pszName = (LPWSTR)CoTaskMemAlloc((Value + 1) * sizeof(WCHAR));
            if (!Section->pszName || FAILED(DestListRead(File, Section->pszName, Value * sizeof(WCHAR))))
                break;
            Section->pszName[Value] = UNICODE_NULL;
        }
        else if (Type == DESTLIST_SECTION_KNOWN)
        {
            if (FAILED(DestListRead(File, &Section->Category, sizeof(Section->Category))))
                break;
        }
        else if (Type != DESTLIST_SECTION_TASKS)
        {
            break;
        }

        if (FAILED(DestListRead(File, &ItemCount, sizeof(ItemCount))) || ItemCount > DESTLIST_MAX_ITEMS)
            break;
        if (ItemCount && FAILED(ShellObjectCreator<CEnumerableObjectCollection>(IID_PPV_ARG(IObjectCollection, &Items))))
            break;
        for (j = 0; j < ItemCount; j++)
        {
            CComPtr<IUnknown> Object;
            DWORD Kind;

            if (FAILED(DestListRead(File, &Kind, sizeof(Kind))))
                break;
            if (Kind == DESTLIST_ITEM_LINK)
            {
                CComPtr<IPersistStream> Persist;
                if (FAILED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARG(IPersistStream, &Persist))) ||
                    FAILED(Persist->Load(File)))
                {
                    break;
                }
                Object = Persist;
            }
            else if (Kind == DESTLIST_ITEM_IDLIST)
            {
                LPITEMIDLIST Pidl = NULL;
                CComPtr<IShellItem> Item;
                hr = ILLoadFromStream(File, &Pidl);
                if (FAILED(hr) || !Pidl)
                {
                    ILFree(Pidl);
                    break;
                }
                hr = SHCreateItemFromIDList((PCIDLIST_ABSOLUTE)Pidl, IID_PPV_ARG(IShellItem, &Item));
                ILFree(Pidl);
                if (FAILED(hr))
                    continue; /* the stream is still in step */
                Object = Item;
            }
            else
            {
                break;
            }
            if (FAILED(Items->AddObject(Object)))
                break;
        }
        if (j < ItemCount)
        {
            /* A damaged item: keep what came before this section. */
            CoTaskMemFree(Section->pszName);
            Section->pszName = NULL;
            break;
        }
        if (Items)
            Items->QueryInterface(IID_PPV_ARG(IObjectArray, &Section->Items));
        Loaded++;
    }
    /* A section that failed part-way may hold a name. */
    if (Loaded < Count)
    {
        CoTaskMemFree(Sections[Loaded].pszName);
        Sections[Loaded].pszName = NULL;
    }

    if (!Loaded)
    {
        SHELL_FreeCustomDestinations(Sections, 0);
        return S_FALSE;
    }
    *ppSections = Sections;
    *pcSections = Loaded;
    return S_OK;
}

STDMETHODIMP
CDestinationList::SetAppID(LPCWSTR pszAppID)
{
    if (!pszAppID || !*pszAppID)
        return E_INVALIDARG;

    CComCritSecLock<CComAutoCriticalSection> lock(m_Lock);
    if (m_Building)
        return E_UNEXPECTED;
    m_AppId = pszAppID;
    return S_OK;
}

STDMETHODIMP
CDestinationList::BeginList(UINT *pcMinSlots, REFIID riid, void **ppv)
{
    DWORD Slots = DESTLIST_DEFAULT_SLOTS, Size = sizeof(Slots);
    CComPtr<IStream> Pending;
    HRESULT hr;

    if (ppv)
        *ppv = NULL;
    if (!pcMinSlots || !ppv)
        return E_POINTER;

    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                     L"JumpListItems_Maximum", RRF_RT_REG_DWORD, NULL, &Slots, &Size) != ERROR_SUCCESS)
    {
        Slots = DESTLIST_DEFAULT_SLOTS;
    }

    hr = CreateStreamOnHGlobal(NULL, TRUE, &Pending);
    if (FAILED(hr))
        return hr;
    hr = DestListCreateEmpty(riid, ppv);
    if (FAILED(hr))
        return hr;

    CComCritSecLock<CComAutoCriticalSection> lock(m_Lock);
    m_Pending = Pending;
    m_Sections = 0;
    m_Building = TRUE;
    *pcMinSlots = Slots;
    return S_OK;
}

STDMETHODIMP
CDestinationList::AppendCategory(LPCWSTR pszCategory, IObjectArray *poa)
{
    if (!pszCategory || !*pszCategory || !poa)
        return E_INVALIDARG;

    CComCritSecLock<CComAutoCriticalSection> lock(m_Lock);
    return WriteSection(DESTLIST_SECTION_CUSTOM, pszCategory, 0, poa);
}

STDMETHODIMP
CDestinationList::AppendKnownCategory(KNOWNDESTCATEGORY category)
{
    if (category != KDC_FREQUENT && category != KDC_RECENT)
        return E_INVALIDARG;

    CComCritSecLock<CComAutoCriticalSection> lock(m_Lock);
    return WriteSection(DESTLIST_SECTION_KNOWN, NULL, category, NULL);
}

STDMETHODIMP
CDestinationList::AddUserTasks(IObjectArray *poa)
{
    if (!poa)
        return E_INVALIDARG;

    CComCritSecLock<CComAutoCriticalSection> lock(m_Lock);
    return WriteSection(DESTLIST_SECTION_TASKS, NULL, 0, poa);
}

STDMETHODIMP
CDestinationList::CommitList()
{
    CComPtr<IStream> File;
    ULARGE_INTEGER All;
    LARGE_INTEGER Zero = { { 0, 0 } };
    CStringW Path;
    HRESULT hr;

    CComCritSecLock<CComAutoCriticalSection> lock(m_Lock);
    if (!m_Building)
        return E_UNEXPECTED;

    hr = ListPath(NULL, TRUE, Path);
    if (SUCCEEDED(hr))
        hr = SHCreateStreamOnFileEx(Path, STGM_CREATE | STGM_WRITE | STGM_SHARE_DENY_WRITE,
                                    FILE_ATTRIBUTE_NORMAL, TRUE, NULL, &File);
    if (SUCCEEDED(hr))
        hr = DestListWriteDword(File, DESTLIST_MAGIC);
    if (SUCCEEDED(hr))
        hr = DestListWriteDword(File, DESTLIST_VERSION);
    if (SUCCEEDED(hr))
        hr = DestListWriteDword(File, m_Sections);
    if (SUCCEEDED(hr))
        hr = m_Pending->Seek(Zero, STREAM_SEEK_SET, NULL);
    if (SUCCEEDED(hr))
    {
        All.QuadPart = ~0ULL;
        hr = m_Pending->CopyTo(File, All, NULL, NULL);
    }
    if (SUCCEEDED(hr))
        hr = File->Commit(STGC_DEFAULT);
    if (FAILED(hr))
        return hr;

    m_Pending.Release();
    m_Sections = 0;
    m_Building = FALSE;
    return S_OK;
}

STDMETHODIMP
CDestinationList::GetRemovedDestinations(REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = NULL;
    return DestListCreateEmpty(riid, ppv);
}

STDMETHODIMP
CDestinationList::DeleteList(LPCWSTR pszAppID)
{
    CStringW Path;
    HRESULT hr;

    CComCritSecLock<CComAutoCriticalSection> lock(m_Lock);
    hr = ListPath(pszAppID, FALSE, Path);
    if (FAILED(hr))
        return hr;
    if (!DeleteFileW(Path) &&
        GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

STDMETHODIMP
CDestinationList::AbortList()
{
    CComCritSecLock<CComAutoCriticalSection> lock(m_Lock);

    m_Pending.Release();
    m_Sections = 0;
    m_Building = FALSE;
    return S_OK;
}
