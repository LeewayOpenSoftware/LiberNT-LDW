/*
 * PROJECT:     ReactOS Universal Serial Bus Bulk Storage Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     USB block storage device driver.
 * COPYRIGHT:   2005-2006 James Tabor
 *              2011-2012 Michael Martin (michael.martin@reactos.org)
 *              2011-2013 Johannes Anderwald (johannes.anderwald@reactos.org)
 *              2019 Victor Perevertkin (victor.perevertkin@reactos.org)
 */

#include "usbstor.h"

#define NDEBUG
#include <debug.h>


NTSTATUS
USBSTOR_GetEndpointStatus(
    IN PDEVICE_OBJECT DeviceObject,
    IN UCHAR bEndpointAddress,
    OUT PUSHORT Value)
{
    PURB Urb;
    NTSTATUS Status;

    DPRINT("Allocating URB\n");
    Urb = (PURB)AllocateItem(NonPagedPool, sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST));
    if (!Urb)
    {
        DPRINT1("OutofMemory!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // build status
    UsbBuildGetStatusRequest(Urb, URB_FUNCTION_GET_STATUS_FROM_ENDPOINT, bEndpointAddress & 0x0F, Value, NULL, NULL);

    // send the request
    Status = USBSTOR_SyncUrbRequest(DeviceObject, Urb);

    FreeItem(Urb);
    return Status;
}

NTSTATUS
USBSTOR_ResetPipeWithHandle(
    IN PDEVICE_OBJECT DeviceObject,
    IN USBD_PIPE_HANDLE PipeHandle)
{
    PURB Urb;
    NTSTATUS Status;

    DPRINT("Allocating URB\n");
    Urb = (PURB)AllocateItem(NonPagedPool, sizeof(struct _URB_PIPE_REQUEST));
    if (!Urb)
    {
        DPRINT1("OutofMemory!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Urb->UrbPipeRequest.Hdr.Length = sizeof(struct _URB_PIPE_REQUEST);
    Urb->UrbPipeRequest.Hdr.Function = URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL;
    Urb->UrbPipeRequest.PipeHandle = PipeHandle;

    // send the request
    Status = USBSTOR_SyncUrbRequest(DeviceObject, Urb);

    FreeItem(Urb);
    return Status;
}

NTSTATUS
USBSTOR_AbortPipeWithHandle(
    IN PDEVICE_OBJECT DeviceObject,
    IN USBD_PIPE_HANDLE PipeHandle)
{
    PURB Urb;
    NTSTATUS Status;

    Urb = (PURB)AllocateItem(NonPagedPool, sizeof(struct _URB_PIPE_REQUEST));
    if (!Urb)
        return STATUS_INSUFFICIENT_RESOURCES;

    Urb->UrbPipeRequest.Hdr.Length = sizeof(struct _URB_PIPE_REQUEST);
    Urb->UrbPipeRequest.Hdr.Function = URB_FUNCTION_ABORT_PIPE;
    Urb->UrbPipeRequest.PipeHandle = PipeHandle;

    // the host controller driver completes outstanding transfers locally;
    // this does not require the (now absent) device to respond
    Status = USBSTOR_SyncUrbRequest(DeviceObject, Urb);

    FreeItem(Urb);
    return Status;
}

VOID
USBSTOR_SurpriseRemoveDevice(
    IN PDEVICE_OBJECT FdoDevice)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension = FdoDevice->DeviceExtension;
    PUSBD_INTERFACE_INFORMATION Iface;
    KIRQL OldLevel;

    ASSERT(FDODeviceExtension->Common.IsFDO);

    /* Latch the gone state: the device is physically absent, so no request may
     * touch the hardware again. New and retried requests fail fast with
     * SRB_STATUS_NO_DEVICE (queue.c), and the BOT reset workers short-circuit
     * instead of issuing control transfers or a port reset to a device that will
     * never answer (error.c) - that unanswered recovery is what otherwise pins
     * the in-flight request forever, so its file-object reference never drops
     * and the PnP manager never sends IRP_MN_REMOVE_DEVICE. */
    KeAcquireSpinLock(&FDODeviceExtension->IrpListLock, &OldLevel);
    FDODeviceExtension->Flags |= USBSTOR_FDO_FLAGS_DEVICE_GONE;
    KeReleaseSpinLock(&FDODeviceExtension->IrpListLock, OldLevel);

    /* Retire the data phase in flight on the bulk pipes right now, so the
     * active request completes promptly and runs the gone-gated recovery
     * instead of waiting out its SCSI timeout. */
    Iface = FDODeviceExtension->InterfaceInformation;
    if (Iface != NULL)
    {
        USBSTOR_AbortPipeWithHandle(FDODeviceExtension->LowerDeviceObject,
                                    Iface->Pipes[FDODeviceExtension->BulkInPipeIndex].PipeHandle);
        USBSTOR_AbortPipeWithHandle(FDODeviceExtension->LowerDeviceObject,
                                    Iface->Pipes[FDODeviceExtension->BulkOutPipeIndex].PipeHandle);
    }
}

VOID
NTAPI
USBSTOR_ResetPipeWorkItemRoutine(
    IN PDEVICE_OBJECT FdoDevice,
    IN PVOID Ctx)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension = (PFDO_DEVICE_EXTENSION)Ctx;
    PIRP_CONTEXT Context = &FDODeviceExtension->CurrentIrpContext;
    NTSTATUS Status;

    // clear stall on the corresponding pipe
    KeMemoryBarrier();
    if (FDODeviceExtension->Flags & USBSTOR_FDO_FLAGS_DEVICE_GONE)
        Status = STATUS_DEVICE_DOES_NOT_EXIST;   /* gone: do not touch the pipe, take the failure path */
    else
        Status = USBSTOR_ResetPipeWithHandle(FDODeviceExtension->LowerDeviceObject, Context->Urb.UrbBulkOrInterruptTransfer.PipeHandle);
    if (!NT_SUCCESS(Status))
    {
        PIRP Irp = Context->Irp;
        PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
        PSCSI_REQUEST_BLOCK Request = FDODeviceExtension->ActiveSrb;

        /* Do not issue a CSW while the pipe is still halted or its host and
         * device protocol state differ. Restore the original SRB if this
         * was auto-sense, then restart through full BOT reset recovery. */
        IoStack->Parameters.Scsi.Srb = Request;
        Irp->IoStatus.Information = 0;
        if (USBSTOR_IsRequestTimedOut(FDODeviceExtension, Irp))
        {
            Request->SrbStatus = SRB_STATUS_TIMEOUT;
            Irp->IoStatus.Status = STATUS_IO_TIMEOUT;
        }
        else
        {
            Request->SrbStatus = SRB_STATUS_BUS_RESET;
            Irp->IoStatus.Status = STATUS_IO_DEVICE_ERROR;
        }

        if (USBSTOR_FinishRequest(FDODeviceExtension, Irp, TRUE))
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return;
    }

    // now resend the csw as the stall got cleared
    USBSTOR_SendCSWRequest(FDODeviceExtension, Context->Irp);
}

VOID
NTAPI
USBSTOR_ResetDeviceWorkItemRoutine(
    IN PDEVICE_OBJECT FdoDevice,
    IN PVOID Context)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    UINT32 ix;
    NTSTATUS Status;
    BOOLEAN SingleInterface;

    DPRINT("USBSTOR_ResetDeviceWorkItemRoutine\n");

    FDODeviceExtension = FdoDevice->DeviceExtension;

    DPRINT1("PNPDIAG usbstor resetdev fdo %p pending %lu active %p flags 0x%lx\n",
            FdoDevice, FDODeviceExtension->IrpPendingCount,
            FDODeviceExtension->ActiveSrb, FDODeviceExtension->Flags);

    KeMemoryBarrier();
    if (FDODeviceExtension->Flags & USBSTOR_FDO_FLAGS_DEVICE_GONE)
    {
        /* The device is gone; a BOT reset or port reset would block on a device
         * that cannot answer. Fail every pending request instead. */
        USBSTOR_QueueEndReset(FdoDevice, STATUS_DEVICE_DOES_NOT_EXIST);
        return;
    }

    SingleInterface = (FDODeviceExtension->ConfigurationDescriptor &&
                       FDODeviceExtension->ConfigurationDescriptor->bNumInterfaces == 1);

    if (SingleInterface && InterlockedExchange(&FDODeviceExtension->BotResetPending, 0))
        goto PortReset;

    for (ix = 0; ix < 3; ++ix)
    {
        // first perform a mass storage reset step 1 in 5.3.4 USB Mass Storage Bulk Only Specification
        Status = USBSTOR_ResetDevice(FDODeviceExtension->LowerDeviceObject, FDODeviceExtension);
        if (NT_SUCCESS(Status))
        {
            // step 2 reset bulk in pipe section 5.3.4
            Status = USBSTOR_ResetPipeWithHandle(FDODeviceExtension->LowerDeviceObject, FDODeviceExtension->InterfaceInformation->Pipes[FDODeviceExtension->BulkInPipeIndex].PipeHandle);
            if (NT_SUCCESS(Status))
            {
                // finally reset bulk out pipe
                Status = USBSTOR_ResetPipeWithHandle(FDODeviceExtension->LowerDeviceObject, FDODeviceExtension->InterfaceInformation->Pipes[FDODeviceExtension->BulkOutPipeIndex].PipeHandle);
                if (NT_SUCCESS(Status))
                {
                    break;
                }
            }
        }
    }

    if (NT_SUCCESS(Status) || !SingleInterface)
    {
        if (NT_SUCCESS(Status) && SingleInterface)
            InterlockedExchange(&FDODeviceExtension->BotResetPending, 1);
        goto Exit;
    }

PortReset:
    /* A device may acknowledge a BOT reset without recovering an aborted
     * data phase. Reset the port while the request queue is frozen; the hub
     * restores the configuration and retains our pipe handles. Limit this
     * to devices owned entirely by USBSTOR so other interfaces keep running. */
    Status = USBSTOR_SyncInternalRequest(FDODeviceExtension->LowerDeviceObject,
                                        IOCTL_INTERNAL_USB_RESET_PORT,
                                        NULL);
    /* A failed port reset may leave the device unaddressed. Preserve
     * the failure so the queue retries full recovery on the next request;
     * a class request cannot use the old address or configuration. */

Exit:
    USBSTOR_QueueEndReset(FdoDevice, Status);
}

VOID
NTAPI
USBSTOR_QueueResetPipe(
    IN PFDO_DEVICE_EXTENSION FDODeviceExtension)
{
    DPRINT("USBSTOR_QueueResetPipe\n");

    IoQueueWorkItem(FDODeviceExtension->ResetDeviceWorkItem,
                    USBSTOR_ResetPipeWorkItemRoutine,
                    CriticalWorkQueue,
                    FDODeviceExtension);
}

VOID
NTAPI
USBSTOR_QueueResetDevice(
    IN PFDO_DEVICE_EXTENSION FDODeviceExtension)
{
    DPRINT("USBSTOR_QueueResetDevice\n");

    IoQueueWorkItem(FDODeviceExtension->ResetDeviceWorkItem,
                    USBSTOR_ResetDeviceWorkItemRoutine,
                    CriticalWorkQueue,
                    NULL);
}

static
VOID
USBSTOR_FinalizeRequest(
    IN PFDO_DEVICE_EXTENSION FDODeviceExtension,
    IN PIRP Irp,
    IN BOOLEAN ResetDevice,
    IN BOOLEAN CompleteIrp)
{
    PDEVICE_OBJECT FdoDevice = FDODeviceExtension->FunctionalDeviceObject;

    USBSTOR_QueueTerminateRequest(FdoDevice, Irp, ResetDevice);

    if (ResetDevice)
        USBSTOR_QueueResetDevice(FDODeviceExtension);
    else
        USBSTOR_QueueNextRequest(FdoDevice);

    if (CompleteIrp)
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static
VOID
NTAPI
USBSTOR_RequestTimeoutDpc(
    IN PKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension = DeferredContext;
    PIRP Irp;
    PIRP DeferredIrp = NULL;
    BOOLEAN DeferredReset = FALSE;
    BOOLEAN Cancelled;
    LARGE_INTEGER RetryTime;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    KeAcquireSpinLock(&FDODeviceExtension->RequestTimerLock, &OldIrql);
    Irp = FDODeviceExtension->RequestTimerIrp;
    if (!Irp)
    {
        KeReleaseSpinLock(&FDODeviceExtension->RequestTimerLock, OldIrql);
        return;
    }

    FDODeviceExtension->RequestTimerDpcRunning = TRUE;
    if (!FDODeviceExtension->RequestTimedOut)
    {
        FDODeviceExtension->RequestTimedOut = TRUE;
        DPRINT1("USBSTOR: request %p timed out after %lu seconds\n",
                Irp,
                FDODeviceExtension->RequestTimeoutValue);
    }
    KeReleaseSpinLock(&FDODeviceExtension->RequestTimerLock, OldIrql);

    /* The lower USB stack owns the current cancel routine.  Cancellation
     * retires DMA before the normal BOT error path resets the device. */
    Cancelled = IoCancelIrp(Irp);

    KeAcquireSpinLock(&FDODeviceExtension->RequestTimerLock, &OldIrql);
    FDODeviceExtension->RequestTimerDpcRunning = FALSE;

    if (FDODeviceExtension->DeferredCompletionIrp)
    {
        DeferredIrp = FDODeviceExtension->DeferredCompletionIrp;
        DeferredReset = FDODeviceExtension->DeferredCompletionReset;
        FDODeviceExtension->DeferredCompletionIrp = NULL;
        FDODeviceExtension->RequestTimedOut = FALSE;
        FDODeviceExtension->RequestTimeoutValue = 0;
    }
    else if (!Cancelled && FDODeviceExtension->RequestTimerIrp == Irp)
    {
        /* A completion routine may be between BOT phases with no lower cancel
         * routine installed.  Retry after it either submits the next phase or
         * finishes the request. */
        RetryTime.QuadPart = -10LL * 10000;
        KeSetTimer(&FDODeviceExtension->RequestTimer,
                   RetryTime,
                   &FDODeviceExtension->RequestTimerDpc);
    }

    KeReleaseSpinLock(&FDODeviceExtension->RequestTimerLock, OldIrql);

    if (DeferredIrp)
        USBSTOR_FinalizeRequest(FDODeviceExtension, DeferredIrp, DeferredReset, TRUE);
}

VOID
USBSTOR_InitializeRequestTimer(
    IN PFDO_DEVICE_EXTENSION FDODeviceExtension)
{
    KeInitializeSpinLock(&FDODeviceExtension->RequestTimerLock);
    KeInitializeTimer(&FDODeviceExtension->RequestTimer);
    KeInitializeDpc(&FDODeviceExtension->RequestTimerDpc,
                    USBSTOR_RequestTimeoutDpc,
                    FDODeviceExtension);
}

VOID
USBSTOR_StartRequestTimer(
    IN PFDO_DEVICE_EXTENSION FDODeviceExtension,
    IN PIRP Irp,
    IN ULONG TimeOutValue)
{
    LARGE_INTEGER DueTime;
    KIRQL OldIrql;

    KeAcquireSpinLock(&FDODeviceExtension->RequestTimerLock, &OldIrql);
    ASSERT(FDODeviceExtension->RequestTimerIrp == NULL);
    ASSERT(FDODeviceExtension->DeferredCompletionIrp == NULL);

    if (FDODeviceExtension->RequestTimerIrp != NULL)
    {
        KeReleaseSpinLock(&FDODeviceExtension->RequestTimerLock, OldIrql);
        return;
    }

    FDODeviceExtension->RequestTimerIrp = Irp;
    FDODeviceExtension->RequestTimedOut = FALSE;
    FDODeviceExtension->RequestTimeoutValue = TimeOutValue;

    if (TimeOutValue != 0)
    {
        DueTime.QuadPart = -(LONGLONG)TimeOutValue * 1000 * 1000 * 10;
        KeSetCoalescableTimer(&FDODeviceExtension->RequestTimer,
                             DueTime,
                             0,
                             500,
                             &FDODeviceExtension->RequestTimerDpc);
    }

    KeReleaseSpinLock(&FDODeviceExtension->RequestTimerLock, OldIrql);
}

BOOLEAN
USBSTOR_IsRequestTimedOut(
    IN PFDO_DEVICE_EXTENSION FDODeviceExtension,
    IN PIRP Irp)
{
    BOOLEAN TimedOut;
    KIRQL OldIrql;

    KeAcquireSpinLock(&FDODeviceExtension->RequestTimerLock, &OldIrql);
    TimedOut = (FDODeviceExtension->RequestTimerIrp == Irp &&
                FDODeviceExtension->RequestTimedOut);
    KeReleaseSpinLock(&FDODeviceExtension->RequestTimerLock, OldIrql);

    return TimedOut;
}

BOOLEAN
USBSTOR_FinishRequest(
    IN PFDO_DEVICE_EXTENSION FDODeviceExtension,
    IN PIRP Irp,
    IN BOOLEAN ResetDevice)
{
    BOOLEAN Deferred = FALSE;
    KIRQL OldIrql;

    KeAcquireSpinLock(&FDODeviceExtension->RequestTimerLock, &OldIrql);

    if (FDODeviceExtension->RequestTimerIrp == Irp)
    {
        KeCancelTimer(&FDODeviceExtension->RequestTimer);
        KeRemoveQueueDpc(&FDODeviceExtension->RequestTimerDpc);
        FDODeviceExtension->RequestTimerIrp = NULL;

        if (FDODeviceExtension->RequestTimerDpcRunning)
        {
            ASSERT(FDODeviceExtension->DeferredCompletionIrp == NULL);
            FDODeviceExtension->DeferredCompletionIrp = Irp;
            FDODeviceExtension->DeferredCompletionReset = ResetDevice;
            Deferred = TRUE;
        }
        else
        {
            FDODeviceExtension->RequestTimedOut = FALSE;
            FDODeviceExtension->RequestTimeoutValue = 0;
        }
    }

    KeReleaseSpinLock(&FDODeviceExtension->RequestTimerLock, OldIrql);

    if (!Deferred)
        USBSTOR_FinalizeRequest(FDODeviceExtension, Irp, ResetDevice, FALSE);

    return !Deferred;
}
