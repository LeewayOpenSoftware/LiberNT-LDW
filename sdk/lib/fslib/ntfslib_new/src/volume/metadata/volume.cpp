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
Volume::GetVolumeLabel(_Inout_ PWSTR   VolumeLabel,
                       _Inout_ PUSHORT Length)
{
    NTSTATUS Status;
    PFileRecord VolumeFile;
    PAttribute VolumeNameAttr;
    ULONG BytesRemaining, LabelLength;

    if (!VolumeLabel || !Length)
        return STATUS_INVALID_PARAMETER;

    if (!VolumeLabelCached)
    {
        Status = MFT->GetFileAttributeFromFileRecordNumber(TypeVolumeName,
                                                           NULL,
                                                           _Volume,
                                                           &VolumeFile,
                                                           &VolumeNameAttr);
        if (!NT_SUCCESS(Status))
            return Status;

        LabelLength = GetAttributeDataSize(VolumeNameAttr);
        if (LabelLength > sizeof(VolumeLabelCache))
        {
            delete VolumeFile;
            return STATUS_FILE_CORRUPT_ERROR;
        }

        BytesRemaining = LabelLength;
        Status = LabelLength == 0
                 ? STATUS_SUCCESS
                 : VolumeFile->CopyData(VolumeNameAttr,
                                        (PUCHAR)VolumeLabelCache,
                                        &BytesRemaining);
        delete VolumeFile;
        if (!NT_SUCCESS(Status) || BytesRemaining != 0)
            return NT_SUCCESS(Status) ? STATUS_END_OF_FILE : Status;

        VolumeLabelCacheLength = (USHORT)LabelLength;
        VolumeLabelCached = TRUE;
    }

    *Length = VolumeLabelCacheLength;
    RtlCopyMemory(VolumeLabel, VolumeLabelCache, VolumeLabelCacheLength);
    return STATUS_SUCCESS;
}

NTSTATUS
Volume::SetVolumeLabel(_In_ PWSTR VolumeLabel,
                       _In_ ULONG Length)
{
    NTSTATUS Status;
    FileRecord* VolumeFile;
    PAttribute VolumeNameAttr;

    if ((!VolumeLabel && Length != 0) ||
        Length > sizeof(VolumeLabelCache))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (IsReadOnly)
        return STATUS_ACCESS_DENIED;

    /* The dirty flag lives in this record: set it before reading the record,
     * or the label write below would put back the copy without it. */
    Status = MarkDirtyBeforeWrite();
    if (!NT_SUCCESS(Status))
        return Status;

    /* Allocate memory for $Volume file record, retrieve the file record, and
     * get a pointer to the $VOLUME_NAME attribute.
     */
    Status = MFT->GetFileAttributeFromFileRecordNumber(TypeVolumeName,
                                                       NULL,
                                                       _Volume,
                                                       &VolumeFile,
                                                       &VolumeNameAttr);

    if (!NT_SUCCESS(Status))
        return Status;

    // Replace and repack the complete resident $VOLUME_NAME value.
    Status = VolumeFile->ReplaceResidentData(
        VolumeNameAttr,
        reinterpret_cast<const UCHAR*>(VolumeLabel),
        Length);

    if (NT_SUCCESS(Status))
    {
        // Write the volume file to disk.
        Status = MFT->WriteFileRecordToMFT(VolumeFile);
        if (NT_SUCCESS(Status))
        {
            RtlCopyMemory(VolumeLabelCache, VolumeLabel, Length);
            VolumeLabelCacheLength = (USHORT)Length;
            VolumeLabelCached = TRUE;
        }
    }

    delete VolumeFile;
    return Status;
}

NTSTATUS
Volume::QueryVolumeFlags(_Out_ PUSHORT Flags)
{
    NTSTATUS Status;
    PFileRecord VolumeFile;
    PAttribute InfoAttr;
    VolumeInformationEx Info;
    ULONG BytesRemaining;

    if (!Flags)
        return STATUS_INVALID_PARAMETER;

    Status = MFT->GetFileAttributeFromFileRecordNumber(TypeVolumeInformation,
                                                       NULL,
                                                       _Volume,
                                                       &VolumeFile,
                                                       &InfoAttr);
    if (!NT_SUCCESS(Status))
        return Status;

    if (InfoAttr->IsNonResident ||
        GetAttributeDataSize(InfoAttr) < FIELD_OFFSET(VolumeInformationEx, Reserved2))
    {
        delete VolumeFile;
        return STATUS_FILE_CORRUPT_ERROR;
    }

    BytesRemaining = FIELD_OFFSET(VolumeInformationEx, Reserved2);
    Status = VolumeFile->CopyData(InfoAttr, reinterpret_cast<PUCHAR>(&Info), &BytesRemaining);
    delete VolumeFile;
    if (!NT_SUCCESS(Status))
        return Status;
    if (BytesRemaining != 0)
        return STATUS_END_OF_FILE;

    *Flags = Info.Flags;
    return STATUS_SUCCESS;
}

NTSTATUS
Volume::SetVolumeFlags(_In_ USHORT Set,
                       _In_ USHORT Clear)
{
    NTSTATUS Status;
    FileRecord* VolumeFile;
    PAttribute InfoAttr;
    UCHAR Data[sizeof(VolumeInformationEx) + 64];
    PVolumeInformationEx Info = reinterpret_cast<PVolumeInformationEx>(Data);
    ULONG Length, BytesRemaining;

    if (IsReadOnly)
        return STATUS_MEDIA_WRITE_PROTECTED;

    /* As in SetVolumeLabel: settle the tracked flag before taking a copy. */
    Status = MarkDirtyBeforeWrite();
    if (!NT_SUCCESS(Status))
        return Status;

    Status = MFT->GetFileAttributeFromFileRecordNumber(TypeVolumeInformation,
                                                       NULL,
                                                       _Volume,
                                                       &VolumeFile,
                                                       &InfoAttr);
    if (!NT_SUCCESS(Status))
        return Status;

    Length = (ULONG)GetAttributeDataSize(InfoAttr);
    if (InfoAttr->IsNonResident || Length < FIELD_OFFSET(VolumeInformationEx, Reserved2) ||
        Length > sizeof(Data))
    {
        delete VolumeFile;
        return STATUS_FILE_CORRUPT_ERROR;
    }

    BytesRemaining = Length;
    Status = VolumeFile->CopyData(InfoAttr, Data, &BytesRemaining);
    if (NT_SUCCESS(Status) && BytesRemaining != 0)
        Status = STATUS_END_OF_FILE;
    if (NT_SUCCESS(Status) && (Info->Flags & (Set | Clear)) != Set)
    {
        Info->Flags = (USHORT)((Info->Flags & ~Clear) | Set);
        Status = VolumeFile->ReplaceResidentData(InfoAttr, Data, Length);
        if (NT_SUCCESS(Status))
            Status = MFT->WriteFileRecordToMFT(VolumeFile);
        if (NT_SUCCESS(Status))
            Status = FlushVolume();
    }

    delete VolumeFile;
    return Status;
}

/*
 * Without a journal, a crash can leave any subset of the cached metadata
 * writes on the medium, so the volume must say "check me" before the first of
 * them can land. The flag is made durable (FlushVolume reaches the medium)
 * before the write that triggered it proceeds. Writes are serialized by the
 * caller's metadata lock, so the flag cannot be overtaken by a second writer.
 */
NTSTATUS
Volume::EnableDirtyTracking()
{
    NTSTATUS Status;
    USHORT Flags;

    if (IsReadOnly)
        return STATUS_SUCCESS;

    Status = QueryVolumeFlags(&Flags);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Dirty already: an earlier crash or a chkdsk request. Keep it for chkdsk. */
    DirtyRetained = (Flags & VOL_DIRTY) ? TRUE : FALSE;
    DirtyMarked = DirtyRetained;
    DirtyTracking = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS
Volume::MarkDirtyBeforeWrite()
{
    NTSTATUS Status;

    if (!DirtyTracking || DirtyMarked || IsReadOnly)
        return STATUS_SUCCESS;

    /* Set first: the flag's own record write comes back through WriteVolume. */
    DirtyMarked = TRUE;
    Status = SetVolumeFlags(VOL_DIRTY, 0);
    if (NT_SUCCESS(Status))
        Status = FlushVolume();
    if (!NT_SUCCESS(Status))
        DirtyMarked = FALSE;
    return Status;
}

NTSTATUS
Volume::MarkClean()
{
    NTSTATUS Status;

    if (!DirtyTracking || !DirtyMarked || DirtyRetained || IsReadOnly)
        return STATUS_SUCCESS;

    /* Everything else reaches the medium before the flag says it has. */
    Status = FlushVolume();
    if (NT_SUCCESS(Status))
        Status = SetVolumeFlags(0, VOL_DIRTY);
    if (NT_SUCCESS(Status))
        Status = FlushVolume();
    if (NT_SUCCESS(Status))
        DirtyMarked = FALSE;
    return Status;
}

BOOLEAN
Volume::IsDirtyTransient()
{
    return (DirtyTracking && DirtyMarked && !DirtyRetained) ? TRUE : FALSE;
}

void
Volume::RetainDirty(_In_ BOOLEAN Retain)
{
    if (!DirtyTracking)
        return;
    DirtyRetained = Retain;
    DirtyMarked = Retain;
}
