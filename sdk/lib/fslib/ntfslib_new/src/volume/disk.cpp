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
    NTSTATUS Status;

    if (BytesPerSector == 0 ||
        SectorsInVolume > (ULONGLONG)-1 / BytesPerSector)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    VolumeBytes = SectorsInVolume * BytesPerSector;
    if (Offset > VolumeBytes || Length > VolumeBytes - Offset)
        return STATUS_DISK_FULL;

    /* The first write since the volume was last clean marks it dirty. */
    if (DirtyTracking && !DirtyMarked && !ShutdownClean)
    {
        Status = MarkDirtyBeforeWrite();
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Status = NtfsWriteVolumeContext(IoContext,
                                    BytesPerSector,
                                    Offset,
                                    Length,
                                    Buffer);

    /* After the shutdown clean point nothing flushes again: write through. */
    if (ShutdownClean && NT_SUCCESS(Status))
    {
#if DBG
        /* Debug builds name every late write. */
        if (TRUE)
#else
        if (LateWrites < 8)
#endif
        {
            DPRINT1("Write after the shutdown clean point at offset 0x%I64x (%lu bytes), written through\n",
                    Offset, Length);
        }
        LateWrites++;
        Status = FlushVolume();
    }
    return Status;
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
