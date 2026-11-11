/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NTFS filesystem driver
 * COPYRIGHT:   Copyright 2024 Carl Bialorucki <carl.bialorucki@reactos.org>
 *              Copyright 2024 Justin Miller <justin.miller@reactos.org>
 */

#include "ntfslib_new.h"
#include "ntfslib_new_internal.h"

NTSTATUS
Volume::ReadVolume(_In_    ULONGLONG Offset,
                   _In_    ULONG Length,
                   _Inout_ PUCHAR Buffer)
{
    ULONGLONG VolumeBytes;

    if (BytesPerSector == 0 ||
        SectorsInVolume > (ULONGLONG)-1 / BytesPerSector)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    VolumeBytes = SectorsInVolume * BytesPerSector;
    if (Offset > VolumeBytes || Length > VolumeBytes - Offset)
        return STATUS_END_OF_FILE;

    return NtfsReadVolumeContext(IoContext,
                                 BytesPerSector,
                                 Offset,
                                 Length,
                                 Buffer);
}

NTSTATUS
Volume::WriteVolume(_In_    ULONGLONG Offset,
                    _In_    ULONG Length,
                    _Inout_ PUCHAR Buffer)
{
    ULONGLONG VolumeBytes;

    if (BytesPerSector == 0 ||
        SectorsInVolume > (ULONGLONG)-1 / BytesPerSector)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    VolumeBytes = SectorsInVolume * BytesPerSector;
    if (Offset > VolumeBytes || Length > VolumeBytes - Offset)
        return STATUS_DISK_FULL;

    /* The first write since the volume was last clean marks it dirty, durable
     * before this write proceeds (MarkDirtyBeforeWrite sets DirtyMarked first,
     * so its own $Volume write does not recurse). */
    if (DirtyTracking && !DirtyMarked)
    {
        NTSTATUS DirtyStatus = MarkDirtyBeforeWrite();
        if (!NT_SUCCESS(DirtyStatus))
            return DirtyStatus;
    }

    if (WriteThroughDepth != 0)
    {
        return NtfsWriteVolumeThroughContext(IoContext,
                                             BytesPerSector,
                                             Offset,
                                             Length,
                                             Buffer);
    }

    return NtfsWriteVolumeContext(IoContext,
                                  BytesPerSector,
                                  Offset,
                                  Length,
                                  Buffer);
}

NTSTATUS
Volume::FlushVolume()
{
    return NtfsFlushVolumeContext(IoContext);
}

NTSTATUS
Volume::CommitRange(_In_ ULONGLONG Offset,
                    _In_ ULONG Length)
{
    return NtfsCommitRangeContext(IoContext, Offset, Length);
}

NTSTATUS
Volume::BarrierAfter(_In_ ULONGLONG LaterOffset, _In_ ULONG LaterLength,
                     _In_ ULONGLONG EarlierOffset, _In_ ULONG EarlierLength)
{
    return NtfsBarrierAfterContext(IoContext, LaterOffset, LaterLength,
                                   EarlierOffset, EarlierLength);
}

void
Volume::BarrierCancel(_In_ ULONGLONG LaterOffset, _In_ ULONG LaterLength)
{
    NtfsBarrierCancelContext(IoContext, LaterOffset, LaterLength);
}

BOOLEAN
Volume::WriteBarrierEnabled()
{
    return NtfsWriteBarrierEnabledContext(IoContext);
}

NTSTATUS
Volume::OrderAfter(_In_ ULONGLONG PointerOffset, _In_ ULONG PointerLength,
                   _In_ ULONGLONG PointeeOffset, _In_ ULONG PointeeLength)
{
    NTSTATUS Status;

    if (PointeeLength == 0)
        return STATUS_SUCCESS;

    if (PointerLength != 0 && WriteBarrierEnabled())
    {
        Status = BarrierAfter(PointerOffset, PointerLength,
                              PointeeOffset, PointeeLength);
        if (NT_SUCCESS(Status))
            return Status;
        /*
         * No ordering was installed (BarrierAfter sets its constraint only
         * on success), so there is nothing to cancel: make the pointee
         * durable instead, as the barrier call sites do on refusal.
         */
    }

    Status = CommitRange(PointeeOffset, PointeeLength);
    if (Status == STATUS_NOT_IMPLEMENTED)
        Status = FlushVolume();
    return Status;
}

NTSTATUS
Volume::WriteVolumeThrough(_In_    ULONGLONG Offset,
                           _In_    ULONG Length,
                           _Inout_ PUCHAR Buffer)
{
    ULONGLONG VolumeBytes;

    if (BytesPerSector == 0 ||
        SectorsInVolume > (ULONGLONG)-1 / BytesPerSector)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    VolumeBytes = SectorsInVolume * BytesPerSector;
    if (Offset > VolumeBytes || Length > VolumeBytes - Offset)
        return STATUS_DISK_FULL;

    /* Same rule as WriteVolume: the first write marks the volume dirty first. */
    if (DirtyTracking && !DirtyMarked)
    {
        NTSTATUS DirtyStatus = MarkDirtyBeforeWrite();
        if (!NT_SUCCESS(DirtyStatus))
            return DirtyStatus;
    }

    return NtfsWriteVolumeThroughContext(IoContext,
                                         BytesPerSector,
                                         Offset,
                                         Length,
                                         Buffer);
}

NTSTATUS
Volume::FlushVolumeRange(_In_ ULONGLONG Offset,
                         _In_ ULONG Length)
{
    ULONGLONG VolumeBytes;

    if (BytesPerSector == 0 ||
        SectorsInVolume > (ULONGLONG)-1 / BytesPerSector)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    VolumeBytes = SectorsInVolume * BytesPerSector;
    if (Offset > VolumeBytes || Length > VolumeBytes - Offset)
        return STATUS_INVALID_PARAMETER;

    return NtfsFlushVolumeRangeContext(IoContext, Offset, Length);
}
