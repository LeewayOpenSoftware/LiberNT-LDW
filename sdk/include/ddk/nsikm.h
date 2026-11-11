/*
 * PROJECT:     LiberNT kernel-mode network store (NSI)
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Provider and consumer API of the network store hosted in netio.sys
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * A provider owns the rows of one (module, table) pair and answers requests
 * from its own data under its own locks; the store keeps only the registry,
 * the per-table change sequence and the pending change requests.
 * Row layouts are the ones of the user-mode NSI tables (sdk/include/wine/nsi.h).
 * Enumerate and GetAll are called at the consumer's IRQL, which is
 * PASSIVE_LEVEL for the \Device\Nsi proxy. NsiNotifyChange may be called
 * at or below DISPATCH_LEVEL.
 *
 * A change request is an IRP: NsiRequestChangeNotificationKm marks it
 * pending and queues it, returning STATUS_PENDING; the next NsiNotifyChange
 * on that table completes it with STATUS_SUCCESS and Information set to the
 * table's change sequence, cancellation completes it with STATUS_CANCELLED.
 * The IRP's current stack location must be valid: a dispatch routine passes
 * its IRP as is, a kernel caller that builds the IRP sets its completion
 * routine and then calls IoSetNextIrpStackLocation before requesting.
 *
 * Deregistration: NsiDeregisterProvider unlinks the provider, waits for every
 * call that holds it (consumer requests and NsiNotifyChange alike, the store
 * references the handle under its lock before touching it) and then cancels
 * the requests still queued. A change signalled for a handle that is being or
 * has been deregistered is dropped. A producer must nevertheless stop raising
 * changes before it deregisters: once the call has returned the handle value
 * may belong to a later registration.
 */

#ifndef _NSIKM_
#define _NSIKM_

#pragma once

#include <netiodef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NSI_PARAM_RW      0
#define NSI_PARAM_DYNAMIC 1
#define NSI_PARAM_STATIC  2

typedef enum _NSI_CHANGE_TYPE
{
    NsiChangeAdd = 1,
    NsiChangeDelete,
    NsiChangeParameter,
    NsiChangeBulk
} NSI_CHANGE_TYPE;

typedef struct _NSI_ROW_SIZES
{
    ULONG Key;
    ULONG Rw;
    ULONG Dynamic;
    ULONG Static;
} NSI_ROW_SIZES, *PNSI_ROW_SIZES;

typedef struct _NSI_ENUMERATE_REQUEST
{
    ULONG FirstArg;
    ULONG SecondArg;
    PVOID KeyData;
    PVOID RwData;
    PVOID DynamicData;
    PVOID StaticData;
    ULONG Count;
} NSI_ENUMERATE_REQUEST, *PNSI_ENUMERATE_REQUEST;

typedef
NTSTATUS
(NTAPI *PNSI_PROVIDER_ENUMERATE)(
    _In_opt_ PVOID Context,
    _Inout_ PNSI_ENUMERATE_REQUEST Request);

typedef
NTSTATUS
(NTAPI *PNSI_PROVIDER_GET_ALL)(
    _In_opt_ PVOID Context,
    _In_ ULONG FirstArg,
    _In_reads_bytes_(KeySize) const VOID *Key,
    _In_ ULONG KeySize,
    _Out_writes_bytes_opt_(RwSize) PVOID Rw,
    _Out_writes_bytes_opt_(DynamicSize) PVOID Dynamic,
    _Out_writes_bytes_opt_(StaticSize) PVOID Static);

typedef
NTSTATUS
(NTAPI *PNSI_PROVIDER_GET_PARAMETER)(
    _In_opt_ PVOID Context,
    _In_ ULONG FirstArg,
    _In_reads_bytes_(KeySize) const VOID *Key,
    _In_ ULONG KeySize,
    _In_ ULONG ParamType,
    _In_ ULONG DataOffset,
    _Out_writes_bytes_(DataSize) PVOID Data,
    _In_ ULONG DataSize);

typedef struct _NSI_PROVIDER_DISPATCH
{
    ULONG Size;
    NSI_ROW_SIZES RowSizes;
    PNSI_PROVIDER_ENUMERATE Enumerate;
    PNSI_PROVIDER_GET_ALL GetAll;
    PNSI_PROVIDER_GET_PARAMETER GetParameter;
} NSI_PROVIDER_DISPATCH, *PNSI_PROVIDER_DISPATCH;

typedef struct _NSI_PROVIDER *PNSI_PROVIDER;

NTSTATUS
NTAPI
NsiRegisterProvider(
    _In_ const NPI_MODULEID *Module,
    _In_ ULONG Table,
    _In_ const NSI_PROVIDER_DISPATCH *Dispatch,
    _In_opt_ PVOID Context,
    _Out_ PNSI_PROVIDER *Provider);

VOID
NTAPI
NsiDeregisterProvider(
    _In_ PNSI_PROVIDER Provider);

VOID
NTAPI
NsiNotifyChange(
    _In_ PNSI_PROVIDER Provider,
    _In_ NSI_CHANGE_TYPE Type,
    _In_reads_bytes_opt_(KeySize) const VOID *Key,
    _In_ ULONG KeySize);

NTSTATUS
NTAPI
NsiRequestChangeNotificationKm(
    _In_ const NPI_MODULEID *Module,
    _In_ ULONG Table,
    _In_ PIRP Irp);

VOID
NTAPI
NsiCancelChangeNotificationsKm(
    _In_ PFILE_OBJECT FileObject);

NTSTATUS
NTAPI
NsiQueryTableSizesKm(
    _In_ const NPI_MODULEID *Module,
    _In_ ULONG Table,
    _Out_ PNSI_ROW_SIZES Sizes);

NTSTATUS
NTAPI
NsiEnumerateAllParametersKm(
    _In_ const NPI_MODULEID *Module,
    _In_ ULONG Table,
    _In_ const NSI_ROW_SIZES *Sizes,
    _Inout_ PNSI_ENUMERATE_REQUEST Request);

NTSTATUS
NTAPI
NsiGetAllParametersKm(
    _In_ const NPI_MODULEID *Module,
    _In_ ULONG Table,
    _In_ const NSI_ROW_SIZES *Sizes,
    _In_ ULONG FirstArg,
    _In_reads_bytes_(KeySize) const VOID *Key,
    _In_ ULONG KeySize,
    _Out_writes_bytes_opt_(Sizes->Rw) PVOID Rw,
    _Out_writes_bytes_opt_(Sizes->Dynamic) PVOID Dynamic,
    _Out_writes_bytes_opt_(Sizes->Static) PVOID Static);

NTSTATUS
NTAPI
NsiGetParameterKm(
    _In_ const NPI_MODULEID *Module,
    _In_ ULONG Table,
    _In_ ULONG FirstArg,
    _In_reads_bytes_(KeySize) const VOID *Key,
    _In_ ULONG KeySize,
    _In_ ULONG ParamType,
    _In_ ULONG DataOffset,
    _Out_writes_bytes_(DataSize) PVOID Data,
    _In_ ULONG DataSize);

#ifdef __cplusplus
}
#endif

#endif
