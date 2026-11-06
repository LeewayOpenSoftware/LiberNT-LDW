/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             base/services/umpnpmgr/install.c
 * PURPOSE:          Device installer
 * PROGRAMMER:       Eric Kohl (eric.kohl@reactos.org)
 *                   Hervé Poussineau (hpoussin@reactos.org)
 *                   Colin Finck (colin@reactos.org)
 */

#ifndef _UMPNPMGR_PCH_
#define _UMPNPMGR_PCH_

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winsvc.h>
#include <winuser.h>
#include <dbt.h>
#include <stdio.h>
#include <cmfuncs.h>
#include <rtlfuncs.h>
#include <setypes.h>
#include <umpnpmgr/sysguid.h>
#include <wdmguid.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <regstr.h>
#include <userenv.h>
#include <shlwapi.h>
#include <winsvc_undoc.h>
#include <pnp_s.h>
#include <plugplay_s.h>
#include <drvinst_s.h>
#include <dll/newdevp.h>
#include <umpnpmgr/drvinst.h>


#define LOGCONF_NAME_BUFFER_SIZE 32

typedef struct
{
    LIST_ENTRY ListEntry;
    WCHAR DeviceIds[ANYSIZE_ARRAY];
} DeviceInstallParams;

typedef enum
{
    CLASS_NOTIFICATION = 1,
    TARGET_NOTIFICATION
} NOTIFICATION_TYPE;


typedef struct
{
    LIST_ENTRY ListEntry;
    NOTIFICATION_TYPE dwType;
    PWSTR pszName;
    DWORD_PTR hRecipient;
    DWORD ulFlags;
    GUID ClassGuid;
} NOTIFY_ENTRY, *PNOTIFY_ENTRY;

/* event.c */

DWORD
WINAPI
PnpEventThread(
    LPVOID lpParameter);


/* listener.c */

VOID
InitializeListeners(VOID);

VOID
BroadcastListenerEvent(
    _In_ DWORD Code,
    _In_ PCWSTR Path,
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ UINT Size);


/* install.c */

extern HANDLE hUserToken;
extern HANDLE hInstallEvent;
extern HANDLE hNoPendingInstalls;
extern HANDLE hBootInstallComplete;

/* Device-install event list */
extern HANDLE hDeviceInstallListMutex;
extern LIST_ENTRY DeviceInstallListHead;
extern HANDLE hDeviceInstallListNotEmpty;
extern LIST_ENTRY DoNotInstallListHead;

VOID
SetDeviceDoNotInstall(
    _In_ PCWSTR DeviceId,
    _In_ BOOL DoNotInstall);

BOOL
IsDeviceDoNotInstall(
    _In_ PCWSTR DeviceId);

BOOL
SetupIsActive(VOID);

DWORD
WINAPI
DeviceInstallThread(
    LPVOID lpParameter);


/* drvinst/job.c */

VOID
DrvInstInitialize(VOID);

BOOL
IsValidDeviceInstanceId(
    _In_ PCWSTR DeviceInstanceId);

BOOL
IsValidInfPath(
    _In_ PCWSTR InfPath);

BOOL
DrvInstUseWorker(VOID);

BOOL
DrvInstRunDeviceJob(
    _In_ PCWSTR DeviceInstanceId,
    _In_ ULONG Source,
    _In_ ULONG Flags,
    _Out_opt_ PDRVINST_JOB_ID OutJobId);

BOOL
DrvInstRunInfJob(
    _In_ PCWSTR DeviceInstanceId,
    _In_ PCWSTR InfPath,
    _In_ ULONG Source,
    _In_ ULONG Flags,
    _Out_opt_ PDRVINST_JOB_ID OutJobId);

DWORD
DrvInstSubmitDeviceJob(
    _In_ PCWSTR DeviceInstanceId,
    _In_ ULONG Source,
    _In_ ULONG Flags,
    _Out_ PDRVINST_JOB_ID OutJobId);

DWORD
DrvInstSubmitInfJob(
    _In_ PCWSTR DeviceInstanceId,
    _In_ PCWSTR InfPath,
    _In_ ULONG Source,
    _In_ ULONG Flags,
    _Out_ PDRVINST_JOB_ID OutJobId);

BOOL
DrvInstGetJobInfo(
    _In_ DRVINST_JOB_ID JobId,
    _Out_ PDRVINST_JOB_INFO Info);

ULONG
DrvInstpEnumJobs(
    _Out_writes_to_(Max, return) PDRVINST_JOB_INFO Info,
    _In_ ULONG Max);


/* drvinst/rollback.c */

VOID
DrvInstRollbackInitialize(VOID);

DWORD
DrvInstSnapshotDevice(
    _In_ PCWSTR DeviceInstanceId,
    _Out_opt_ PULONG OutSeq);

DWORD
DrvInstRollbackDevice(
    _In_ PCWSTR DeviceInstanceId,
    _Out_opt_ PBOOL NeedReboot);

DWORD
DrvInstRollbackDeviceToSeq(
    _In_ PCWSTR DeviceInstanceId,
    _In_ ULONG Seq,
    _Out_opt_ PBOOL NeedReboot);

DWORD
DrvInstAcquireInstallLock(
    _In_ DWORD TimeoutMs);

VOID
DrvInstReleaseInstallLock(VOID);


/* rpcserver.c */

extern LIST_ENTRY NotificationListHead;
extern RTL_RESOURCE NotificationListLock;

DWORD
WINAPI
RpcServerThread(
    LPVOID lpParameter);

BOOL
IsRootDeviceInstanceID(
    _In_ PWSTR pszDeviceInstanceID);


/* umpnpmgr.c */

extern HKEY hEnumKey;
extern HKEY hClassKey;
extern BOOL g_ShuttingDown;
extern BOOL g_IsMiniNT;

#endif /* _UMPNPMGR_PCH_ */
