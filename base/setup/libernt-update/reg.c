/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     BootExecute registration for the bootstrap (online and offline hive)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "bootstrap.h"

#define BOOTEXECUTE_VALUE L"BootExecute"

/* Opens the Session Manager key for read or write. Online opens
 * CurrentControlSet; offline loads the target SYSTEM hive and resolves the
 * control set through Select\Current, then unloads on close. */
typedef struct _BS_REG_SESSION
{
    BS_TARGET *Target;
    HKEY Machine; /* HKEY_LOCAL_MACHINE or the loaded hive root */
    HKEY SmKey;
    BOOL Loaded;
} BS_REG_SESSION;

static VOID
BsRegCloseSm(
    _Inout_ BS_REG_SESSION *Session);

static DWORD
BsRegOpenSm(
    _Inout_ BS_REG_SESSION *Session,
    _In_ BS_TARGET *Target,
    _In_ REGSAM Sam)
{
    WCHAR HivePath[BS_MAX_PATH_BUF];
    WCHAR ControlSet[64];
    DWORD Current = 1, Size;
    LONG Ret;
    DWORD Error = ERROR_SUCCESS;

    memset(Session, 0, sizeof(*Session));
    Session->Target = Target;

    if (!Target->IsImage)
    {
        Session->Machine = HKEY_LOCAL_MACHINE;
        wcscpy(ControlSet, L"SYSTEM\\CurrentControlSet");
    }
    else
    {
        Error = BsEnablePrivilege(SE_RESTORE_NAME);
        if (Error != ERROR_SUCCESS)
            return Error;
        Error = BsEnablePrivilege(SE_BACKUP_NAME);
        if (Error != ERROR_SUCCESS)
            return Error;
        if (swprintf(HivePath, RTL_NUMBER_OF(HivePath),
                     L"%s\\Windows\\System32\\config\\SYSTEM", Target->Root) < 0)
            return ERROR_FILENAME_EXCED_RANGE;
        Ret = RegLoadKeyW(HKEY_LOCAL_MACHINE, BS_LOADED_HIVE_NAME, HivePath);
        if (Ret != ERROR_SUCCESS)
            return Ret;
        Session->Loaded = TRUE;
        Session->Machine = NULL;

        /* Single exit below: every failure after RegLoadKeyW unloads the
         * hive, so an error never leaves the file locked for the next run
         * (Pam review B5). An unreadable Select\\Current or a missing
         * control set is an error, not a silent default. */
        Ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                            L"LiberNTUpdateBootstrap\\Select", 0, KEY_READ,
                            &Session->Machine);
        if (Ret != ERROR_SUCCESS)
        {
            Error = Ret;
            goto unload;
        }
        Size = sizeof(Current);
        Ret = RegQueryValueExW(Session->Machine, L"Current", NULL, NULL,
                               (BYTE *)&Current, &Size);
        RegCloseKey(Session->Machine);
        Session->Machine = NULL;
        if (Ret != ERROR_SUCCESS)
        {
            Error = Ret;
            goto unload;
        }
        swprintf(ControlSet, RTL_NUMBER_OF(ControlSet),
                 L"LiberNTUpdateBootstrap\\ControlSet%03lu", (unsigned long)Current);
    }

    if (swprintf(HivePath, RTL_NUMBER_OF(HivePath),
                 L"%s\\Control\\Session Manager", ControlSet) < 0)
    {
        Error = ERROR_FILENAME_EXCED_RANGE;
        goto unload;
    }
    Ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE, HivePath, 0, Sam, &Session->SmKey);
    if (Ret != ERROR_SUCCESS)
    {
        Error = Ret;
        goto unload;
    }
    return ERROR_SUCCESS;

unload:
    BsRegCloseSm(Session);
    return Error;
}

static VOID
BsRegCloseSm(
    _Inout_ BS_REG_SESSION *Session)
{
    if (Session->SmKey)
        RegCloseKey(Session->SmKey);
    if (Session->Loaded)
        RegUnLoadKeyW(HKEY_LOCAL_MACHINE, BS_LOADED_HIVE_NAME);
    memset(Session, 0, sizeof(*Session));
}

DWORD
BsBootExecuteRead(
    _In_ const BS_TARGET *Target,
    _Out_ WCHAR *Value,
    _In_ size_t Cch,
    _Out_ ULONG *Type)
{
    BS_REG_SESSION Session;
    DWORD Error, Size;
    ULONG ActualType;
    LONG Ret;

    Error = BsRegOpenSm(&Session, (BS_TARGET *)Target, KEY_READ);
    if (Error != ERROR_SUCCESS)
        return Error;
    Size = (DWORD)(Cch * sizeof(WCHAR));
    Ret = RegQueryValueExW(Session.SmKey, BOOTEXECUTE_VALUE, NULL,
                           &ActualType, (BYTE *)Value, &Size);
    if (Ret == ERROR_FILE_NOT_FOUND)
    {
        Value[0] = 0;
        Value[1] = 0;
        *Type = REG_MULTI_SZ;
        BsRegCloseSm(&Session);
        return ERROR_SUCCESS;
    }
    if (Ret != ERROR_SUCCESS)
    {
        BsRegCloseSm(&Session);
        return Ret;
    }
    if (ActualType != REG_MULTI_SZ)
    {
        BsRegCloseSm(&Session);
        return ERROR_BAD_FORMAT;
    }
    if (Size + sizeof(WCHAR) > Cch * sizeof(WCHAR))
    {
        BsRegCloseSm(&Session);
        return ERROR_MORE_DATA;
    }
    Value[Size / sizeof(WCHAR)] = 0;
    *Type = ActualType;
    BsRegCloseSm(&Session);
    return ERROR_SUCCESS;
}

static DWORD
BsBootExecuteWrite(
    _In_ const BS_TARGET *Target,
    _In_ const WCHAR *Value,
    _In_ SIZE_T Chars)
{
    BS_REG_SESSION Session;
    DWORD Error;
    LONG Ret;

    Error = BsRegOpenSm(&Session, (BS_TARGET *)Target, KEY_SET_VALUE);
    if (Error != ERROR_SUCCESS)
        return Error;
    Ret = RegSetValueExW(Session.SmKey, BOOTEXECUTE_VALUE, 0, REG_MULTI_SZ,
                         (const BYTE *)Value, (DWORD)(Chars * sizeof(WCHAR)));
    if (Ret == ERROR_SUCCESS && Session.Loaded)
        RegFlushKey(Session.SmKey);
    BsRegCloseSm(&Session);
    return Ret;
}

DWORD
BsBootExecuteAdd(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR App)
{
    WCHAR Current[1024], Updated[1024];
    ULONG Type;
    SIZE_T NewChars;
    DWORD Error;

    Error = BsBootExecuteRead(Target, Current, RTL_NUMBER_OF(Current), &Type);
    if (Error != ERROR_SUCCESS)
        return Error;
    NewChars = BsMultiSzAdd(Updated, RTL_NUMBER_OF(Updated), Current, App);
    if (NewChars == 0)
        return ERROR_INSUFFICIENT_BUFFER;
    return BsBootExecuteWrite(Target, Updated, NewChars);
}

DWORD
BsBootExecuteRemove(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR App)
{
    WCHAR Current[1024], Updated[1024];
    ULONG Type;
    SIZE_T NewChars;
    DWORD Error;

    Error = BsBootExecuteRead(Target, Current, RTL_NUMBER_OF(Current), &Type);
    if (Error != ERROR_SUCCESS)
        return Error;
    NewChars = BsMultiSzRemove(Updated, RTL_NUMBER_OF(Updated), Current, App);
    if (NewChars == 0)
        return ERROR_INSUFFICIENT_BUFFER;
    return BsBootExecuteWrite(Target, Updated, NewChars);
}
