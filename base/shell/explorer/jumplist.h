/*
 * PROJECT:     LiberNT Explorer
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Jump list section of the task button menu
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

/* Menu command ids owned by the jump list (taskswnd uses small ids). */
#define IDM_JUMPLIST_FIRST 0x1000
#define IDM_JUMPLIST_LAST  0x1FFF

/*
 * One jump list menu, for one button and one TrackPopupMenu:
 *   Build()      insert Pinned, the app's categories (custom, Recent, Frequent)
 *                and Tasks at the top of the menu, from the windows.storage
 *                store of the app's AppUserModelID (or exe path).
 *   BeginTrack() / EndTrack() around the menu: a right click on a document
 *                closes the menu and offers Open / Pin / Unpin / Remove.
 *   Invoke()     after the menu: TRUE when the command was the jump list's.
 */
class CTaskJumpList
{
public:
    CTaskJumpList();
    ~CTaskJumpList();

    UINT Build(HMENU hMenu, LPCWSTR pszAppId, LPCWSTR pszExePath);
    VOID BeginTrack(HMENU hMenu);
    VOID EndTrack();
    BOOL Invoke(INT Cmd, HWND hwndOwner);

private:
    enum SOURCE { SRC_PINNED, SRC_AUTO, SRC_CUSTOM, SRC_TASK };
    struct ENTRY
    {
        IUnknown *Object; /* IShellLinkW or IShellItem */
        SOURCE Source;
        BOOL bDocument;   /* an IShellItem: can be pinned */
    };

    CSimpleArray<ENTRY> m_Entries;
    CStringW m_Key;
    CStringW m_ExePath;
    BOOL m_bExplicit;
    HMENU m_hMenu;
    HHOOK m_hHook;
    INT m_ContextCmd;
    UINT m_Pos;

    static CTaskJumpList *s_Tracking;
    static LRESULT CALLBACK MsgFilterProc(INT Code, WPARAM wParam, LPARAM lParam);

    VOID InsertHeader(LPCWSTR pszText);
    VOID InsertSeparator();
    BOOL InsertObject(IUnknown *Object, SOURCE Source);
    VOID InsertArray(IObjectArray *Array, SOURCE Source, LPCWSTR pszHeader, UINT cMax);
    VOID InsertAuto(DWORD dwList, LPCWSTR pszHeader, UINT cMax);
    ENTRY *EntryFromCmd(INT Cmd);
    VOID Launch(ENTRY *Entry);
    VOID ShowItemMenu(ENTRY *Entry, HWND hwndOwner);
};
