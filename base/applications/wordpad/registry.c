/*
 * Wordpad implementation - Registry functions
 *
 * Copyright 2007 by Alexander N. Sørnes <alex@thehandofagony.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <windows.h>
#include <shlobj.h>
#include <richedit.h>

#include "wordpad.h"

static WCHAR filelist_paths[FILELIST_ENTRIES][MAX_PATH];
static DWORD ruler_unit_value = UNIT_CM;

static LRESULT registry_get_handle(HKEY *hKey, LPDWORD action, LPCWSTR subKey)
{
    LONG ret;
    LPWSTR key = calloc(lstrlenW(L"Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\Wordpad\\")+lstrlenW(subKey)+1, sizeof(WCHAR));

    if(!key) return 1;

    lstrcpyW(key, L"Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\Wordpad\\");
    lstrcatW(key, subKey);

    if(action)
    {
        ret = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, REG_OPTION_NON_VOLATILE,
                              KEY_READ | KEY_WRITE, NULL, hKey, action);
    } else
    {
        ret = RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ | KEY_WRITE, hKey);
    }

    free(key);
    return ret;
}

void registry_set_options(HWND hMainWnd)
{
    HKEY hKey = 0;
    DWORD action;

    if(registry_get_handle(&hKey, &action, L"Options") == ERROR_SUCCESS)
    {
        WINDOWPLACEMENT wp;
        DWORD isMaximized;

        wp.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(hMainWnd, &wp);
        isMaximized = (wp.showCmd == SW_SHOWMAXIMIZED);

        RegSetValueExW(hKey, L"FrameRect", 0, REG_BINARY, (LPBYTE)&wp.rcNormalPosition, sizeof(RECT));
        RegSetValueExW(hKey, L"Maximized", 0, REG_DWORD, (LPBYTE)&isMaximized, sizeof(DWORD));

        registry_set_pagemargins(hKey);
        RegCloseKey(hKey);
    }
}

void registry_read_winrect(RECT* rc)
{
    HKEY hKey = 0;
    DWORD size = sizeof(RECT);
    BOOL loaded = TRUE;

    if(registry_get_handle(&hKey, 0, L"Options") != ERROR_SUCCESS ||
       RegQueryValueExW(hKey, L"FrameRect", 0, NULL, (LPBYTE)rc, &size) !=
       ERROR_SUCCESS || size != sizeof(RECT))
        loaded = FALSE;

    RegCloseKey(hKey);

    if(!loaded)
    {
        RECT work;
        int w, h;

        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        w = (work.right - work.left) * 2 / 3;
        h = (work.bottom - work.top) * 2 / 3;
        SetRect(rc, work.left + ((work.right - work.left) - w) / 2,
                     work.top + ((work.bottom - work.top) - h) / 2,
                     work.left + ((work.right - work.left) - w) / 2 + w,
                     work.top + ((work.bottom - work.top) - h) / 2 + h);
    } else
    {
        if(rc->right - rc->left < 800)
            rc->right = rc->left + 800;
        if(rc->bottom - rc->top < 600)
            rc->bottom = rc->top + 600;
    }
}

void registry_read_maximized(DWORD *bMaximized)
{
    HKEY hKey = 0;
    DWORD size = sizeof(DWORD);

    if(registry_get_handle(&hKey, 0, L"Options") != ERROR_SUCCESS ||
       RegQueryValueExW(hKey, L"Maximized", 0, NULL, (LPBYTE)bMaximized, &size) !=
       ERROR_SUCCESS || size != sizeof(DWORD))
    {
        *bMaximized = FALSE;
    }

    RegCloseKey(hKey);
}

DWORD registry_get_ruler_unit(void)
{
    return ruler_unit_value;
}

void registry_set_ruler_unit(DWORD unit)
{
    HKEY hKey;
    DWORD action;

    ruler_unit_value = unit;
    if(registry_get_handle(&hKey, &action, L"Options") == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, L"RulerUnit", 0, REG_DWORD, (const BYTE*)&unit, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}


void registry_read_filelist(HWND hMainWnd)
{
    HKEY hFileKey;

    (void)hMainWnd;

    ZeroMemory(filelist_paths, sizeof(filelist_paths));

    if(registry_get_handle(&hFileKey, 0, L"Recent file list") == ERROR_SUCCESS)
    {
        DWORD pathSize;
        int i;
        WCHAR key[6];

        for(i = 0; i < FILELIST_ENTRIES; i++)
        {
            wsprintfW(key, L"File%d", i+1);
            pathSize = MAX_PATH*sizeof(WCHAR);
            if(RegQueryValueExW(hFileKey, (LPWSTR)key, 0, NULL, (LPBYTE)filelist_paths[i], &pathSize)
               != ERROR_SUCCESS)
                break;
        }

        RegCloseKey(hFileKey);
    }
}

LPCWSTR registry_get_recent_file(int index)
{
    if(index < 0 || index >= FILELIST_ENTRIES)
        return NULL;
    return filelist_paths[index];
}

void registry_set_filelist(LPCWSTR newFile, HWND hMainWnd)
{
    HKEY hKey;
    DWORD action;

    if(registry_get_handle(&hKey, &action, L"Recent file list") == ERROR_SUCCESS)
    {
        WCHAR pFiles[FILELIST_ENTRIES][MAX_PATH];
        int i;
        WCHAR buffer[6];

        for(i = 0; i < FILELIST_ENTRIES; i++)
            pFiles[i][0] = L'\0';

        registry_read_filelist(hMainWnd);
        for(i = 0; i < FILELIST_ENTRIES; i++)
            lstrcpynW(pFiles[i], filelist_paths[i], MAX_PATH);

        if(lstrcmpiW(newFile, pFiles[0]))
        {
            for(i = 0; i < FILELIST_ENTRIES && pFiles[i][0]; i++)
            {
                if(!lstrcmpiW(pFiles[i], newFile))
                {
                    int j;
                    for(j = 0; j < i; j++)
                        lstrcpynW(pFiles[i-j], pFiles[i-j-1], MAX_PATH);
                    pFiles[0][0] = L'\0';
                    break;
                }
            }

            if(!pFiles[0][0])
            {
                lstrcpynW(pFiles[0], newFile, MAX_PATH);
            } else
            {
                for(i = 0; i < FILELIST_ENTRIES-1; i++)
                    lstrcpynW(pFiles[FILELIST_ENTRIES-1-i], pFiles[FILELIST_ENTRIES-2-i], MAX_PATH);

                lstrcpynW(pFiles[0], newFile, MAX_PATH);
            }

            for(i = 0; i < FILELIST_ENTRIES && pFiles[i][0]; i++)
            {
                wsprintfW(buffer, L"File%d", i+1);
                RegSetValueExW(hKey, (LPWSTR)&buffer, 0, REG_SZ, (const BYTE*)pFiles[i],
                               (lstrlenW(pFiles[i])+1)*sizeof(WCHAR));
            }
        }
        RegCloseKey(hKey);
    }
    registry_read_filelist(hMainWnd);
}

int reg_formatindex(WPARAM format)
{
    return (format & SF_TEXT) ? 1 : 0;
}

void registry_read_options(void)
{
    HKEY hKey;

    if(registry_get_handle(&hKey, 0, L"Options") != ERROR_SUCCESS)
        registry_read_pagemargins(NULL);
    else
    {
        DWORD size = sizeof(DWORD);
        registry_read_pagemargins(hKey);
        RegQueryValueExW(hKey, L"RulerUnit", 0, NULL, (LPBYTE)&ruler_unit_value, &size);
        RegCloseKey(hKey);
    }
}

static void registry_read_formatopts(int index, LPCWSTR key, DWORD barState[], DWORD wordWrap[])
{
    HKEY hKey;
    DWORD action = 0;
    BOOL fetched = FALSE;
    barState[index] = 0;
    wordWrap[index] = 0;

    if(registry_get_handle(&hKey, &action, key) != ERROR_SUCCESS)
        return;

    if(action == REG_OPENED_EXISTING_KEY)
    {
        DWORD size = sizeof(DWORD);

        if(RegQueryValueExW(hKey, L"BarState0", 0, NULL, (LPBYTE)&barState[index],
           &size) == ERROR_SUCCESS)
            fetched = TRUE;
    }

    if(!fetched)
        barState[index] = (1 << BANDID_TOOLBAR) | (1 << BANDID_FORMATBAR) | (1 << BANDID_RULER) | (1 << BANDID_STATUSBAR);

    fetched = FALSE;
    if(action == REG_OPENED_EXISTING_KEY)
    {
        DWORD size = sizeof(DWORD);
        if(RegQueryValueExW(hKey, L"Wrap", 0, NULL, (LPBYTE)&wordWrap[index],
           &size) == ERROR_SUCCESS)
            fetched = TRUE;
    }

    if (!fetched)
    {
        if(index == reg_formatindex(SF_RTF))
            wordWrap[index] = ID_WORDWRAP_WINDOW;
        else if(index == reg_formatindex(SF_TEXT))
            wordWrap[index] = ID_WORDWRAP_NONE;
    }

    RegCloseKey(hKey);
}

void registry_read_formatopts_all(DWORD barState[], DWORD wordWrap[])
{
    registry_read_formatopts(reg_formatindex(SF_RTF), L"RTF", barState, wordWrap);
    registry_read_formatopts(reg_formatindex(SF_TEXT), L"Text", barState, wordWrap);
}

static void registry_set_formatopts(int index, LPCWSTR key, DWORD barState[], DWORD wordWrap[])
{
    HKEY hKey;
    DWORD action = 0;

    if(registry_get_handle(&hKey, &action, key) == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, L"BarState0", 0, REG_DWORD, (LPBYTE)&barState[index],
                       sizeof(DWORD));
        RegSetValueExW(hKey, L"Wrap", 0, REG_DWORD, (LPBYTE)&wordWrap[index],
                       sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

void registry_set_formatopts_all(DWORD barState[], DWORD wordWrap[])
{
    registry_set_formatopts(reg_formatindex(SF_RTF), L"RTF", barState, wordWrap);
    registry_set_formatopts(reg_formatindex(SF_TEXT), L"Text", barState, wordWrap);
}
