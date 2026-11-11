/*
 * PROJECT:     NetIO driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NT6 compatible NETIO.SYS driver: A BSD sockets like kernel
 *              internal interface for networking (TCP/IP, UDP/IP).
 * COPYRIGHT:   Copyright 2023-2025 Johannes Khoshnazar-Thoma <johannes@johannesthoma.com>
 */

/*
Copyright © 2025 Johannes Khoshnazar-Thoma <johannes@johannesthoma.com>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

/*
 * [NETIO] NETIO.SYS driver
 *
 * This files adds the NETIO.SYS driver to ReactOS. It is not
 * feature complete (meaning some functionality is unimplemented)
 * but does its job quite good for what it originally was written
 * for (which is getting WinDRBD working on ReactOS/Windows 2003 SP2).
 *
 * The driver re-uses parts of the AFD.SYS driver, namely those
 * functions that ease communitating with the transport device
 * interface (TDI). Other than that, following features are implemented
 * and should work:
 *
 *     * TCP/IP networking: connect, listen, accept, read, write
 *     * UDP/IP networking: write
 *
 * So in a nutshell TCP/IP support is completed, UDP support is
 * partially complete and ICMP support does not exist yet.
 * In particular the listen/accept mechanism allows one to write
 * kernel side TCP servers that one can connect to via the internet.
 */

#include <ntifs.h>
#include <netiodef.h>
#include <ws2def.h>
#include <wsk.h>
#include <ndis.h>
#include <pseh/pseh2.h>

/* If you want to see the DPRINT() output in your debugger,
 * remove the following line (or comment it out):
 * Note that DPRINT1() is always printed to the debugger. */
#define NDEBUG
#include <reactos/debug.h>

#include <tdi.h>
#include <tcpioctl.h>
#include <tdikrnl.h>
#include <tdiinfo.h>
#include <tdi_proto.h>
#include <tdiconn.h>

#define TAG_NETIO 'OIEN'
#define NETIO_ACCEPT_BACKLOG 64

#ifndef TAG_AFD_TDI_CONNECTION_INFORMATION
#define TAG_AFD_TDI_CONNECTION_INFORMATION 'cTfA'
#endif

/* AFD Share Flags */
#define AFD_SHARE_UNIQUE    0x0L
#define AFD_SHARE_REUSE     0x1L
#define AFD_SHARE_WILDCARD  0x2L
#define AFD_SHARE_EXCLUSIVE 0x3L

/* Function trace */
// #define FUNCTION_TRACE DPRINT1("%s() ...\n", __FUNCTION__)
#define FUNCTION_TRACE do { } while (0)

#define WSK_REQUIRE_IRP(Irp) \
    do \
    { \
        if ((Irp) == NULL) \
        { \
            DPRINT1("%s: a WSK request needs an IRP, none was given\n", __FUNCTION__); \
            return STATUS_INVALID_PARAMETER; \
        } \
    } while (0)

static NTSTATUS
WskCompleteUnsupported(_In_opt_ PIRP Irp, _In_ PCSTR Function)
{
    DPRINT1("%s is not implemented\n", Function);
    if (Irp != NULL)
    {
        IoSetNextIrpStackLocation(Irp);
        Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
    }
    return STATUS_NOT_IMPLEMENTED;
}

typedef struct _WSK_SOCKET_INTERNAL
{
    WSK_SOCKET Socket;

    ADDRESS_FAMILY family;      /* AF_INET or AF_INET6 */
    USHORT type;                /* SOCK_DGRAM, SOCK_STREAM, ... */
    ULONG proto;                /* IPPROTO_UDP, IPPROTO_TCP */
    ULONG WskFlags;             /* WSK_FLAG_LISTEN_SOCKET, ... */
    PVOID user_context;         /* parameter for callbacks, opaque */
    UNICODE_STRING TdiName;     /* \\Devices\\Tcp, \\Devices\\Udp */

    struct sockaddr LocalAddress;
    PFILE_OBJECT LocalAddressFile;
    HANDLE LocalAddressHandle;
    BOOLEAN LocalAddressKnown;

    struct sockaddr RemoteAddress;      /* Remember latest remote connection */
    PFILE_OBJECT RemoteAddressFile;
    HANDLE RemoteAddressHandle;

    /* Those exist for connection oriented (TCP/IP) sockets only */
    PFILE_OBJECT ConnectionFile;        /* Returned by TdiOpenConnectionEndpointFile() */
    HANDLE ConnectionHandle;            /* Returned by TdiOpenConnectionEndpointFile() */
    BOOLEAN ConnectionFileAssociated;

    /* Incoming connection callback function */
    const WSK_CLIENT_LISTEN_DISPATCH *ListenDispatch;
    UINT CallbackMask;

    UINT Flags;                    /* SO_REUSEADDR, ... see ws2def.h */
    LONG RefCount;                 /* See ReferenceSocket/DereferenceSocket */

    PIRP ListenIrp;	           /* must be cancelled on close */
    volatile BOOLEAN ListenCancelled;
    HANDLE ListenThreadHandle;     /* needed to restart listening */
    PKTHREAD ListenThread;
    KEVENT StartListenEvent;
    volatile BOOLEAN ListenThreadShouldRun;
    LONG ListenActive;
    BOOLEAN AcceptQueueReady;
    KSPIN_LOCK AcceptLock;
    KSPIN_LOCK AcceptCsqLock;
    IO_CSQ AcceptCsq;
    LIST_ENTRY AcceptIrpList;
    LIST_ENTRY AcceptedSockets;
    ULONG AcceptedCount;
    LIST_ENTRY AcceptedEntry;

    /* AcceptSocket's keep a reference on their listen sockets so
     * that the Address file will be closed only if there are no
     * open connections any more.
     */
    struct _WSK_SOCKET_INTERNAL *ListenSocket;

    /* DereferenceSocket() only works at PASSIVE_LEVEL. If > PASSIVE_LEVEL,
     * it will put the socket on this list and wake the putsockets
     * thread.
     */
    struct _WSK_SOCKET_INTERNAL *NextSocketToDereference;
    ULONG NumSocketDereferences;
} WSK_SOCKET_INTERNAL, *PWSK_SOCKET_INTERNAL;

typedef struct _NETIO_CONTEXT
{
    PIRP UserIrp;
    PWSK_SOCKET_INTERNAL socket;
    PTDI_CONNECTION_INFORMATION TargetConnectionInfo;
    PTDI_CONNECTION_INFORMATION PeerAddrRet;
    PIRP TdiIrp;
    LONG RefCount;
    volatile LONG CancelBusy;
    PKTHREAD CancelThread;
    BOOLEAN Completed;
    PSOCKADDR UserRemoteAddress;
    PCHAR SendData;
    ULONG SendLength;
    ULONG SendDone;
} NETIO_CONTEXT, *PNETIO_CONTEXT;

static void
ReferenceSocket(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    FUNCTION_TRACE;

    InterlockedIncrement(&Socket->RefCount);
}

static void
DereferenceSocket(_In_ PWSK_SOCKET_INTERNAL Socket);

static void
DrainAcceptQueue(_In_ PWSK_SOCKET_INTERNAL ListenSocket)
{
    PIRP Irp;
    KIRQL OldIrql;
    LIST_ENTRY Orphans;
    PLIST_ENTRY Entry;

    while ((Irp = IoCsqRemoveNextIrp(&ListenSocket->AcceptCsq, NULL)) != NULL)
    {
        Irp->IoStatus.Status = STATUS_CANCELLED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
    }

    InitializeListHead(&Orphans);
    KeAcquireSpinLock(&ListenSocket->AcceptLock, &OldIrql);
    while (!IsListEmpty(&ListenSocket->AcceptedSockets))
    {
        Entry = RemoveHeadList(&ListenSocket->AcceptedSockets);
        InsertTailList(&Orphans, Entry);
    }
    ListenSocket->AcceptedCount = 0;
    KeReleaseSpinLock(&ListenSocket->AcceptLock, OldIrql);

    while (!IsListEmpty(&Orphans))
    {
        Entry = RemoveHeadList(&Orphans);
        DereferenceSocket(CONTAINING_RECORD(Entry, WSK_SOCKET_INTERNAL, AcceptedEntry));
    }
}

/* This function can be called several times on the same socket.
   In particular it will be called by WskClose and later by the
   DereferenceSocket() of the socket.
 */

static
void SocketShutdown(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    NTSTATUS status;

    FUNCTION_TRACE;

    if (Socket->ListenSocket != NULL)
    {
        DereferenceSocket(Socket->ListenSocket);
        Socket->ListenSocket = NULL;
    }
    if (Socket->ListenThreadHandle != NULL)
    {
        Socket->ListenThreadShouldRun = FALSE;
        KeSetEvent(&Socket->StartListenEvent, IO_NO_INCREMENT, FALSE);
        status = KeWaitForSingleObject(Socket->ListenThread, Executive, KernelMode, FALSE, (PLARGE_INTEGER)NULL);
        if (!NT_SUCCESS(status))
        {
            DPRINT1("KeWaitForSingleObject failed with status 0x%08x!\n", status);
        }
        ObDereferenceObject(Socket->ListenThread);
        Socket->ListenThread = NULL;
        Socket->ListenThreadHandle = NULL;
    }
    if (Socket->ListenIrp != NULL)
    {
        Socket->ListenCancelled = TRUE;
        IoCancelIrp(Socket->ListenIrp);
        Socket->ListenIrp = NULL;
    }
    if (Socket->AcceptQueueReady)
    {
        DrainAcceptQueue(Socket);
    }
    if (Socket->ConnectionFile != NULL)
    {
        if (Socket->ConnectionFileAssociated)
        {
            /* This fails with error STATUS_CONNECTION_ACTIVE on Windows 2003 */
            status = TdiDisassociateAddressFile(Socket->ConnectionFile);
            if (!NT_SUCCESS(status))
            {
                DPRINT1("Warning: TdiDisassociateAddressFile returned status %08x\n", status);
            }
            Socket->ConnectionFileAssociated = FALSE;
        }
        ObDereferenceObject(Socket->ConnectionFile);
        Socket->ConnectionFile = NULL;
    }
    if (Socket->ConnectionHandle != NULL)
    {
        ZwClose(Socket->ConnectionHandle);
        Socket->ConnectionHandle = NULL;
    }
}

static void
SocketDestroy(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    SocketShutdown(Socket);

    /* Especially listen sockets must keep the LocalAddressFile
     * open when being closed and there are still accepted sockets
     * somewhere. Else the accepted socket I/O will fail with
     * STATUS_INVALID_DEVICE_STATE. So do not close the
     * address file in Shutdown only close it here when all AcceptSockets
     * are gone (AcceptSockets hold a reference to the listen socket).
     */

    if (Socket->LocalAddressFile != NULL)
    {
        ObDereferenceObject(Socket->LocalAddressFile);
        Socket->LocalAddressFile = NULL;
    }
    if (Socket->LocalAddressHandle != NULL)
    {
        ZwClose(Socket->LocalAddressHandle);
        Socket->LocalAddressHandle = NULL;
    }
    ExFreePoolWithTag(Socket, TAG_NETIO);
}

static void
DereferenceSocketSynchronous(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    if (InterlockedDecrement(&Socket->RefCount) == 0)
    {
        SocketDestroy(Socket);
    }
}

static PWSK_SOCKET_INTERNAL SocketsToDereference = NULL;
static KSPIN_LOCK SocketsToDereferenceListLock = 0;
static volatile BOOLEAN DereferenceSocketsThreadShouldRun = FALSE;
static KEVENT DereferenceSocketsEvent;
static HANDLE DereferenceSocketsThreadHandle;

static VOID NTAPI DereferenceSocketsThread(_In_opt_ void *p)
{
    NTSTATUS status;
    PWSK_SOCKET_INTERNAL SocketToDereference;
    KIRQL OldIrql;
    ULONG NumSocketDereferences;

    FUNCTION_TRACE;

    while (DereferenceSocketsThreadShouldRun)
    {
        status = KeWaitForSingleObject(&DereferenceSocketsEvent, Executive, KernelMode, FALSE, NULL);
        if (!NT_SUCCESS(status))
        {
            DPRINT1("KeWaitForSingleObject failed with status 0x%08x!\n", status);
        }

        KeAcquireSpinLock(&SocketsToDereferenceListLock, &OldIrql);
        while (SocketsToDereference != NULL)
        {
            SocketToDereference = SocketsToDereference;
            SocketsToDereference = SocketToDereference->NextSocketToDereference;
            NumSocketDereferences = SocketToDereference->NumSocketDereferences;

            while (NumSocketDereferences > 0)
            {
                SocketToDereference->NumSocketDereferences--;
                NumSocketDereferences = SocketToDereference->NumSocketDereferences;

                KeReleaseSpinLock(&SocketsToDereferenceListLock, OldIrql);
                DereferenceSocketSynchronous(SocketToDereference);
                KeAcquireSpinLock(&SocketsToDereferenceListLock, &OldIrql);
            }
        }
        KeReleaseSpinLock(&SocketsToDereferenceListLock, OldIrql);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS StartSocketDereferenceThread(void)
{
    NTSTATUS status;

    KeInitializeSpinLock(&SocketsToDereferenceListLock);
    KeInitializeEvent(&DereferenceSocketsEvent, SynchronizationEvent, FALSE);
    DereferenceSocketsThreadShouldRun = TRUE;

    status = PsCreateSystemThread(&DereferenceSocketsThreadHandle, THREAD_ALL_ACCESS, NULL, NULL, NULL, DereferenceSocketsThread, NULL);
    if (!NT_SUCCESS(status))
    {
        DPRINT1("Could not start put sockets thread, status is %x\n", status);
    }
    return status;
}

static void StopSocketDereferenceThread(void)
{
    DereferenceSocketsThreadShouldRun = FALSE;
    KeSetEvent(&DereferenceSocketsEvent, IO_NO_INCREMENT, FALSE);

    /* eventually it will terminate, no need to wait for that. */
}

static void
DereferenceSocket(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    KIRQL OldIrql;

    FUNCTION_TRACE;

    /* We are doing calls like ZwClose, TdiDisassociateAddressFile, ...
       which require IRQL to be PASSIVE_LEVEL. So if we get here
       when IRQL is greater than PASSIVE_LEVEL, defer the DereferenceSocket()
       to a separate thread. */

    if (KeGetCurrentIrql() > PASSIVE_LEVEL)
    {
        KeAcquireSpinLock(&SocketsToDereferenceListLock, &OldIrql);
        if (Socket->NumSocketDereferences == 0)
        {
            Socket->NextSocketToDereference = SocketsToDereference;
            SocketsToDereference = Socket;
            Socket->NumSocketDereferences = 1;
        }
        else
        {
            Socket->NumSocketDereferences++;
        }
        KeReleaseSpinLock(&SocketsToDereferenceListLock, OldIrql);

        KeSetEvent(&DereferenceSocketsEvent, IO_NO_INCREMENT, FALSE);

        return;
    }
    DereferenceSocketSynchronous(Socket);
}

NTSTATUS
WfpCreateDevice(
    _In_ PDRIVER_OBJECT DriverObject);

VOID
NsiInitialize(VOID);

NTSTATUS NTAPI
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    FUNCTION_TRACE;

    NsiInitialize();
    return WfpCreateDevice(DriverObject);
}

static NTSTATUS WSKAPI
WskSocket(
    _In_ PWSK_CLIENT Client,
    _In_ ADDRESS_FAMILY AddressFamily,
    _In_ USHORT SocketType,
    _In_ ULONG Protocol,
    _In_ ULONG Flags,
    _In_opt_ PVOID SocketContext,
    _In_opt_ const VOID * Dispatch,
    _In_opt_ PEPROCESS OwningProcess,
    _In_opt_ PETHREAD OwningThread,
    _In_opt_ PSECURITY_DESCRIPTOR SecurityDescriptor,
    _Inout_ PIRP Irp);

static void
NetioContextRelease(_In_ PNETIO_CONTEXT Context)
{
    if (InterlockedDecrement(&Context->RefCount) != 0)
    {
        return;
    }
    if (Context->TargetConnectionInfo != NULL)
    {
        ExFreePoolWithTag(Context->TargetConnectionInfo, TAG_AFD_TDI_CONNECTION_INFORMATION);
    }
    if (Context->PeerAddrRet != NULL)
    {
        ExFreePoolWithTag(Context->PeerAddrRet, TAG_AFD_TDI_CONNECTION_INFORMATION);
    }
    ExFreePoolWithTag(Context, TAG_NETIO);
}

static PNETIO_CONTEXT
NetioContextAllocate(_In_ PWSK_SOCKET_INTERNAL Socket, _In_ PIRP UserIrp)
{
    PNETIO_CONTEXT Context = ExAllocatePoolZero(NonPagedPool, sizeof(*Context), TAG_NETIO);

    if (Context != NULL)
    {
        Context->socket = Socket;
        Context->UserIrp = UserIrp;
        Context->RefCount = 1;
    }
    return Context;
}

static void
NetioCancelTdiIrp(_In_ PNETIO_CONTEXT Context, _In_ KIRQL CancelIrql)
{
    PIRP TdiIrp = Context->TdiIrp;

    InterlockedIncrement(&Context->RefCount);
    Context->CancelThread = KeGetCurrentThread();
    InterlockedExchange(&Context->CancelBusy, 1);
    IoReleaseCancelSpinLock(CancelIrql);
    if (TdiIrp != NULL)
    {
        IoCancelIrp(TdiIrp);
    }
    InterlockedExchange(&Context->CancelBusy, 0);
    NetioContextRelease(Context);
}

static VOID NTAPI
NetioCancel(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    NetioCancelTdiIrp((PNETIO_CONTEXT)Irp->Tail.Overlay.DriverContext[0], Irp->CancelIrql);
}

static void
NetioArmCancel(_In_ PNETIO_CONTEXT Context)
{
    Context->RefCount = 2;
    Context->UserIrp->Tail.Overlay.DriverContext[0] = Context;
    IoSetCancelRoutine(Context->UserIrp, NetioCancel);
}

static void
NetioDisarmCancel(_In_ PNETIO_CONTEXT Context)
{
    KIRQL OldIrql;
    PDRIVER_CANCEL CancelRoutine;

    IoAcquireCancelSpinLock(&OldIrql);
    CancelRoutine = IoSetCancelRoutine(Context->UserIrp, NULL);
    Context->Completed = TRUE;
    IoReleaseCancelSpinLock(OldIrql);
    if (CancelRoutine == NULL && Context->CancelThread != KeGetCurrentThread())
    {
        while (Context->CancelBusy != 0)
        {
            YieldProcessor();
        }
    }
}

static NTSTATUS
NetioIssued(_In_ PNETIO_CONTEXT Context, _In_ NTSTATUS Status)
{
    PIRP UserIrp = Context->UserIrp;
    KIRQL OldIrql;

    if (Context->TdiIrp == NULL)
    {
        NetioDisarmCancel(Context);
        DereferenceSocket(Context->socket);
        UserIrp->IoStatus.Status = Status;
        UserIrp->IoStatus.Information = 0;
        IoCompleteRequest(UserIrp, IO_NETWORK_INCREMENT);
        NetioContextRelease(Context);
        NetioContextRelease(Context);
        return Status;
    }

    IoAcquireCancelSpinLock(&OldIrql);
    if (!Context->Completed && UserIrp->Cancel && IoSetCancelRoutine(UserIrp, NULL) != NULL)
    {
        NetioCancelTdiIrp(Context, OldIrql);
    }
    else
    {
        IoReleaseCancelSpinLock(OldIrql);
    }
    NetioContextRelease(Context);
    return STATUS_PENDING;
}

static BOOLEAN
NetioContinueSend(_In_ PNETIO_CONTEXT Context);

static NTSTATUS NTAPI
NetioComplete(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PVOID ContextParam)
{
    PNETIO_CONTEXT Context = (PNETIO_CONTEXT)ContextParam;
    PIRP UserIrp = Context->UserIrp;
    NTSTATUS Status;
    ULONG_PTR Information;

    FUNCTION_TRACE;

    NetioDisarmCancel(Context);

    Status = Irp->IoStatus.Status;
    Information = Irp->IoStatus.Information;
    if (Context->SendData != NULL)
    {
        if (NT_SUCCESS(Status) && Information != 0)
        {
            Context->SendDone += (ULONG)Information;
            if (Context->SendDone < Context->SendLength)
            {
                if (NetioContinueSend(Context))
                {
                    return STATUS_SUCCESS;
                }
                Status = STATUS_CANCELLED;
            }
        }
        Information = Context->SendDone;
    }
    UserIrp->IoStatus.Status = Status;
    UserIrp->IoStatus.Information = Information;

    if (Context->PeerAddrRet != NULL && NT_SUCCESS(Irp->IoStatus.Status))
    {
        PSOCKADDR RemoteAddress =
            (PSOCKADDR)(&((PTRANSPORT_ADDRESS)Context->PeerAddrRet->RemoteAddress)->Address[0].AddressType);

        if (Context->UserRemoteAddress != NULL)
        {
            memcpy(Context->UserRemoteAddress, RemoteAddress, sizeof(*Context->UserRemoteAddress));
        }
        else
        {
            memcpy(&Context->socket->RemoteAddress, RemoteAddress, sizeof(Context->socket->RemoteAddress));
        }
    }
    IoCompleteRequest(UserIrp, IO_NETWORK_INCREMENT);

    DereferenceSocket(Context->socket);
    NetioContextRelease(Context);

    return STATUS_SUCCESS;
}

static BOOLEAN
NetioContinueSend(_In_ PNETIO_CONTEXT Context)
{
    PIRP UserIrp = Context->UserIrp;
    KIRQL OldIrql;
    NTSTATUS Status;

    IoAcquireCancelSpinLock(&OldIrql);
    if (UserIrp->Cancel)
    {
        IoReleaseCancelSpinLock(OldIrql);
        return FALSE;
    }
    Context->Completed = FALSE;
    Context->TdiIrp = NULL;
    IoSetCancelRoutine(UserIrp, NetioCancel);
    IoReleaseCancelSpinLock(OldIrql);

    InterlockedIncrement(&Context->RefCount);
    Status = TdiSend(&Context->TdiIrp, Context->socket->ConnectionFile, 0,
                     Context->SendData + Context->SendDone,
                     Context->SendLength - Context->SendDone, NetioComplete, Context);
    NetioIssued(Context, Status);
    return TRUE;
}

typedef struct _LISTEN_CONTEXT
{
    PWSK_SOCKET_INTERNAL ListenSocket;
    PWSK_SOCKET_INTERNAL AcceptSocket;
    PTDI_CONNECTION_INFORMATION RequestConnectionInfo, ReturnConnectionInfo;
} LISTEN_CONTEXT, *PLISTEN_CONTEXT;

static NTSTATUS NTAPI
CompletionFireEvent(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PVOID Context)
{
    PKEVENT Event = (PKEVENT)Context;

    FUNCTION_TRACE;

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS CreateSocket(
    _In_ ADDRESS_FAMILY AddressFamily,
    _In_ USHORT SocketType,
    _In_ ULONG Protocol,
    _In_ ULONG Flags,
    _Outptr_ PWSK_SOCKET_INTERNAL *TheSocket)
{
    PIRP NewSocketIrp;
    KEVENT CompletionEvent;
    NTSTATUS Status;

    FUNCTION_TRACE;

    NewSocketIrp = IoAllocateIrp(1, FALSE);
    if (NewSocketIrp == NULL)
    {
        DPRINT1("Out of memory?\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KeInitializeEvent(&CompletionEvent, NotificationEvent, FALSE);
    IoSetCompletionRoutine(NewSocketIrp, CompletionFireEvent, &CompletionEvent, TRUE, TRUE, TRUE);
    NewSocketIrp->Tail.Overlay.Thread = PsGetCurrentThread();

    Status = WskSocket(NULL, AddressFamily, SocketType, Protocol, Flags,
        NULL, NULL, NULL, NULL, NULL, NewSocketIrp);

    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&CompletionEvent, Executive, KernelMode, FALSE, NULL);
        Status = NewSocketIrp->IoStatus.Status;
    }

    if (NT_SUCCESS(Status))
    {
        *TheSocket = (PWSK_SOCKET_INTERNAL)NewSocketIrp->IoStatus.Information;
    }

    IoFreeIrp(NewSocketIrp);
    return Status;
}

static void QueueListening(_In_ PWSK_SOCKET_INTERNAL ListenSocket)
{
    FUNCTION_TRACE;

    KeSetEvent(&ListenSocket->StartListenEvent, IO_NO_INCREMENT, FALSE);
}

static NTSTATUS
StartListening(_In_ PWSK_SOCKET_INTERNAL ListenSocket);

static void StartListeningOnce(_In_ PWSK_SOCKET_INTERNAL ListenSocket)
{
    if (InterlockedExchange(&ListenSocket->ListenActive, 1) != 0)
    {
        return;
    }
    if (KeGetCurrentIrql() == PASSIVE_LEVEL)
    {
        if (StartListening(ListenSocket) != STATUS_PENDING)
        {
            InterlockedExchange(&ListenSocket->ListenActive, 0);
        }
    }
    else
    {
        QueueListening(ListenSocket);
    }
}

static PWSK_SOCKET_INTERNAL
AcceptCsqSocket(_In_ PIO_CSQ Csq)
{
    return CONTAINING_RECORD(Csq, WSK_SOCKET_INTERNAL, AcceptCsq);
}

static VOID NTAPI
AcceptCsqInsertIrp(_In_ PIO_CSQ Csq, _In_ PIRP Irp)
{
    InsertTailList(&AcceptCsqSocket(Csq)->AcceptIrpList, &Irp->Tail.Overlay.ListEntry);
}

static VOID NTAPI
AcceptCsqRemoveIrp(_In_ PIO_CSQ Csq, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

static PIRP NTAPI
AcceptCsqPeekNextIrp(_In_ PIO_CSQ Csq, _In_opt_ PIRP Irp, _In_opt_ PVOID PeekContext)
{
    PLIST_ENTRY Head = &AcceptCsqSocket(Csq)->AcceptIrpList;
    PLIST_ENTRY Entry = (Irp == NULL) ? Head->Flink : Irp->Tail.Overlay.ListEntry.Flink;
    PIRP Next;

    for (; Entry != Head; Entry = Entry->Flink)
    {
        Next = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        if (PeekContext == NULL || Next == PeekContext)
        {
            return Next;
        }
    }
    return NULL;
}

_IRQL_raises_(DISPATCH_LEVEL)
static VOID NTAPI
AcceptCsqAcquireLock(_In_ PIO_CSQ Csq, _Out_ PKIRQL Irql)
{
    KeAcquireSpinLock(&AcceptCsqSocket(Csq)->AcceptCsqLock, Irql);
}

static VOID NTAPI
AcceptCsqReleaseLock(_In_ PIO_CSQ Csq, _In_ KIRQL Irql)
{
    KeReleaseSpinLock(&AcceptCsqSocket(Csq)->AcceptCsqLock, Irql);
}

static VOID NTAPI
AcceptCsqCompleteCanceledIrp(_In_ PIO_CSQ Csq, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
}

static void
CompleteAcceptIrp(_In_ PWSK_SOCKET_INTERNAL ListenSocket, _In_ PWSK_SOCKET_INTERNAL AcceptSocket, _In_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PSOCKADDR LocalAddress = Stack->Parameters.Others.Argument3;
    PSOCKADDR RemoteAddress = Stack->Parameters.Others.Argument4;

    AcceptSocket->user_context = Stack->Parameters.Others.Argument1;
    AcceptSocket->ListenDispatch = Stack->Parameters.Others.Argument2;
    if (LocalAddress != NULL)
    {
        memcpy(LocalAddress, &ListenSocket->LocalAddress, sizeof(*LocalAddress));
    }
    if (RemoteAddress != NULL)
    {
        memcpy(RemoteAddress, &AcceptSocket->RemoteAddress, sizeof(*RemoteAddress));
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = (ULONG_PTR)&AcceptSocket->Socket;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
}

static void
QueueAcceptedSocket(_In_ PWSK_SOCKET_INTERNAL ListenSocket, _In_ PWSK_SOCKET_INTERNAL AcceptSocket)
{
    PIRP Irp;
    KIRQL OldIrql;
    BOOLEAN Queued = FALSE;

    KeAcquireSpinLock(&ListenSocket->AcceptLock, &OldIrql);
    Irp = IoCsqRemoveNextIrp(&ListenSocket->AcceptCsq, NULL);
    if (Irp == NULL && ListenSocket->AcceptedCount < NETIO_ACCEPT_BACKLOG)
    {
        InsertTailList(&ListenSocket->AcceptedSockets, &AcceptSocket->AcceptedEntry);
        ListenSocket->AcceptedCount++;
        Queued = TRUE;
    }
    KeReleaseSpinLock(&ListenSocket->AcceptLock, OldIrql);

    if (Irp != NULL)
    {
        CompleteAcceptIrp(ListenSocket, AcceptSocket, Irp);
    }
    else if (!Queued)
    {
        DPRINT1("Accept backlog of %u connections is full, dropping the new connection\n", NETIO_ACCEPT_BACKLOG);
        DereferenceSocket(AcceptSocket);
    }
}

static NTSTATUS NTAPI
ListenComplete(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PVOID Context)
{
    PLISTEN_CONTEXT ListenContext = (PLISTEN_CONTEXT)Context;
    PWSK_SOCKET_INTERNAL ListenSocket = ListenContext->ListenSocket;
    PWSK_SOCKET_INTERNAL AcceptSocket = ListenContext->AcceptSocket;
    PWSK_CLIENT_LISTEN_DISPATCH ListenDispatch =
        (PWSK_CLIENT_LISTEN_DISPATCH)ListenSocket->ListenDispatch;

    /* A PTRANSPORT_ADDRESS address field has an additional
     * AddressLength field so the struct sockaddr_in starts
     * at the AddressType (the address family, 2 for AF_INET)
     * field.
     */

    PSOCKADDR RemoteAddress =
        (PSOCKADDR)(&((PTRANSPORT_ADDRESS)ListenContext->ReturnConnectionInfo->RemoteAddress)->Address[0].AddressType);
    PVOID AcceptSocketContext;
    const WSK_CLIENT_CONNECTION_DISPATCH *AcceptSocketDispatch;
    BOOLEAN Requeue = Irp->PendingReturned || NT_SUCCESS(Irp->IoStatus.Status);
    NTSTATUS Status;

    FUNCTION_TRACE;

    if (ListenSocket->CallbackMask & WSK_EVENT_ACCEPT &&
        ListenDispatch->WskAcceptEvent != NULL &&
        ListenSocket->ListenIrp != NULL &&
        !ListenSocket->ListenCancelled &&
        Irp->IoStatus.Status != STATUS_CANCELLED)
    {
        ListenSocket->ListenIrp = NULL;

        Status = ListenDispatch->WskAcceptEvent(ListenSocket->user_context,
                                                0,
                                                &ListenSocket->LocalAddress,
                                                RemoteAddress,
                                                &AcceptSocket->Socket,
                                                &AcceptSocketContext,
                                                &AcceptSocketDispatch);

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ListenDispatch->WskAcceptEvent returned non-successful status 0x%08x\n", Status);
        }
        else
        {
            memcpy(&AcceptSocket->RemoteAddress, RemoteAddress, sizeof(AcceptSocket->RemoteAddress));
        }
    }
    else if (NT_SUCCESS(Irp->IoStatus.Status) &&
             !ListenSocket->ListenCancelled &&
             (ListenSocket->CallbackMask & WSK_EVENT_ACCEPT) == 0)
    {
        memcpy(&AcceptSocket->RemoteAddress, RemoteAddress, sizeof(AcceptSocket->RemoteAddress));
        QueueAcceptedSocket(ListenSocket, AcceptSocket);
    }
    else
    {
        DereferenceSocket(AcceptSocket);	/* close the AcceptSocket */
    }
    ListenSocket->ListenIrp = NULL;

    DereferenceSocket(AcceptSocket);
    DereferenceSocket(ListenSocket);

    ExFreePoolWithTag(ListenContext->ReturnConnectionInfo, TAG_AFD_TDI_CONNECTION_INFORMATION);
    ExFreePoolWithTag(ListenContext->RequestConnectionInfo, TAG_AFD_TDI_CONNECTION_INFORMATION);
    ExFreePoolWithTag(ListenContext, TAG_NETIO);

    /* And wait for the next incoming connection.
     * This is done in a separate thread at IRQL = PASSIVE_LEVEL
     */

    if (Requeue)
    {
        QueueListening(ListenSocket);
    }
    else
    {
        InterlockedExchange(&ListenSocket->ListenActive, 0);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
StartListening(_In_ PWSK_SOCKET_INTERNAL ListenSocket)
{
    PIRP tdiIrp;
    NTSTATUS status;
    PLISTEN_CONTEXT lc;
    PWSK_SOCKET_INTERNAL AcceptSocket;

    FUNCTION_TRACE;

    if (ListenSocket->LocalAddressHandle == NULL)
    {
        DPRINT1("LocalAddressHandle is not set, need to bind your socket before listening\n");
        return STATUS_INVALID_PARAMETER;
    }

    status = CreateSocket(ListenSocket->family, ListenSocket->type, ListenSocket->proto, WSK_FLAG_CONNECTION_SOCKET, &AcceptSocket);
    if (!NT_SUCCESS(status))
    {
        DPRINT1("Could not create AcceptSocket, status is 0x%08x\n", status);
        return status;
    }
    AcceptSocket->ListenSocket = ListenSocket;
    memcpy(&AcceptSocket->LocalAddress, &ListenSocket->LocalAddress, sizeof(AcceptSocket->LocalAddress));
    AcceptSocket->LocalAddressKnown = TRUE;
    /* Dereferenced when the AcceptSocket is closed */
    ReferenceSocket(AcceptSocket->ListenSocket);

    status = STATUS_INSUFFICIENT_RESOURCES;
    lc = ExAllocatePoolUninitialized(NonPagedPool, sizeof(*lc), TAG_NETIO);
    if (lc == NULL)
    {
        goto err_out_free_accept_socket;
    }
    lc->ListenSocket = ListenSocket;
    lc->AcceptSocket = AcceptSocket;

    tdiIrp = NULL;

    status = TdiAssociateAddressFile(ListenSocket->LocalAddressHandle, AcceptSocket->ConnectionFile);
    if (!NT_SUCCESS(status))
    {
        goto err_out_free_lc;
    }
    AcceptSocket->ConnectionFileAssociated = TRUE;

    lc->RequestConnectionInfo = NULL;
    TdiBuildNullConnectionInfo(&lc->RequestConnectionInfo, TDI_ADDRESS_TYPE_IP);
    if (lc->RequestConnectionInfo == NULL)
    {
        goto err_out_free_lc_and_disassociate;
    }
    lc->RequestConnectionInfo->OptionsLength = 0;

    lc->ReturnConnectionInfo = NULL;
    TdiBuildNullConnectionInfo(&lc->ReturnConnectionInfo, TDI_ADDRESS_TYPE_IP);
    if (lc->ReturnConnectionInfo == NULL)
    {
        goto err_out_free_lc_and_req_conn_info;
    }
    lc->ReturnConnectionInfo->OptionsLength = 0;

    ReferenceSocket(ListenSocket);
    ReferenceSocket(AcceptSocket);

    ListenSocket->ListenCancelled = FALSE;
    /* pass the ConnectionFile from accept socket, not from ListenSocket ... */
    status = TdiListen(&tdiIrp, AcceptSocket->ConnectionFile, &lc->RequestConnectionInfo, &lc->ReturnConnectionInfo, ListenComplete, lc);

    if (tdiIrp == NULL)
    {
        ExFreePoolWithTag(lc->ReturnConnectionInfo, TAG_NETIO);
        DereferenceSocket(ListenSocket);
        DereferenceSocket(AcceptSocket);
        goto err_out_free_lc_and_req_conn_info;
    }
    if (status == STATUS_PENDING)
        ListenSocket->ListenIrp = tdiIrp;

    return STATUS_PENDING;

err_out_free_lc_and_req_conn_info:
    ExFreePoolWithTag(lc->RequestConnectionInfo, TAG_NETIO);

err_out_free_lc_and_disassociate:
    status = TdiDisassociateAddressFile(AcceptSocket->ConnectionFile);
    if (!NT_SUCCESS(status))
    {
        DPRINT1("Warning: TdiDisassociateAddressFile returned status %08x\n", status);
    }
    AcceptSocket->ConnectionFileAssociated = FALSE;

err_out_free_lc:
    ExFreePoolWithTag(lc, TAG_NETIO);

err_out_free_accept_socket:
    DereferenceSocket(AcceptSocket);

    return status;
}

static VOID NTAPI RequeueListenThread(_In_ PVOID p)
{
    PWSK_SOCKET_INTERNAL ListenSocket = (PWSK_SOCKET_INTERNAL)p;
    NTSTATUS status;

    FUNCTION_TRACE;

    while (ListenSocket->ListenThreadShouldRun)
    {
        status = KeWaitForSingleObject(&ListenSocket->StartListenEvent, Executive, KernelMode, FALSE, NULL);
        if (!NT_SUCCESS(status))
        {
            DPRINT1("KeWaitForSingleObject failed with status 0x%08x!\n", status);
        }
        if (!ListenSocket->ListenThreadShouldRun)
        {
            break;
        }
        if (StartListening(ListenSocket) != STATUS_PENDING)
        {
            InterlockedExchange(&ListenSocket->ListenActive, 0);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS WSKAPI
WskControlSocket(
    _In_ PWSK_SOCKET SocketParam,
    _In_ WSK_CONTROL_SOCKET_TYPE RequestType,
    _In_ ULONG ControlCode,
    _In_ ULONG Level,
    _In_ SIZE_T InputSize,
    _In_reads_bytes_opt_(InputSize) PVOID InputBuffer,
    _In_ SIZE_T OutputSize,
    _Out_writes_bytes_opt_(OutputSize) PVOID OutputBuffer,
    _Out_opt_ SIZE_T * OutputSizeReturned,
    _Inout_opt_ PIRP Irp)
{
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    NTSTATUS status = STATUS_NOT_IMPLEMENTED;

    FUNCTION_TRACE;

    if (RequestType == WskIoctl)
    {
        WSK_REQUIRE_IRP(Irp);
    }
    if (Irp != NULL)
    {
        IoSetNextIrpStackLocation(Irp);
    }
    if (Socket == NULL)
    {
        status = STATUS_INVALID_PARAMETER;
        goto err_out;
    }

    switch (RequestType)
    {
        case WskSetOption:
            switch (Level)
            {
                case SOL_SOCKET:
                    switch (ControlCode)
                    {
                        case SO_REUSEADDR:     /* add more supported flags here */
                            if (InputBuffer == NULL)
                            {
                                DPRINT1("WskControlSocket: Need an InputBuffer for this operation\n");
                                status = STATUS_INVALID_PARAMETER;
                                break;
                            }
                            if (InputSize < sizeof(UINT))
                            {
                                DPRINT1("WskControlSocket: InputBuffer too small for this operation\n");
                                status = STATUS_INVALID_PARAMETER;
                                break;
                            }
                            UINT enable = *(PUINT)InputBuffer;
                            if (enable != 0)
                            {
                                Socket->Flags |= ControlCode;
                            }
                            else
                            {
                                Socket->Flags &= ~ControlCode;
                            }
                            status = STATUS_SUCCESS;
                            break;

                            /* Windows specific. This sets the mask for callback functions. */
                        case SO_WSK_EVENT_CALLBACK:
                            if (InputBuffer == NULL)
                            {
                                DPRINT1("WskControlSocket: Need an InputBuffer for this operation\n");
                                status = STATUS_INVALID_PARAMETER;
                                break;
                            }
                            if (InputSize < sizeof(WSK_EVENT_CALLBACK_CONTROL))
                            {
                                DPRINT1("WskControlSocket: InputBuffer too small for this operation\n");
                                status = STATUS_INVALID_PARAMETER;
                                break;
                            }
                            PWSK_EVENT_CALLBACK_CONTROL Control = (WSK_EVENT_CALLBACK_CONTROL *)InputBuffer;

                            if (((Socket->CallbackMask & WSK_EVENT_ACCEPT) == 0) &&
                                ((Control->EventMask & WSK_EVENT_ACCEPT) == WSK_EVENT_ACCEPT))
                            {
                                Socket->CallbackMask = Control->EventMask;
                                StartListeningOnce(Socket);
                            }
                            else
                            {
                                Socket->CallbackMask = Control->EventMask;
                            }

                            status = STATUS_SUCCESS;
                            break;

                        default:
                            DPRINT1("WskControlSocket: ControlCode %d Not implemented\n", ControlCode);
                    }
                    break;
                default:
                    DPRINT1("WskControlSocket: Level %d Not implemented\n", Level);
            }
            break;

        case WskGetOption:
        case WskIoctl:
        default:
            DPRINT1("WskControlSocket: Option %d Not implemented\n", RequestType);
    }

err_out:
    if (Irp == NULL)
    {
        return status;
    }
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return STATUS_PENDING;
}

static NTSTATUS WSKAPI
WskCloseSocket(_In_ PWSK_SOCKET SocketParam, _Inout_ PIRP Irp)
{
    NTSTATUS status = STATUS_SUCCESS;
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);

    WSK_REQUIRE_IRP(Irp);
    IoSetNextIrpStackLocation(Irp);

    /* There might be a reference from (for example) a pending
     * receive. Shutdown the socket here explicitly. We expect
     * all pending I/O operations to be cancelled, then.
     */

    SocketShutdown(Socket);
    DereferenceSocket(Socket);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return STATUS_PENDING;
}

static PTRANSPORT_ADDRESS
TdiTransportAddressFromSocketAddress(_In_ PSOCKADDR SocketAddress)
{
    PTRANSPORT_ADDRESS ta;
    ULONG Size;

    Size = FIELD_OFFSET(TRANSPORT_ADDRESS, Address) +
           FIELD_OFFSET(TA_ADDRESS, Address[sizeof(SocketAddress->sa_data)]);

    ta = ExAllocatePoolUninitialized(NonPagedPool, Size, TAG_NETIO);
    if (ta == NULL)
    {
        DPRINT1("TdiTransportAddressFromSocketAddress: Out of memory\n");
        return NULL;
    }

    ta->TAAddressCount = 1;
    ta->Address[0].AddressLength = sizeof(SocketAddress->sa_data);
    ta->Address[0].AddressType = TDI_ADDRESS_TYPE_IP;	/* AF_INET */

    memcpy(&ta->Address[0].Address[0], &SocketAddress->sa_data, ta->Address[0].AddressLength);

    return ta;
}

static PTDI_CONNECTION_INFORMATION
TdiConnectionInfoFromSocketAddress(_In_ PSOCKADDR SocketAddress)
{
    PTRANSPORT_ADDRESS TargetAddress;
    PTDI_CONNECTION_INFORMATION ConnectionInformation = NULL;
    NTSTATUS status;

    TargetAddress = TdiTransportAddressFromSocketAddress(SocketAddress);
    if (TargetAddress == NULL)
        return NULL;

    status = TdiBuildConnectionInfo(&ConnectionInformation, TargetAddress);
    ExFreePoolWithTag(TargetAddress, TAG_NETIO);

    if (!NT_SUCCESS(status))
    {
        if (ConnectionInformation != NULL)
        {
            ExFreePoolWithTag(ConnectionInformation, TAG_AFD_TDI_CONNECTION_INFORMATION);
        }
        return NULL;
    }
    ConnectionInformation->OptionsLength = 0;
    return ConnectionInformation;
}

static NTSTATUS
QueryLocalAddress(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    PTDI_ADDRESS_INFO Info;
    PMDL Mdl;
    ULONG Size = sizeof(TDI_ADDRESS_INFO) + sizeof(TA_IP_ADDRESS);
    NTSTATUS status = STATUS_SUCCESS;

    Info = ExAllocatePoolZero(NonPagedPool, Size, TAG_NETIO);
    if (Info == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Mdl = IoAllocateMdl(Info, Size, FALSE, FALSE, NULL);
    if (Mdl == NULL)
    {
        ExFreePoolWithTag(Info, TAG_NETIO);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    _SEH2_TRY
    {
        MmProbeAndLockPages(Mdl, KernelMode, IoModifyAccess);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(status))
    {
        IoFreeMdl(Mdl);
        ExFreePoolWithTag(Info, TAG_NETIO);
        return status;
    }

    status = TdiQueryInformation(Socket->LocalAddressFile, TDI_QUERY_ADDRESS_INFO, Mdl);
    if (NT_SUCCESS(status) && Info->Address.TAAddressCount >= 1)
    {
        memcpy(&Socket->LocalAddress, &Info->Address.Address[0].AddressType, sizeof(Socket->LocalAddress));
    }
    ExFreePoolWithTag(Info, TAG_NETIO);
    return status;
}

static NTSTATUS WSKAPI
WskBind(_In_ PWSK_SOCKET SocketParam, _In_ PSOCKADDR LocalAddress, _Reserved_ ULONG Flags, _Inout_ PIRP Irp)
{
    NTSTATUS status, status2;
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    PTRANSPORT_ADDRESS ta;

    FUNCTION_TRACE;
    WSK_REQUIRE_IRP(Irp);
    IoSetNextIrpStackLocation(Irp);

    ta = TdiTransportAddressFromSocketAddress(LocalAddress);
    if (ta == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_out;
    }
    if (Socket->LocalAddressHandle != NULL)
    {
        status = STATUS_INVALID_PARAMETER;
        goto err_out;
    }

    status = TdiOpenAddressFile(&Socket->TdiName,
                                ta, AFD_SHARE_REUSE, &Socket->LocalAddressHandle, &Socket->LocalAddressFile);

    ExFreePoolWithTag(ta, TAG_NETIO);
    if (NT_SUCCESS(status))
    {
        memcpy(&Socket->LocalAddress, LocalAddress, sizeof(Socket->LocalAddress));
        Socket->LocalAddressKnown = TRUE;
        status2 = QueryLocalAddress(Socket);
        if (!NT_SUCCESS(status2))
        {
            DPRINT1("Could not read back the bound address, status is %x\n", status2);
        }
        if (Socket->WskFlags == WSK_FLAG_LISTEN_SOCKET)
        {
            StartListeningOnce(Socket);
        }
    }

err_out:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return STATUS_PENDING;
}

enum direction
{
    DIR_SEND,
    DIR_RECEIVE
};

static NTSTATUS WSKAPI
WskSendTo(
    _In_ PWSK_SOCKET SocketParam,
    _In_ PWSK_BUF Buffer,
    _Reserved_ ULONG Flags,
    _In_opt_ PSOCKADDR RemoteAddress,
    _In_ ULONG ControlInfoLength,
    _In_reads_bytes_opt_(ControlInfoLength) PCMSGHDR ControlInfo,
    _Inout_ PIRP Irp)
{
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    PTDI_CONNECTION_INFORMATION TargetConnectionInfo;
    NTSTATUS status;
    void *BufferData;
    PNETIO_CONTEXT NetioContext;

    FUNCTION_TRACE;

    WSK_REQUIRE_IRP(Irp);
    IoSetNextIrpStackLocation(Irp);

    status = STATUS_INSUFFICIENT_RESOURCES;
    NetioContext = NetioContextAllocate(Socket, Irp);
    if (NetioContext == NULL)
    {
        goto err_out;
    }

    TargetConnectionInfo = TdiConnectionInfoFromSocketAddress(RemoteAddress);
    if (TargetConnectionInfo == NULL)
    {
        goto err_out_free_nc;
    }
    NetioContext->TargetConnectionInfo = TargetConnectionInfo;

    BufferData = MmGetSystemAddressForMdlSafe(Buffer->Mdl, NormalPagePriority);
    if (BufferData == NULL)
    {
        DPRINT1("Error mapping MDL\n");
        goto err_out_free_nc;
    }

    IoMarkIrpPending(Irp);
    ReferenceSocket(Socket);
    NetioArmCancel(NetioContext);

    status = TdiSendDatagram(&NetioContext->TdiIrp, Socket->LocalAddressFile, ((char *)BufferData) + Buffer->Offset,
                        Buffer->Length, TargetConnectionInfo, NetioComplete, NetioContext);
    return NetioIssued(NetioContext, status);

err_out_free_nc:
    NetioContextRelease(NetioContext);

err_out:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return status;
}

static NTSTATUS WSKAPI
WskReceiveFrom(
    _In_ PWSK_SOCKET SocketParam,
    _In_ PWSK_BUF Buffer,
    _Reserved_ ULONG Flags,
    _Out_opt_ PSOCKADDR RemoteAddress,
    _Inout_ PULONG ControlLength,
    _Out_writes_bytes_opt_(*ControlLength) PCMSGHDR ControlInfo,
    _Out_opt_ PULONG ControlFlags, _Inout_ PIRP Irp)
{
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    PTDI_CONNECTION_INFORMATION PeerAddrRet;
    NTSTATUS status;
    void *BufferData;
    PNETIO_CONTEXT NetioContext;

    FUNCTION_TRACE;
    UNREFERENCED_PARAMETER(ControlInfo);

    WSK_REQUIRE_IRP(Irp);
    IoSetNextIrpStackLocation(Irp);

    status = STATUS_INVALID_PARAMETER;
    if (Flags != 0 || Buffer == NULL || ControlLength == NULL)
    {
        goto err_out;
    }
    status = STATUS_INVALID_DEVICE_STATE;
    if (Socket->LocalAddressHandle == NULL)
    {
        DPRINT1("LocalAddressHandle is not set, need to bind your socket before receiving\n");
        goto err_out;
    }

    status = STATUS_INSUFFICIENT_RESOURCES;
    NetioContext = NetioContextAllocate(Socket, Irp);
    if (NetioContext == NULL)
    {
        goto err_out;
    }
    NetioContext->UserRemoteAddress = RemoteAddress;

    PeerAddrRet = NULL;
    TdiBuildNullConnectionInfo(&PeerAddrRet, TDI_ADDRESS_TYPE_IP);
    if (PeerAddrRet == NULL)
    {
        goto err_out_free_nc;
    }
    PeerAddrRet->OptionsLength = 0;
    NetioContext->PeerAddrRet = PeerAddrRet;

    BufferData = MmGetSystemAddressForMdlSafe(Buffer->Mdl, NormalPagePriority);
    if (BufferData == NULL)
    {
        DPRINT1("Error mapping MDL\n");
        goto err_out_free_nc;
    }

    *ControlLength = 0;
    if (ControlFlags != NULL)
    {
        *ControlFlags = 0;
    }

    IoMarkIrpPending(Irp);
    ReferenceSocket(Socket);
    NetioArmCancel(NetioContext);

    status = TdiReceiveDatagram(&NetioContext->TdiIrp, Socket->LocalAddressFile, 0, ((char *)BufferData) + Buffer->Offset,
                                Buffer->Length, PeerAddrRet, NetioComplete, NetioContext);
    return NetioIssued(NetioContext, status);

err_out_free_nc:
    NetioContextRelease(NetioContext);

err_out:
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return status;
}

static NTSTATUS WSKAPI
WskReleaseUdp(_In_ PWSK_SOCKET SocketParam, _In_ PWSK_DATAGRAM_INDICATION DatagramIndication)
{
    FUNCTION_TRACE;

    UNIMPLEMENTED;

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS WSKAPI
WskReleaseTcp(_In_ PWSK_SOCKET SocketParam, _In_ PWSK_DATA_INDICATION DataIndication)
{
    FUNCTION_TRACE;

    UNIMPLEMENTED;

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS WSKAPI
WskGetLocalAddress(_In_ PWSK_SOCKET SocketParam, _Out_ PSOCKADDR LocalAddress, _Inout_ PIRP Irp)
{
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    NTSTATUS status = STATUS_INVALID_DEVICE_STATE;

    FUNCTION_TRACE;

    WSK_REQUIRE_IRP(Irp);
    IoSetNextIrpStackLocation(Irp);

    if (Socket->LocalAddressKnown)
    {
        memcpy(LocalAddress, &Socket->LocalAddress, sizeof(*LocalAddress));
        status = STATUS_SUCCESS;
    }
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return STATUS_PENDING;
}

static NTSTATUS WSKAPI
WskGetRemoteAddress(_In_ PWSK_SOCKET SocketParam, _Out_ PSOCKADDR RemoteAddress, _Inout_ PIRP Irp)
{
    FUNCTION_TRACE;

    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    NTSTATUS Status = STATUS_INVALID_PARAMETER;

    WSK_REQUIRE_IRP(Irp);
    IoSetNextIrpStackLocation(Irp);
    if (Socket != NULL)
    {
        memcpy(RemoteAddress, &Socket->RemoteAddress, sizeof(*RemoteAddress));
        Status = STATUS_SUCCESS;
    }
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return STATUS_PENDING;  /* Caller has to wait for his completion routine */
}

static NTSTATUS WSKAPI
WskSocketConnect(
    _In_ PWSK_CLIENT Client,
    _In_ USHORT SocketType,
    _In_ ULONG Protocol,
    _In_ PSOCKADDR LocalAddress,
    _In_ PSOCKADDR RemoteAddress,
    _In_ ULONG Flags,
    _In_opt_ PVOID SocketContext,
    _In_opt_ const WSK_CLIENT_CONNECTION_DISPATCH * Dispatch,
    _In_opt_ PEPROCESS OwningProcess,
    _In_opt_ PETHREAD OwningThread,
    _In_opt_ PSECURITY_DESCRIPTOR SecurityDescriptor,
    _Inout_ PIRP Irp)
{
    FUNCTION_TRACE;

    return WskCompleteUnsupported(Irp, __FUNCTION__);
}

static NTSTATUS WSKAPI
WskControlClient(
    _In_ PWSK_CLIENT Client,
    _In_ ULONG ControlCode,
    _In_ SIZE_T InputSize,
    _In_reads_bytes_opt_(InputSize) PVOID InputBuffer,
    _In_ SIZE_T OutputSize,
    _Out_writes_bytes_opt_(OutputSize) PVOID OutputBuffer,
    _Out_opt_ SIZE_T * OutputSizeReturned, _Inout_opt_ PIRP Irp)
{
    FUNCTION_TRACE;

    return WskCompleteUnsupported(Irp, __FUNCTION__);
}

static WSK_PROVIDER_DATAGRAM_DISPATCH UdpDispatch = {
    .WskControlSocket = WskControlSocket,
    .WskCloseSocket = WskCloseSocket,
    .WskBind = WskBind,
    .WskSendTo = WskSendTo,
    .WskReceiveFrom = WskReceiveFrom,
    .WskRelease = WskReleaseUdp,
    .WskGetLocalAddress = WskGetLocalAddress,
};

/* Connection oriented routines (TCP/IP) */

static NTSTATUS WSKAPI
WskConnect(_In_ PWSK_SOCKET SocketParam, _In_ PSOCKADDR RemoteAddress, _Reserved_ ULONG Flags, _Inout_ PIRP Irp)
{
    PTDI_CONNECTION_INFORMATION TargetConnectionInfo, PeerAddrRet;
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    NTSTATUS status, status2;
    PNETIO_CONTEXT NetioContext;

    FUNCTION_TRACE;

    WSK_REQUIRE_IRP(Irp);
    IoSetNextIrpStackLocation(Irp);

    status = STATUS_INVALID_PARAMETER;
    if (Socket->LocalAddressHandle == NULL)
    {
        DPRINT1("LocalAddressHandle is not set, need to bind your socket before connecting\n");
        goto err_out;
    }

    status = STATUS_INSUFFICIENT_RESOURCES;
    NetioContext = NetioContextAllocate(Socket, Irp);
    if (NetioContext == NULL)
    {
        goto err_out;
    }

    status = TdiAssociateAddressFile(Socket->LocalAddressHandle, Socket->ConnectionFile);
    if (!NT_SUCCESS(status))
    {
        goto err_out_free_nc;
    }
    Socket->ConnectionFileAssociated = TRUE;

    status = STATUS_INSUFFICIENT_RESOURCES;
    TargetConnectionInfo = TdiConnectionInfoFromSocketAddress(RemoteAddress);
    if (TargetConnectionInfo == NULL)
    {
        goto err_out_free_nc_and_disassociate;
    }
    NetioContext->TargetConnectionInfo = TargetConnectionInfo;

    PeerAddrRet = NULL;
    TdiBuildNullConnectionInfo(&PeerAddrRet, TDI_ADDRESS_TYPE_IP);
    if (PeerAddrRet == NULL)
    {
        goto err_out_free_nc_and_disassociate;
    }
    PeerAddrRet->OptionsLength = 0;
    NetioContext->PeerAddrRet = PeerAddrRet;

    IoMarkIrpPending(Irp);
    ReferenceSocket(Socket);
    NetioArmCancel(NetioContext);

    status = TdiConnect(&NetioContext->TdiIrp, Socket->ConnectionFile, TargetConnectionInfo, PeerAddrRet, NetioComplete, NetioContext);
    return NetioIssued(NetioContext, status);

err_out_free_nc_and_disassociate:
    status2 = TdiDisassociateAddressFile(Socket->ConnectionFile);
    if (!NT_SUCCESS(status2))
    {
        DPRINT1("Warning: TdiDisassociateAddressFile returned status %08x\n", status2);
    }
    Socket->ConnectionFileAssociated = FALSE;

err_out_free_nc:
    NetioContextRelease(NetioContext);

err_out:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return status;
}

static NTSTATUS WSKAPI
WskStreamIo(
    _In_ PWSK_SOCKET SocketParam,
    _In_ PWSK_BUF Buffer,
    _In_ ULONG Flags,
     _Inout_ PIRP Irp,
    _In_ enum direction Direction)
{
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    NTSTATUS status;
    void *BufferData;
    PNETIO_CONTEXT NetioContext;

    FUNCTION_TRACE;

    WSK_REQUIRE_IRP(Irp);
    IoSetNextIrpStackLocation(Irp);
    status = STATUS_INSUFFICIENT_RESOURCES;

    NetioContext = NetioContextAllocate(Socket, Irp);
    if (NetioContext == NULL)
    {
        goto err_out;
    }

    BufferData = MmGetSystemAddressForMdlSafe(Buffer->Mdl, NormalPagePriority);
    if (BufferData == NULL)
    {
        DPRINT1("Error mapping MDL\n");
        goto err_out_free_nc;
    }

    IoMarkIrpPending(Irp);
    ReferenceSocket(Socket);
    NetioArmCancel(NetioContext);

    if (Direction == DIR_SEND)
    {
        NetioContext->SendData = ((PCHAR)BufferData) + Buffer->Offset;
        NetioContext->SendLength = Buffer->Length;
        status = TdiSend(&NetioContext->TdiIrp, Socket->ConnectionFile, 0, NetioContext->SendData,
                    NetioContext->SendLength, NetioComplete, NetioContext);
    }
    else
    {
        status = TdiReceive(&NetioContext->TdiIrp, Socket->ConnectionFile, 0, ((char *)BufferData) + Buffer->Offset,
                       Buffer->Length, NetioComplete, NetioContext);
    }
    return NetioIssued(NetioContext, status);

err_out_free_nc:
    NetioContextRelease(NetioContext);

err_out:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return status;
}

static NTSTATUS WSKAPI
WskSend(_In_ PWSK_SOCKET SocketParam, _In_ PWSK_BUF Buffer, _In_ ULONG Flags, _Inout_ PIRP Irp)
{
    FUNCTION_TRACE;

    return WskStreamIo(SocketParam, Buffer, Flags, Irp, DIR_SEND);
}

static NTSTATUS WSKAPI
WskReceive(_In_ PWSK_SOCKET SocketParam, _In_ PWSK_BUF Buffer, _In_ ULONG Flags, _Inout_ PIRP Irp)
{
    FUNCTION_TRACE;

    return WskStreamIo(SocketParam, Buffer, Flags, Irp, DIR_RECEIVE);
}

static NTSTATUS WSKAPI
WskDisconnect(_In_ PWSK_SOCKET SocketParam, _In_opt_ PWSK_BUF Buffer, _In_ ULONG Flags, _Inout_ PIRP Irp)
{
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);

    FUNCTION_TRACE;

    WSK_REQUIRE_IRP(Irp);
    IoSetNextIrpStackLocation(Irp);

    SocketShutdown(Socket);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return STATUS_PENDING;
}


static WSK_PROVIDER_CONNECTION_DISPATCH TcpDispatch = {
    .WskControlSocket = WskControlSocket,
    .WskCloseSocket = WskCloseSocket,
    .WskBind = WskBind,
    .WskConnect = WskConnect,
    .WskGetLocalAddress = WskGetLocalAddress,
    .WskGetRemoteAddress = WskGetRemoteAddress,
    .WskSend = WskSend,
    .WskReceive = WskReceive,
    .WskDisconnect = WskDisconnect,
    .WskRelease = WskReleaseTcp,
};

static NTSTATUS WSKAPI
WskAccept(
    _In_ PWSK_SOCKET ListenSocketParam,
    _Reserved_ ULONG Flags,
    _In_opt_ PVOID AcceptSocketContext,
    _In_opt_ const WSK_CLIENT_CONNECTION_DISPATCH *AcceptSocketDispatch,
    _Out_opt_ PSOCKADDR LocalAddress,
    _Out_opt_ PSOCKADDR RemoteAddress,
    _Inout_ PIRP Irp)
{
    PWSK_SOCKET_INTERNAL ListenSocket = CONTAINING_RECORD(ListenSocketParam, WSK_SOCKET_INTERNAL, Socket);
    PWSK_SOCKET_INTERNAL AcceptSocket = NULL;
    PIO_STACK_LOCATION Stack;
    PLIST_ENTRY Entry;
    NTSTATUS status;
    KIRQL OldIrql;

    FUNCTION_TRACE;
    UNREFERENCED_PARAMETER(Flags);

    WSK_REQUIRE_IRP(Irp);
    IoSetNextIrpStackLocation(Irp);

    status = STATUS_INVALID_DEVICE_STATE;
    if (ListenSocket->WskFlags != WSK_FLAG_LISTEN_SOCKET || ListenSocket->LocalAddressHandle == NULL)
    {
        DPRINT1("WskAccept needs a bound listen socket\n");
        goto err_out;
    }
    if (ListenSocket->CallbackMask & WSK_EVENT_ACCEPT)
    {
        DPRINT1("WskAccept cannot be used while the WskAcceptEvent callback is enabled\n");
        goto err_out;
    }

    Stack = IoGetCurrentIrpStackLocation(Irp);
    Stack->Parameters.Others.Argument1 = AcceptSocketContext;
    Stack->Parameters.Others.Argument2 = (PVOID)AcceptSocketDispatch;
    Stack->Parameters.Others.Argument3 = LocalAddress;
    Stack->Parameters.Others.Argument4 = RemoteAddress;

    IoMarkIrpPending(Irp);
    IoCsqInsertIrp(&ListenSocket->AcceptCsq, Irp, NULL);

    KeAcquireSpinLock(&ListenSocket->AcceptLock, &OldIrql);
    if (!IsListEmpty(&ListenSocket->AcceptedSockets) &&
        IoCsqRemoveNextIrp(&ListenSocket->AcceptCsq, Irp) == Irp)
    {
        Entry = RemoveHeadList(&ListenSocket->AcceptedSockets);
        ListenSocket->AcceptedCount--;
        AcceptSocket = CONTAINING_RECORD(Entry, WSK_SOCKET_INTERNAL, AcceptedEntry);
    }
    KeReleaseSpinLock(&ListenSocket->AcceptLock, OldIrql);

    if (AcceptSocket != NULL)
    {
        CompleteAcceptIrp(ListenSocket, AcceptSocket, Irp);
    }
    else
    {
        StartListeningOnce(ListenSocket);
    }
    return STATUS_PENDING;

err_out:
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return status;
}

static NTSTATUS WSKAPI
WskInspectComplete(
    _In_ PWSK_SOCKET ListenSocket,
    _In_ PWSK_INSPECT_ID InspectID,
    _In_ WSK_INSPECT_ACTION Action,
    _Inout_ PIRP Irp)
{
    FUNCTION_TRACE;
    UNREFERENCED_PARAMETER(ListenSocket);
    UNREFERENCED_PARAMETER(InspectID);
    UNREFERENCED_PARAMETER(Action);

    return WskCompleteUnsupported(Irp, __FUNCTION__);
}

static WSK_PROVIDER_LISTEN_DISPATCH TcpListenDispatch = {
    .WskControlSocket = WskControlSocket,
    .WskCloseSocket = WskCloseSocket,
    .WskBind = WskBind,
    .WskAccept = WskAccept,
    .WskInspectComplete = WskInspectComplete,
    .WskGetLocalAddress = WskGetLocalAddress,
};

static NTSTATUS WSKAPI
WskSocket(
    _In_ PWSK_CLIENT Client,
    _In_ ADDRESS_FAMILY AddressFamily,
    _In_ USHORT SocketType,
    _In_ ULONG Protocol,
    _In_ ULONG Flags,
    _In_opt_ PVOID SocketContext,
    _In_opt_ const VOID * Dispatch,
    _In_opt_ PEPROCESS OwningProcess,
    _In_opt_ PETHREAD OwningThread,
    _In_opt_ PSECURITY_DESCRIPTOR SecurityDescriptor,
    _Inout_ PIRP Irp)
{
    PWSK_SOCKET_INTERNAL Socket;
    NTSTATUS status;

    FUNCTION_TRACE;

    WSK_REQUIRE_IRP(Irp);
    IoSetNextIrpStackLocation(Irp);

    if (AddressFamily != AF_INET)
    {
        DPRINT1("Address family %d not supported\n", AddressFamily);
        status = STATUS_NOT_SUPPORTED;
        goto err_out;
    }
    switch (SocketType)
    {
        case SOCK_DGRAM:
            if (Protocol != IPPROTO_UDP)
            {
                DPRINT1("SOCK_DGRAM only supports IPPROTO_UDP\n");
                status = STATUS_INVALID_PARAMETER;
                goto err_out;
            }
            if (Flags != WSK_FLAG_DATAGRAM_SOCKET)
            {
                DPRINT1("SOCK_DGRAM flags must be WSK_FLAG_DATAGRAM_SOCKET\n");
                status = STATUS_INVALID_PARAMETER;
                goto err_out;
            }
            break;

        case SOCK_STREAM:
            if (Protocol != IPPROTO_TCP)
            {
                DPRINT1("SOCK_STREAM only supports IPPROTO_TCP\n");
                status = STATUS_INVALID_PARAMETER;
                goto err_out;
            }
            if ((Flags != WSK_FLAG_CONNECTION_SOCKET) && (Flags != WSK_FLAG_LISTEN_SOCKET))
            {
                DPRINT1("SOCK_STREAM flags must be either WSK_FLAG_CONNECTION_SOCKET or WSK_FLAG_LISTEN_SOCKET\n");
                status = STATUS_INVALID_PARAMETER;
                goto err_out;
            }
            break;

        case SOCK_RAW:
            DPRINT1("SOCK_RAW not supported\n");
            status = STATUS_NOT_SUPPORTED;
            goto err_out;

        default:
            status = STATUS_INVALID_PARAMETER;
            goto err_out;
    }

    Socket = ExAllocatePoolZero(NonPagedPool, sizeof(*Socket), TAG_NETIO);
    if (Socket == NULL)
    {
        DPRINT1("WskSocket: Out of memory\n");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_out;
    }
    Socket->family = AddressFamily;
    Socket->type = SocketType;
    Socket->proto = Protocol;
    Socket->WskFlags = Flags;
    Socket->user_context = SocketContext;
    Socket->ListenDispatch = Dispatch;
    Socket->RefCount = 1;            /* DereferenceSocket() is in WskCloseSocket */

    switch (SocketType)
    {
        case SOCK_DGRAM:
            Socket->Socket.Dispatch = &UdpDispatch;
            RtlInitUnicodeString(&Socket->TdiName, L"\\Device\\Udp");
            break;
        case SOCK_STREAM:
            Socket->Socket.Dispatch = (Flags == WSK_FLAG_LISTEN_SOCKET) ? (const VOID *)&TcpListenDispatch : (const VOID *)&TcpDispatch;
            RtlInitUnicodeString(&Socket->TdiName, L"\\Device\\Tcp");

            if (Flags != WSK_FLAG_LISTEN_SOCKET)
            {
                status = TdiOpenConnectionEndpointFile(&Socket->TdiName, &Socket->ConnectionHandle, &Socket->ConnectionFile);
                if (!NT_SUCCESS(status))
                {
                    DPRINT1("Could not open TDI handle, status is %x\n", status);
                    ExFreePoolWithTag(Socket, TAG_NETIO);
                    goto err_out;
                }
            }
            else
            {
                KeInitializeEvent(&Socket->StartListenEvent, SynchronizationEvent, FALSE);
                Socket->ListenThreadShouldRun = TRUE;
                KeInitializeSpinLock(&Socket->AcceptLock);
                KeInitializeSpinLock(&Socket->AcceptCsqLock);
                InitializeListHead(&Socket->AcceptIrpList);
                InitializeListHead(&Socket->AcceptedSockets);
                status = IoCsqInitialize(&Socket->AcceptCsq, AcceptCsqInsertIrp, AcceptCsqRemoveIrp, AcceptCsqPeekNextIrp,
                                         AcceptCsqAcquireLock, AcceptCsqReleaseLock, AcceptCsqCompleteCanceledIrp);
                if (!NT_SUCCESS(status))
                {
                    DPRINT1("Could not set up the accept queue, status is %x\n", status);
                    ExFreePoolWithTag(Socket, TAG_NETIO);
                    goto err_out;
                }
                Socket->AcceptQueueReady = TRUE;

                status = PsCreateSystemThread(&Socket->ListenThreadHandle, THREAD_ALL_ACCESS, NULL, NULL, NULL, RequeueListenThread, Socket);
                if (!NT_SUCCESS(status))
                {
                    DPRINT1("Could not start listen thread, status is %x\n", status);
                    ExFreePoolWithTag(Socket, TAG_NETIO);
                    goto err_out;
                }
                status = ObReferenceObjectByHandle(Socket->ListenThreadHandle, THREAD_ALL_ACCESS, NULL, KernelMode, (void **)&Socket->ListenThread, NULL);
                if (!NT_SUCCESS(status))
                {
                    DPRINT1("Could not get a PKTHREAD object, status is %x\n", status);
                    ExFreePoolWithTag(Socket, TAG_NETIO);
                    goto err_out;
                }
                ZwClose(Socket->ListenThreadHandle);
            }
            break;

        default:
            DPRINT1("Socket type not yet supported\n");
            status = STATUS_NOT_SUPPORTED;
            goto err_out;
    }

    Irp->IoStatus.Information = (ULONG_PTR)Socket;
    status = STATUS_SUCCESS;

err_out:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return STATUS_PENDING;
}

static WSK_PROVIDER_DISPATCH provider_dispatch = {
    .Version = 0,
    .Reserved = 0,
    .WskSocket = WskSocket,
    .WskSocketConnect = WskSocketConnect,
    .WskControlClient = WskControlClient
};

NTSTATUS WSKAPI
WskRegister(_In_ PWSK_CLIENT_NPI client_npi, _Out_ PWSK_REGISTRATION reg)
{
    FUNCTION_TRACE;

    reg->ReservedRegistrationState = 0;
    reg->ReservedRegistrationContext = NULL;
    KeInitializeSpinLock(&reg->ReservedRegistrationLock);

    return STATUS_SUCCESS;
}

NTSTATUS WSKAPI
WskCaptureProviderNPI(_In_ PWSK_REGISTRATION reg, _In_ ULONG wait, _Out_ PWSK_PROVIDER_NPI npi)
{
    FUNCTION_TRACE;

    npi->Client = NULL;
    npi->Dispatch = &provider_dispatch;

    return StartSocketDereferenceThread();
}

VOID WSKAPI
WskReleaseProviderNPI(_In_ PWSK_REGISTRATION reg)
{
    FUNCTION_TRACE;

    StopSocketDereferenceThread();
}

VOID WSKAPI
WskDeregister(_In_ PWSK_REGISTRATION reg)
{
    FUNCTION_TRACE;

    reg->ReservedRegistrationState = 0;
    reg->ReservedRegistrationContext = NULL;
}

NTSTATUS
NTAPI
NetioSetTriageBlock(VOID)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
NsiEnumerateObjectsAllParametersEx(VOID)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
NsiRegisterChangeNotificationEx(VOID)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
NsiSetAllParametersEx(VOID)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
NsiSetObjectSecurity(VOID)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
NsiSetParameterEx(VOID)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}
