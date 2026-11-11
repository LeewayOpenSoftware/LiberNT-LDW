/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     WSK stream and datagram echo over loopback, cancellation and graceful disconnect
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>
#include <wsk.h>
#define NDEBUG
#include <debug.h>
#include "wsk_echo.h"

#define TAG_WSKECHO 'EksW'

static KMT_MESSAGE_HANDLER TestMessageHandler;
static const WSK_CLIENT_DISPATCH ClientDispatch = { MAKE_WSK_VERSION(1, 0), 0, NULL };

typedef struct _ECHO_BUFFER
{
    PVOID Memory;
    PMDL Mdl;
    WSK_BUF Buf;
} ECHO_BUFFER, *PECHO_BUFFER;

static
NTSTATUS
NTAPI
IrpComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
PIRP
NewIrp(
    _Out_ PKEVENT Event)
{
    PIRP Irp = IoAllocateIrp(1, FALSE);

    KeInitializeEvent(Event, NotificationEvent, FALSE);
    if (Irp != NULL)
    {
        IoSetCompletionRoutine(Irp, IrpComplete, Event, TRUE, TRUE, TRUE);
    }
    return Irp;
}

static
NTSTATUS
WaitIrp(
    _In_ PCSTR Kind,
    _In_ PIRP Irp,
    _In_ PKEVENT Event,
    _In_ NTSTATUS Status,
    _Out_opt_ PULONG_PTR Information)
{
    if (Status == STATUS_PENDING)
    {
        LARGE_INTEGER Timeout;

        Timeout.QuadPart = -(LONGLONG)WSKECHO_IRP_TIMEOUT_MS * 10000;
        if (KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, &Timeout) == STATUS_TIMEOUT)
        {
            ok(FALSE, "%s did not complete within %u ms; cancelled, now waiting without limit for the provider to complete it\n", Kind, WSKECHO_IRP_TIMEOUT_MS);
            IoCancelIrp(Irp);
            KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, NULL);
            trace("%s completed after cancel with 0x%08lx\n", Kind, Irp->IoStatus.Status);
        }
    }
    if (NT_SUCCESS(Status) || Status == STATUS_PENDING)
    {
        Status = Irp->IoStatus.Status;
    }
    if (Information != NULL)
    {
        *Information = Irp->IoStatus.Information;
    }
    return Status;
}

static
BOOLEAN
AllocateBuffer(
    _Out_ PECHO_BUFFER Buffer,
    _In_ ULONG Length)
{
    RtlZeroMemory(Buffer, sizeof(*Buffer));
    Buffer->Memory = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_WSKECHO);
    if (Buffer->Memory == NULL)
    {
        return FALSE;
    }
    Buffer->Mdl = IoAllocateMdl(Buffer->Memory, Length, FALSE, FALSE, NULL);
    if (Buffer->Mdl == NULL)
    {
        ExFreePoolWithTag(Buffer->Memory, TAG_WSKECHO);
        Buffer->Memory = NULL;
        return FALSE;
    }
    MmBuildMdlForNonPagedPool(Buffer->Mdl);
    Buffer->Buf.Mdl = Buffer->Mdl;
    Buffer->Buf.Offset = 0;
    Buffer->Buf.Length = Length;
    return TRUE;
}

static
VOID
FreeBuffer(
    _Inout_ PECHO_BUFFER Buffer)
{
    if (Buffer->Mdl != NULL)
    {
        IoFreeMdl(Buffer->Mdl);
    }
    if (Buffer->Memory != NULL)
    {
        ExFreePoolWithTag(Buffer->Memory, TAG_WSKECHO);
    }
    RtlZeroMemory(Buffer, sizeof(*Buffer));
}

static
VOID
FillPattern(
    _Out_writes_bytes_(Length) PUCHAR Memory,
    _In_ ULONG Length,
    _In_ UCHAR Seed)
{
    ULONG i;

    for (i = 0; i < Length; i++)
    {
        Memory[i] = (UCHAR)(Seed + i * 7);
    }
}

static
PWSK_SOCKET
CreateSocket(
    _In_ PWSK_PROVIDER_NPI Npi,
    _In_ ADDRESS_FAMILY Family,
    _In_ USHORT Type,
    _In_ ULONG Protocol,
    _In_ ULONG Flags)
{
    PWSK_SOCKET Socket = NULL;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp = NewIrp(&Event);
    ULONG_PTR Information = 0;

    if (Irp == NULL)
    {
        return NULL;
    }
    Status = Npi->Dispatch->WskSocket(Npi->Client, Family, Type, Protocol, Flags, NULL, NULL, NULL, NULL, NULL, Irp);
    Status = WaitIrp("WskSocket", Irp, &Event, Status, &Information);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Socket = (PWSK_SOCKET)Information;
    }
    IoFreeIrp(Irp);
    return Socket;
}

static
NTSTATUS
BindLoopback(
    _In_ PWSK_SOCKET Socket,
    _In_ ULONG Flags,
    _Out_ PSOCKADDR_IN Address)
{
    const WSK_PROVIDER_BASIC_DISPATCH *Basic = Socket->Dispatch;
    PFN_WSK_BIND Bind;
    PFN_WSK_GET_LOCAL_ADDRESS GetLocalAddress;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    switch (Flags)
    {
        case WSK_FLAG_LISTEN_SOCKET:
            Bind = ((const WSK_PROVIDER_LISTEN_DISPATCH *)Basic)->WskBind;
            GetLocalAddress = ((const WSK_PROVIDER_LISTEN_DISPATCH *)Basic)->WskGetLocalAddress;
            break;
        case WSK_FLAG_CONNECTION_SOCKET:
            Bind = ((const WSK_PROVIDER_CONNECTION_DISPATCH *)Basic)->WskBind;
            GetLocalAddress = ((const WSK_PROVIDER_CONNECTION_DISPATCH *)Basic)->WskGetLocalAddress;
            break;
        default:
            Bind = ((const WSK_PROVIDER_DATAGRAM_DISPATCH *)Basic)->WskBind;
            GetLocalAddress = ((const WSK_PROVIDER_DATAGRAM_DISPATCH *)Basic)->WskGetLocalAddress;
            break;
    }

    RtlZeroMemory(Address, sizeof(*Address));
    Address->sin_family = AF_INET;
    Address->sin_addr.s_addr = RtlUlongByteSwap(0x7f000001);
    Irp = NewIrp(&Event);
    if (Irp == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = Bind(Socket, (PSOCKADDR)Address, 0, Irp);
    Status = WaitIrp("WskBind", Irp, &Event, Status, NULL);
    IoFreeIrp(Irp);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }
    Irp = NewIrp(&Event);
    if (Irp == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = GetLocalAddress(Socket, (PSOCKADDR)Address, Irp);
    Status = WaitIrp("WskGetLocalAddress", Irp, &Event, Status, NULL);
    IoFreeIrp(Irp);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Address->sin_port != 0, "bound port is 0\n");
    return Status;
}

static
NTSTATUS
CloseSocket(
    _In_ PWSK_SOCKET Socket)
{
    const WSK_PROVIDER_BASIC_DISPATCH *Basic = Socket->Dispatch;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp = NewIrp(&Event);

    if (Irp == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = Basic->WskCloseSocket(Socket, Irp);
    Status = WaitIrp("WskCloseSocket", Irp, &Event, Status, NULL);
    IoFreeIrp(Irp);
    ok_eq_hex(Status, STATUS_SUCCESS);
    return Status;
}

static
NTSTATUS
StreamTransfer(
    _In_ PWSK_SOCKET Sender,
    _In_ PWSK_SOCKET Receiver,
    _In_ PECHO_BUFFER Out,
    _In_ PECHO_BUFFER In,
    _In_ ULONG Length)
{
    const WSK_PROVIDER_CONNECTION_DISPATCH *SendDispatch = Sender->Dispatch;
    const WSK_PROVIDER_CONNECTION_DISPATCH *RecvDispatch = Receiver->Dispatch;
    KEVENT SendEvent, RecvEvent;
    PIRP SendIrp, RecvIrp;
    NTSTATUS Status, RecvStatus;
    ULONG_PTR Information = 0;
    ULONG Received = 0;
    LARGE_INTEGER Start, End, Frequency;
    WSK_BUF SendBuf = Out->Buf;

    SendBuf.Length = Length;
    SendIrp = NewIrp(&SendEvent);
    if (SendIrp == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Start = KeQueryPerformanceCounter(&Frequency);
    Status = SendDispatch->WskSend(Sender, &SendBuf, 0, SendIrp);
    while (Received < Length)
    {
        WSK_BUF Partial = In->Buf;

        Partial.Offset = Received;
        Partial.Length = Length - Received;
        RecvIrp = NewIrp(&RecvEvent);
        if (RecvIrp == NULL)
        {
            break;
        }
        RecvStatus = RecvDispatch->WskReceive(Receiver, &Partial, 0, RecvIrp);
        RecvStatus = WaitIrp("WskReceive", RecvIrp, &RecvEvent, RecvStatus, &Information);
        IoFreeIrp(RecvIrp);
        if (!NT_SUCCESS(RecvStatus) || Information == 0)
        {
            ok(FALSE, "receive stopped with 0x%08lx after %lu bytes\n", RecvStatus, Received);
            break;
        }
        Received += (ULONG)Information;
    }
    Status = WaitIrp("WskSend", SendIrp, &SendEvent, Status, &Information);
    End = KeQueryPerformanceCounter(NULL);
    IoFreeIrp(SendIrp);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong((ULONG)Information, Length);
    ok_eq_ulong(Received, Length);
    ok(RtlCompareMemory(Out->Memory, In->Memory, Length) == Length, "echoed stream data differs\n");
    trace("%lu stream bytes in %I64d us\n", Length, (End.QuadPart - Start.QuadPart) * 1000000 / Frequency.QuadPart);
    return Status;
}

static
VOID
TestStream(
    _In_ PWSK_PROVIDER_NPI Npi)
{
    PWSK_SOCKET Listener, Client, Accepted = NULL;
    const WSK_PROVIDER_LISTEN_DISPATCH *ListenDispatch;
    const WSK_PROVIDER_CONNECTION_DISPATCH *ClientDispatch;
    SOCKADDR_IN ListenAddress, ClientAddress;
    KEVENT AcceptEvent, ConnectEvent, RecvEvent;
    PIRP AcceptIrp, ConnectIrp, RecvIrp;
    NTSTATUS AcceptStatus, ConnectStatus, Status;
    ULONG_PTR Information = 0;
    ECHO_BUFFER Out = { 0 }, In = { 0 };
    LARGE_INTEGER Delay;

    Listener = CreateSocket(Npi, AF_INET, SOCK_STREAM, IPPROTO_TCP, WSK_FLAG_LISTEN_SOCKET);
    Client = CreateSocket(Npi, AF_INET, SOCK_STREAM, IPPROTO_TCP, WSK_FLAG_CONNECTION_SOCKET);
    if (Listener == NULL || Client == NULL)
    {
        skip(FALSE, "stream sockets not created\n");
        goto done;
    }
    ListenDispatch = Listener->Dispatch;
    ClientDispatch = Client->Dispatch;
    if (!NT_SUCCESS(BindLoopback(Listener, WSK_FLAG_LISTEN_SOCKET, &ListenAddress)) ||
        !NT_SUCCESS(BindLoopback(Client, WSK_FLAG_CONNECTION_SOCKET, &ClientAddress)))
    {
        goto done;
    }

    AcceptIrp = NewIrp(&AcceptEvent);
    ConnectIrp = NewIrp(&ConnectEvent);
    if (AcceptIrp == NULL || ConnectIrp == NULL)
    {
        goto done;
    }
    AcceptStatus = ListenDispatch->WskAccept(Listener, 0, NULL, NULL, NULL, NULL, AcceptIrp);
    ConnectStatus = ClientDispatch->WskConnect(Client, (PSOCKADDR)&ListenAddress, 0, ConnectIrp);
    ConnectStatus = WaitIrp("WskConnect", ConnectIrp, &ConnectEvent, ConnectStatus, NULL);
    ok_eq_hex(ConnectStatus, STATUS_SUCCESS);
    AcceptStatus = WaitIrp("WskAccept", AcceptIrp, &AcceptEvent, AcceptStatus, &Information);
    ok_eq_hex(AcceptStatus, STATUS_SUCCESS);
    if (NT_SUCCESS(AcceptStatus))
    {
        Accepted = (PWSK_SOCKET)Information;
    }
    IoFreeIrp(AcceptIrp);
    IoFreeIrp(ConnectIrp);
    if (Accepted == NULL || !NT_SUCCESS(ConnectStatus))
    {
        goto done;
    }

    if (AllocateBuffer(&Out, WSKECHO_LARGE_STREAM_BYTES) && AllocateBuffer(&In, WSKECHO_LARGE_STREAM_BYTES))
    {
        FillPattern(Out.Memory, WSKECHO_STREAM_BYTES, 0x11);
        RtlZeroMemory(In.Memory, WSKECHO_STREAM_BYTES);
        StreamTransfer(Client, Accepted, &Out, &In, WSKECHO_STREAM_BYTES);
        FillPattern(Out.Memory, WSKECHO_STREAM_BYTES, 0x77);
        RtlZeroMemory(In.Memory, WSKECHO_STREAM_BYTES);
        StreamTransfer(Accepted, Client, &Out, &In, WSKECHO_STREAM_BYTES);
        FillPattern(Out.Memory, WSKECHO_LARGE_STREAM_BYTES, 0x55);
        RtlZeroMemory(In.Memory, WSKECHO_LARGE_STREAM_BYTES);
        StreamTransfer(Client, Accepted, &Out, &In, WSKECHO_LARGE_STREAM_BYTES);

        RecvIrp = NewIrp(&RecvEvent);
        if (RecvIrp != NULL)
        {
            Status = ClientDispatch->WskReceive(Client, &In.Buf, 0, RecvIrp);
            ok_eq_hex(Status, STATUS_PENDING);
            Delay.QuadPart = -200 * 10000;
            KeDelayExecutionThread(KernelMode, FALSE, &Delay);
            ok(IoCancelIrp(RecvIrp), "IoCancelIrp found no cancel routine on a pending receive\n");
            Status = WaitIrp("WskReceive", RecvIrp, &RecvEvent, STATUS_PENDING, &Information);
            ok_eq_hex(Status, STATUS_CANCELLED);
            IoFreeIrp(RecvIrp);
        }

        RecvIrp = NewIrp(&RecvEvent);
        if (RecvIrp != NULL)
        {
            KEVENT DisconnectEvent;
            PIRP DisconnectIrp = NewIrp(&DisconnectEvent);

            Status = ((const WSK_PROVIDER_CONNECTION_DISPATCH *)Accepted->Dispatch)->WskReceive(Accepted, &In.Buf, 0, RecvIrp);
            if (DisconnectIrp != NULL)
            {
                NTSTATUS DisconnectStatus = ClientDispatch->WskDisconnect(Client, NULL, 0, DisconnectIrp);

                DisconnectStatus = WaitIrp("WskDisconnect", DisconnectIrp, &DisconnectEvent, DisconnectStatus, NULL);
                ok_eq_hex(DisconnectStatus, STATUS_SUCCESS);
                IoFreeIrp(DisconnectIrp);
            }
            Status = WaitIrp("WskReceive", RecvIrp, &RecvEvent, Status, &Information);
            trace("receive after the peer's graceful disconnect: 0x%08lx, %Iu bytes\n", Status, Information);
            ok(NT_SUCCESS(Status) && Information == 0, "graceful disconnect did not complete the receive with 0 bytes\n");
            IoFreeIrp(RecvIrp);
        }
    }
    else
    {
        skip(FALSE, "no stream buffers\n");
    }
    FreeBuffer(&Out);
    FreeBuffer(&In);

done:
    if (Accepted != NULL)
    {
        CloseSocket(Accepted);
    }
    if (Client != NULL)
    {
        CloseSocket(Client);
    }
    if (Listener != NULL)
    {
        CloseSocket(Listener);
    }
}

static
VOID
TestDatagram(
    _In_ PWSK_PROVIDER_NPI Npi)
{
    PWSK_SOCKET A, B;
    const WSK_PROVIDER_DATAGRAM_DISPATCH *DispatchA, *DispatchB;
    SOCKADDR_IN AddressA, AddressB, From;
    KEVENT SendEvent, RecvEvent;
    PIRP SendIrp, RecvIrp;
    NTSTATUS SendStatus, RecvStatus;
    ULONG_PTR Information = 0;
    ULONG ControlLength = 0, ControlFlags = 0;
    ECHO_BUFFER Out = { 0 }, In = { 0 };

    A = CreateSocket(Npi, AF_INET, SOCK_DGRAM, IPPROTO_UDP, WSK_FLAG_DATAGRAM_SOCKET);
    B = CreateSocket(Npi, AF_INET, SOCK_DGRAM, IPPROTO_UDP, WSK_FLAG_DATAGRAM_SOCKET);
    if (A == NULL || B == NULL)
    {
        skip(FALSE, "datagram sockets not created\n");
        goto done;
    }
    DispatchA = A->Dispatch;
    DispatchB = B->Dispatch;
    if (!NT_SUCCESS(BindLoopback(A, WSK_FLAG_DATAGRAM_SOCKET, &AddressA)) ||
        !NT_SUCCESS(BindLoopback(B, WSK_FLAG_DATAGRAM_SOCKET, &AddressB)))
    {
        goto done;
    }
    if (!AllocateBuffer(&Out, WSKECHO_DATAGRAM_BYTES) || !AllocateBuffer(&In, WSKECHO_DATAGRAM_BYTES))
    {
        skip(FALSE, "no datagram buffers\n");
        FreeBuffer(&Out);
        FreeBuffer(&In);
        goto done;
    }
    FillPattern(Out.Memory, WSKECHO_DATAGRAM_BYTES, 0x33);
    RtlZeroMemory(In.Memory, WSKECHO_DATAGRAM_BYTES);
    RtlZeroMemory(&From, sizeof(From));

    RecvIrp = NewIrp(&RecvEvent);
    SendIrp = NewIrp(&SendEvent);
    if (RecvIrp != NULL && SendIrp != NULL)
    {
        RecvStatus = DispatchB->WskReceiveFrom(B, &In.Buf, 0, (PSOCKADDR)&From, &ControlLength, NULL, &ControlFlags, RecvIrp);
        SendStatus = DispatchA->WskSendTo(A, &Out.Buf, 0, (PSOCKADDR)&AddressB, 0, NULL, SendIrp);
        SendStatus = WaitIrp("WskSendTo", SendIrp, &SendEvent, SendStatus, &Information);
        ok_eq_hex(SendStatus, STATUS_SUCCESS);
        ok_eq_ulong((ULONG)Information, (ULONG)WSKECHO_DATAGRAM_BYTES);
        RecvStatus = WaitIrp("WskReceiveFrom", RecvIrp, &RecvEvent, RecvStatus, &Information);
        ok_eq_hex(RecvStatus, STATUS_SUCCESS);
        ok_eq_ulong((ULONG)Information, (ULONG)WSKECHO_DATAGRAM_BYTES);
        ok(RtlCompareMemory(Out.Memory, In.Memory, WSKECHO_DATAGRAM_BYTES) == WSKECHO_DATAGRAM_BYTES, "datagram payload differs\n");
        ok(From.sin_family == AF_INET && From.sin_port == AddressA.sin_port, "datagram source port %u, expected %u\n",
           RtlUshortByteSwap(From.sin_port), RtlUshortByteSwap(AddressA.sin_port));
    }
    if (RecvIrp != NULL)
    {
        IoFreeIrp(RecvIrp);
    }
    if (SendIrp != NULL)
    {
        IoFreeIrp(SendIrp);
    }
    FreeBuffer(&Out);
    FreeBuffer(&In);

done:
    if (A != NULL)
    {
        CloseSocket(A);
    }
    if (B != NULL)
    {
        CloseSocket(B);
    }
}

static
VOID
TestRun(VOID)
{
    WSK_CLIENT_NPI ClientNpi;
    WSK_PROVIDER_NPI ProviderNpi;
    WSK_REGISTRATION Registration;
    NTSTATUS Status;

    ClientNpi.ClientContext = NULL;
    ClientNpi.Dispatch = &ClientDispatch;
    Status = WskRegister(&ClientNpi, &Registration);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        return;
    }
    Status = WskCaptureProviderNPI(&Registration, WSKECHO_CAPTURE_TIMEOUT_MS, &ProviderNpi);
    ok(NT_SUCCESS(Status), "WskCaptureProviderNPI returned 0x%08lx within %u ms (STATUS_NOINTERFACE = no provider, STATUS_TIMEOUT = provider never attached)\n",
       Status, WSKECHO_CAPTURE_TIMEOUT_MS);
    if (NT_SUCCESS(Status))
    {
        TestStream(&ProviderNpi);
        TestDatagram(&ProviderNpi);
        WskReleaseProviderNPI(&Registration);
    }
    WskDeregister(&Registration);
}

NTSTATUS
TestEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PCUNICODE_STRING RegistryPath,
    OUT PCWSTR *DeviceName,
    IN OUT INT *Flags)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    UNREFERENCED_PARAMETER(Flags);
    *DeviceName = L"WskEcho";
    KmtRegisterMessageHandler(0, NULL, TestMessageHandler);
    return STATUS_SUCCESS;
}

VOID
TestUnload(
    IN PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(DriverObject);
}

static
NTSTATUS
TestMessageHandler(
    IN PDEVICE_OBJECT DeviceObject,
    IN ULONG ControlCode,
    IN PVOID Buffer OPTIONAL,
    IN SIZE_T InLength,
    IN OUT PSIZE_T OutLength)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(InLength);
    UNREFERENCED_PARAMETER(OutLength);
    if (ControlCode != IOCTL_WSKECHO_RUN)
    {
        return STATUS_NOT_SUPPORTED;
    }
    TestRun();
    return STATUS_SUCCESS;
}
