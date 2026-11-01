/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Internal contracts for the LiberNT Update bootstrap
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <s1core.h>

#define BS_VERSION              L"0.1-dev"

#define BS_SERVICING_DIR        L"LiberNT\\Servicing"
#define BS_STORE_DIR            L"LiberNT\\Servicing\\store"
#define BS_JOURNAL_DIR          L"LiberNT\\Servicing\\journal"
#define BS_PENDING_DIR          L"LiberNT\\Servicing\\pending"
#define BS_BACKUP_DIR           L"LiberNT\\Servicing\\backup"
#define BS_BIN_DIR              L"LiberNT\\Servicing\\bin"
#define BS_LOGS_DIR             L"LiberNT\\Servicing\\logs"
#define BS_META_DIR             L"LiberNT\\Servicing\\bootstrap"
#define BS_INSTALLED_JSON       L"LiberNT\\Servicing\\bootstrap\\installed.json"
#define BS_DEVKEY_FILE          L"LiberNT\\Servicing\\store\\dev-public-key.bin"
#define BS_SYSTEM32_PENDING     L"Windows\\System32\\LiberNTUpdatePending.exe"
#define BS_BOOTEXECUTE_APP      L"LiberNTUpdatePending"
#define BS_SERVICE_NAME         L"LiberNTUpdateSvc"
#define BS_SERVICE_BINARY       L"LiberNT\\Servicing\\bin\\LiberNTUpdateSvc.exe"
#define BS_LOADED_HIVE_NAME     L"LiberNTUpdateBootstrap"

#define BS_MAX_PATH_BUF         520

typedef struct _BS_TARGET
{
    /* Root prefix: L"" online, L"<image root>" offline. */
    WCHAR Root[BS_MAX_PATH_BUF];
    BOOL IsImage;
    /* Fully-qualified image of the boot drive, e.g. L"\\\\.\\C:" (offline only). */
    WCHAR BootVolume[BS_MAX_PATH_BUF];
    HANDLE VolumeLock;
} BS_TARGET;

typedef struct _BS_BUNDLE_FILE
{
    WCHAR Name[64];
    WCHAR Source[BS_MAX_PATH_BUF];
    WCHAR Dest[BS_MAX_PATH_BUF];
    char Sha256[65];
    BOOL Required;
    BOOL IsService;
} BS_BUNDLE_FILE;

/* A Start Menu entry the install materializes and the inventory tracks, so
 * uninstall removes exactly what install created. */
typedef struct _BS_BUNDLE_SHORTCUT
{
    WCHAR Name[64];                 /* .lnk file name, e.g. L"libernt-update-ui.lnk" */
    WCHAR Target[BS_MAX_PATH_BUF];  /* root-relative exe, e.g. L"LiberNT\\Update\\libernt-update-ui.exe" */
    WCHAR Arguments[128];
} BS_BUNDLE_SHORTCUT;

typedef struct _BS_BUNDLE
{
    WCHAR Path[BS_MAX_PATH_BUF];
    BS_BUNDLE_FILE Files[16];
    ULONG FileCount;
    BS_BUNDLE_SHORTCUT Shortcuts[8];
    ULONG ShortcutCount;
    ULONG64 TotalBytes;
    char ManifestDigest[65];
} BS_BUNDLE;

typedef struct _BS_FILE_REC
{
    WCHAR Path[BS_MAX_PATH_BUF];
    char Sha256[65];
} BS_FILE_REC;

typedef struct _BS_INVENTORY
{
    BS_FILE_REC Files[24];
    ULONG FileCount;
    WCHAR ServiceState[16]; /* "installed", "stub", "none" */
    BOOL BootExecute;
    char ManifestDigest[65];
} BS_INVENTORY;

typedef struct _BS_LOG
{
    BS_TARGET *Target;
    WCHAR LogPath[BS_MAX_PATH_BUF];
    BOOL Ready;
} BS_LOG;

/* main.c */
VOID
BsLogInit(
    _Inout_ BS_LOG *Log,
    _In_ BS_TARGET *Target);

VOID
BsLog(
    _Inout_ BS_LOG *Log,
    _In_ PCWSTR Format,
    ...);

VOID
BsPrint(
    _In_ PCWSTR Format,
    ...);

/* bundle.c */
const uint8_t *
BsDevPublicKeyBytes(VOID);

/* Loads the bundle manifest ONCE, verifies the signature over those exact
 * bytes and parses the same buffer (Pam review B1: no second read of a file
 * that could change in between). */
DWORD
BsBundleLoad(
    _Out_ BS_BUNDLE *Bundle,
    _In_ PCWSTR BundlePath);

/* target.c */
DWORD
BsTargetInitOnline(
    _Out_ BS_TARGET *Target);

DWORD
BsTargetInitImage(
    _Out_ BS_TARGET *Target,
    _In_ PCWSTR ImageRoot);

VOID
BsTargetFree(
    _Inout_ BS_TARGET *Target);

DWORD
BsBuildTargetPath(
    _Out_ PWSTR Buffer,
    _In_ size_t Cch,
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR Relative);

DWORD
BsCheckSecure(
    _In_ PCWSTR Path);

/* Accepts only normalized relative paths under the allowed prefixes
 * (Windows\ or LiberNT\) whose existing components resolve to themselves
 * (no reparse points, no 8.3 aliases). Mirrors LnIsSafeRelativePath and
 * LnTargetValidateReal of libservicing/util.c and libservicing/target.c;
 * keep the three in sync. */
DWORD
BsValidateTargetRelPath(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR Relative);

DWORD
BsCreateSecureDir(
    _In_ PCWSTR Path);

DWORD
BsEnsureSecureTree(
    _In_ const BS_TARGET *Target);

/* Validates the destination, copies to a temp name next to it, verifies
 * the hash and renames over the destination: an interruption never leaves
 * a partial file in place. */
DWORD
BsCopyFileToTarget(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR Source,
    _In_ PCWSTR RelativeDest,
    _In_ PCSTR ExpectedSha256);

DWORD
BsHashTargetFile(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR RelativePath,
    _Out_ char *HexOut);

DWORD
BsDeleteTargetFile(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR RelativePath);

BOOL
BsTargetFileExists(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR RelativePath);

DWORD
BsEnablePrivilege(
    _In_ PCWSTR Privilege);

/* reg.c */
DWORD
BsBootExecuteRead(
    _In_ const BS_TARGET *Target,
    _Out_ WCHAR *Value,
    _In_ size_t Cch,
    _Out_ ULONG *Type);

DWORD
BsBootExecuteAdd(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR App);

DWORD
BsBootExecuteRemove(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR App);

/* Pure helper shared with the host tests: edit a REG_MULTI_SZ buffer.
 * Returns the new length in WCHARs (including both nulls), or 0 on overflow. */
SIZE_T
BsMultiSzAdd(
    _Out_ WCHAR *Out,
    _In_ size_t OutCch,
    _In_ const WCHAR *In,
    _In_ PCWSTR App);

SIZE_T
BsMultiSzRemove(
    _Out_ WCHAR *Out,
    _In_ size_t OutCch,
    _In_ const WCHAR *In,
    _In_ PCWSTR App);

/* service.c */
DWORD
BsServiceInstall(
    _In_ const BS_TARGET *Target,
    _In_ BOOL Enable,
    _Out_ BOOL *Installed);

DWORD
BsServiceRemove(
    _In_ const BS_TARGET *Target);

DWORD
BsServiceQuery(
    _In_ BOOL *Exists);

/* selftest.c */
DWORD
BsSelfTest(
    _In_ const BS_TARGET *Target,
    _In_ const BS_BUNDLE *Bundle,
    _In_ const BS_INVENTORY *Inventory,
    _In_ BOOL OnlineService);

/* inventory serialization (main.c) */
DWORD
BsInventorySave(
    _In_ const BS_TARGET *Target,
    _In_ const BS_INVENTORY *Inventory);

DWORD
BsInventoryLoad(
    _In_ const BS_TARGET *Target,
    _Out_ BS_INVENTORY *Inventory);
