/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Source file for the ntfs_new write APIs
 * COPYRIGHT:   Copyright 2024 Carl Bialorucki <carl.bialorucki@reactos.org>
 *              Copyright 2024 Justin Miller <justin.miller@reactos.org>
 */

#include "ntfspch.h"

/* GLOBALS *****************************************************************/

#define NTFS_CACHED_GROWTH_LIMIT     (64 * 1024 * 1024)

/* FUNCTIONS ****************************************************************/

static
BOOLEAN
NtfsGrowForCachedWrite(_In_ PVolumeContextBlock VolCB,
                       _In_ PFileContextBlock FileCB,
                       _In_ PFILE_OBJECT FileObj,
                       _In_ LONGLONG EndOffset)
{
    PFSRTL_ADVANCED_FCB_HEADER Header = NtfsGetCommonFcbHeader(FileCB);
    LONGLONG Allocation = Header->AllocationSize.QuadPart;
    PAttribute DataAttribute;
    LONGLONG Target;
    NTSTATUS Status;
    BOOLEAN SizePersisted = FALSE;

    if (EndOffset > Allocation)
    {
        Target = Allocation + ((Allocation < NTFS_CACHED_GROWTH_LIMIT) ? Allocation : NTFS_CACHED_GROWTH_LIMIT);
        if (Target < EndOffset)
            Target = EndOffset;

        NtfsAcquireMetadata(VolCB);
        Status = NtfsRefreshDirectoryRecord(VolCB, FileCB);
        if (!NT_SUCCESS(Status))
        {
            NtfsReleaseMetadata(VolCB);
            return FALSE;
        }
        DataAttribute = NtfsFileRecordGetAttribute(FileCB->FileRec, FileCB->RequestedType,
                                                   FileCB->RequestedStream);
        if (EndOffset <= PAGE_SIZE &&
            (Allocation == 0 || (DataAttribute && !DataAttribute->IsNonResident)))
        {
            Status = NtfsFileRecordSetFileDataSize(FileCB->FileRec, FileCB->RequestedType,
                                                   FileCB->RequestedStream, EndOffset);
            SizePersisted = NT_SUCCESS(Status);
        }
        else
        {
            Status = NtfsFileRecordSetFileAllocationSize(FileCB->FileRec, FileCB->RequestedType,
                                                         FileCB->RequestedStream, Target);
        }
        if (NT_SUCCESS(Status))
        {
            DataAttribute = NtfsFileRecordGetAttribute(FileCB->FileRec, FileCB->RequestedType, FileCB->RequestedStream);
            if (DataAttribute)
            {
                Allocation = DataAttribute->IsNonResident
                    ? (LONGLONG)NtfsAttributeGetPhysicalAllocationSize(DataAttribute)
                    : DataAttribute->Resident.DataLength;
            }
            InterlockedIncrement(&VolCB->DirGeneration);
        }
        NtfsReleaseMetadata(VolCB);
        if (Allocation < EndOffset)
            return FALSE;
        if (SizePersisted)
            FileCB->WriteTimesStamped = TRUE;
    }

    ExAcquireResourceExclusiveLite(NtfsGetPagingIoResource(FileCB), TRUE);
    Header->AllocationSize.QuadPart = Allocation;
    if (Header->FileSize.QuadPart < EndOffset)
        Header->FileSize.QuadPart = EndOffset;
    FileCB->StreamCB->SizePending = !SizePersisted;
    ExReleaseResourceLite(NtfsGetPagingIoResource(FileCB));

    if (FileObj->PrivateCacheMap != NULL)
        CcSetFileSizes(FileObj, (PCC_FILE_SIZES)&Header->AllocationSize);
    FileObj->Flags |= FO_FILE_SIZE_CHANGED;
    FileCB->NotifyFilter |= FILE_NOTIFY_CHANGE_SIZE;
    return TRUE;
}

NTSTATUS
NtfsPersistPendingSize(_In_ PVolumeContextBlock VolCB,
                       _In_ PFileContextBlock FileCB)
{
    PFSRTL_ADVANCED_FCB_HEADER Header;
    PAttribute DataAttribute;
    NTSTATUS Status = STATUS_SUCCESS;
    LONGLONG Allocation = -1;
    LONGLONG FileSize;

    if (!FileCB->StreamCB || !FileCB->StreamCB->SizePending || !FileCB->FileRec)
        return STATUS_SUCCESS;

    Header = NtfsGetCommonFcbHeader(FileCB);
    FileSize = Header->FileSize.QuadPart;

    ExAcquireResourceExclusiveLite(NtfsGetPagingIoResource(FileCB), TRUE);
    NtfsAcquireMetadata(VolCB);
    Status = NtfsRefreshDirectoryRecord(VolCB, FileCB);
    if (!NT_SUCCESS(Status))
    {
        NtfsReleaseMetadata(VolCB);
        ExReleaseResourceLite(NtfsGetPagingIoResource(FileCB));
        return Status;
    }
    DataAttribute = NtfsFileRecordGetAttribute(FileCB->FileRec, FileCB->RequestedType, FileCB->RequestedStream);
    if (DataAttribute && DataAttribute->IsNonResident)
    {
        if (DataAttribute->NonResident.DataSize != (ULONGLONG)FileSize)
        {
            Status = NtfsFileRecordSetFileDataSize(FileCB->FileRec, FileCB->RequestedType, FileCB->RequestedStream,
                                                   (ULONGLONG)FileSize);
        }
        else if (NtfsAttributeGetPhysicalAllocationSize(DataAttribute) > (ULONGLONG)FileSize)
        {
            Status = NtfsFileRecordSetFileAllocationSize(FileCB->FileRec, FileCB->RequestedType,
                                                         FileCB->RequestedStream, (ULONGLONG)FileSize);
        }
        DataAttribute = NtfsFileRecordGetAttribute(FileCB->FileRec, FileCB->RequestedType, FileCB->RequestedStream);
        if (NT_SUCCESS(Status) && DataAttribute && DataAttribute->IsNonResident)
            Allocation = (LONGLONG)NtfsAttributeGetPhysicalAllocationSize(DataAttribute);
        if (NT_SUCCESS(Status))
            InterlockedIncrement(&VolCB->DirGeneration);
    }
    else
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
    }
    NtfsReleaseMetadata(VolCB);

    if (NT_SUCCESS(Status))
    {
        if (Allocation >= FileSize)
            Header->AllocationSize.QuadPart = Allocation;
        FileCB->StreamCB->SizePending = FALSE;
    }
    ExReleaseResourceLite(NtfsGetPagingIoResource(FileCB));
    return Status;
}

NTSTATUS
NtfsCommitFileRange(_In_ PVolumeContextBlock VolCB,
                    _In_ PFileContextBlock FileCB,
                    _In_ ULONGLONG FileOffset,
                    _In_ ULONGLONG Length)
{
    NtfsRetrievalExtent Extent;
    ULONGLONG ReturnedStartingVcn;
    ULONGLONG ClusterSize;
    ULONGLONG Vcn;
    ULONGLONG EndVcn;
    ULONGLONG RunEndVcn;
    ULONGLONG DiskOffset;
    ULONGLONG RunLength;
    ULONG ExtentCount;
    ULONG Chunk;
    NTSTATUS Status;

    ClusterSize = (ULONGLONG)VolCB->BytesPerSector *
                  NtfsVolumeGetSectorsPerCluster(VolCB->DiskVolume);
    if (!VolCB->StorageDevice || !ClusterSize || !FileCB->FileRec ||
        !Length || FileOffset > ~(ULONGLONG)0 - Length)
    {
        return STATUS_SUCCESS;
    }

    Vcn = FileOffset / ClusterSize;
    EndVcn = (FileOffset + Length - 1) / ClusterSize + 1;
    while (Vcn < EndVcn)
    {
        ExtentCount = 1;
        NtfsAcquireMetadata(VolCB);
        Status = NtfsFileRecordQueryRetrievalPointers(FileCB->FileRec,
                                                      FileCB->RequestedType,
                                                      FileCB->RequestedStream,
                                                      Vcn,
                                                      &ReturnedStartingVcn,
                                                      &Extent,
                                                      &ExtentCount);
        NtfsReleaseMetadata(VolCB);
        if ((!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW) ||
            ExtentCount != 1 ||
            ReturnedStartingVcn > Vcn ||
            Extent.NextVcn <= Vcn)
        {
            return STATUS_SUCCESS;
        }

        RunEndVcn = min(Extent.NextVcn, EndVcn);
        if (Extent.Lcn >= 0)
        {
            DiskOffset = ((ULONGLONG)Extent.Lcn + Vcn - ReturnedStartingVcn) * ClusterSize;
            RunLength = (RunEndVcn - Vcn) * ClusterSize;
            while (RunLength != 0)
            {
                Chunk = (ULONG)min(RunLength, 0x40000000ULL);
                Status = NtfsDiskFlushRangeKm(VolCB->StorageDevice, DiskOffset, Chunk);
                if (!NT_SUCCESS(Status))
                    return Status;
                DiskOffset += Chunk;
                RunLength -= Chunk;
            }
        }
        Vcn = RunEndVcn;
    }

    return STATUS_SUCCESS;
}

static
BOOLEAN
NtfsCachedWrite(_In_ PVolumeContextBlock VolCB,
                _In_ PFileContextBlock FileCB,
                _In_ PFILE_OBJECT FileObj,
                _In_ PIRP Irp,
                _In_ PVOID Buffer,
                _In_ ULONG Length,
                _In_ PLARGE_INTEGER ByteOffset,
                _Out_ PNTSTATUS Status)
{
    PFSRTL_ADVANCED_FCB_HEADER Header = NtfsGetCommonFcbHeader(FileCB);
    LONGLONG EndOffset = ByteOffset->QuadPart + Length;
    BOOLEAN Handled = FALSE;
    BOOLEAN Extending;
    NTSTATUS TimestampStatus;

    if (ByteOffset->QuadPart < 0 || !CcCanIWrite(FileObj, Length, TRUE, FALSE))
        return FALSE;

    if (Length >= 1024 * 1024 && EndOffset > Header->ValidDataLength.QuadPart &&
        FileObj->SectionObjectPointer->SharedCacheMap == NULL)
    {
        return FALSE;
    }

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(NtfsGetMainResource(FileCB), TRUE);
    Extending = EndOffset > Header->ValidDataLength.QuadPart;
    if (Extending)
    {
        ExReleaseResourceLite(NtfsGetMainResource(FileCB));
        ExAcquireResourceExclusiveLite(NtfsGetMainResource(FileCB), TRUE);
        if (!FileCB->StreamCB ||
            FileCB->StreamCB->Deleted ||
            !NtfsGrowForCachedWrite(VolCB, FileCB, FileObj, EndOffset))
        {
            ExReleaseResourceLite(NtfsGetMainResource(FileCB));
            KeLeaveCriticalRegion();
            return FALSE;
        }
    }

    if (EndOffset <= Header->ValidDataLength.QuadPart || Extending)
    {
        Handled = TRUE;
        _SEH2_TRY
        {
            if (FileObj->PrivateCacheMap == NULL)
                NtfsInitializeStreamCache(FileCB, FileObj);
            *Status = CcCopyWrite(FileObj, ByteOffset, Length, TRUE, Buffer) ? STATUS_SUCCESS
                                                                              : STATUS_CANT_WAIT;
        }
        _SEH2_EXCEPT(FsRtlIsNtstatusExpected(_SEH2_GetExceptionCode()) ?
                     EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            *Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;

        if (NT_SUCCESS(*Status))
        {
            if (EndOffset > Header->ValidDataLength.QuadPart)
                Header->ValidDataLength.QuadPart = EndOffset;
            FileObj->Flags |= FO_FILE_MODIFIED;
            FileCB->NotifyFilter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
            if (FileObj->Flags & FO_SYNCHRONOUS_IO)
                FileObj->CurrentByteOffset.QuadPart = ByteOffset->QuadPart + Length;
            Irp->IoStatus.Information = Length;
        }
    }

    ExReleaseResourceLite(NtfsGetMainResource(FileCB));

    if (Handled && NT_SUCCESS(*Status) && !FileCB->WriteTimesStamped)
    {
        FileCB->WriteTimesStamped = TRUE;
        ExAcquireResourceExclusiveLite(NtfsGetMainResource(FileCB), TRUE);
        NtfsAcquireMetadata(VolCB);
        TimestampStatus = NtfsRefreshDirectoryRecord(VolCB, FileCB);
        if (NT_SUCCESS(TimestampStatus))
            TimestampStatus = NtfsFileRecordUpdateAutomaticTimestamps(
                FileCB->FileRec,
                NTFS_BASIC_INFO_LAST_WRITE_TIME | NTFS_BASIC_INFO_CHANGE_TIME);
        if (NT_SUCCESS(TimestampStatus))
            InterlockedIncrement(&VolCB->DirGeneration);
        NtfsReleaseMetadata(VolCB);
        ExReleaseResourceLite(NtfsGetMainResource(FileCB));
    }

    KeLeaveCriticalRegion();
    return Handled;
}

/*
 * Paging writes sent outside the volume metadata lock (experimental, off
 * unless NtfsPagingWriteOutsideLock is set).
 *
 * A paging write that only overwrites clusters the stream already owns and
 * has initialized changes no metadata: no allocation, no size, no mapping.
 * Under the lock only its extent is resolved; the transfer goes out after
 * the lock is released, as NtfsTryDirectRead does for paging reads. The
 * caller holds the stream's PagingIoResource shared, and every path that
 * frees or moves the stream's clusters (truncation, overwrite, delete,
 * rename or link over it, MOVE_FILE) takes it exclusive, so the extent
 * stays this stream's until the write completes.
 *
 * The same switch makes every paging write, locked or not, leave the write
 * times to the application, as on Windows: a paging write is the cache or
 * the mapped page writer moving data that a user write (stamped by that
 * write and at its handle's cleanup) or a mapped view already changed. A
 * stream with a user-writable mapped view is the exception: Mm cannot say
 * which pages a view dirtied, so its paging writes stamp the times under
 * the lock, as the change happened through the view.
 */
volatile LONG NtfsPagingWriteCounts[NtfsPagingWriteCountMax];

static
VOID
NtfsCountPagingWrite(_In_ NTFS_PAGING_WRITE_COUNT Which)
{
    InterlockedIncrement(&NtfsPagingWriteCounts[Which]);
}

/* Whether a paging write stamps the last-write and change times. */
static
BOOLEAN
NtfsPagingWriteStampsTimes(_In_ PFileContextBlock FileCB)
{
    if (!gPagingWriteOutsideLock)
        return TRUE;
    return FileCB->StreamCB &&
           MmDoesFileHaveUserWritableReferences(&FileCB->StreamCB->SectionObjectPointers) != 0;
}

static
BOOLEAN
NtfsTryDirectPagingWrite(
    _In_ PVolumeContextBlock VolCB,
    _In_ PFileContextBlock FileCB,
    _In_ ULONGLONG FileOffset,
    _In_ ULONG Length,
    _Out_ PULONGLONG DiskOffset)
{
    PAttribute Attribute;
    NtfsRetrievalExtent Extent;
    ULONGLONG ReturnedStartingVcn;
    ULONGLONG StartingVcn;
    ULONGLONG ClusterSize;
    ULONGLONG InClusterOffset;
    ULONGLONG Available;
    ULONGLONG PhysicalCluster;
    ULONGLONG Offset;
    ULONG ExtentCount;
    NTSTATUS Status;

    *DiskOffset = MAXULONGLONG;

    /* NtfsDiskWriteKm accepts only these sector sizes. */
    if (!VolCB->StorageDevice ||
        (VolCB->BytesPerSector != 512 && VolCB->BytesPerSector != 4096) ||
        FileOffset % VolCB->BytesPerSector != 0 ||
        Length % VolCB->BytesPerSector != 0 ||
        FileOffset > ~(ULONGLONG)0 - Length)
    {
        NtfsCountPagingWrite(NtfsPagingWriteUnaligned);
        return FALSE;
    }

    /* System files, and a stream already being deleted, keep the locked
     * path. So does a reparse point: a WOF-compressed file is materialized
     * by the library's write. */
    if (!FileCB->StreamCB ||
        FileCB->StreamCB->Deleted ||
        (FileCB->StreamCB->FileReference & 0x0000FFFFFFFFFFFFULL) < 24 ||
        FileCB->RequestedType != TypeData ||
        NtfsFileRecordGetAttribute(FileCB->FileRec, TypeReparsePoint, NULL))
    {
        NtfsCountPagingWrite(NtfsPagingWriteNotEligible);
        return FALSE;
    }

    Attribute = NtfsFileRecordGetAttribute(FileCB->FileRec,
                                           FileCB->RequestedType,
                                           FileCB->RequestedStream);
    if (!Attribute ||
        !Attribute->IsNonResident ||
        (Attribute->Flags & (ATTR_COMPRESSION_MASK | ATTR_ENCRYPTED | ATTR_SPARSE)))
    {
        NtfsCountPagingWrite(NtfsPagingWriteNotEligible);
        return FALSE;
    }

    /* Writing past the initialized size changes the record. */
    if (FileOffset + Length > Attribute->NonResident.InitalizedDataSize)
    {
        NtfsCountPagingWrite(NtfsPagingWritePastInitialized);
        return FALSE;
    }

    ClusterSize = (ULONGLONG)VolCB->BytesPerSector *
                  NtfsVolumeGetSectorsPerCluster(VolCB->DiskVolume);
    if (!ClusterSize)
    {
        NtfsCountPagingWrite(NtfsPagingWriteNotEligible);
        return FALSE;
    }

    StartingVcn = FileOffset / ClusterSize;
    InClusterOffset = FileOffset % ClusterSize;
    ExtentCount = 1;
    Status = NtfsFileRecordQueryRetrievalPointers(FileCB->FileRec,
                                                  FileCB->RequestedType,
                                                  FileCB->RequestedStream,
                                                  StartingVcn,
                                                  &ReturnedStartingVcn,
                                                  &Extent,
                                                  &ExtentCount);
    if ((!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW) ||
        ExtentCount != 1 ||
        ReturnedStartingVcn > StartingVcn ||
        Extent.NextVcn <= StartingVcn ||
        Extent.Lcn < 0)
    {
        NtfsCountPagingWrite(NtfsPagingWriteNoExtent);
        return FALSE;
    }

    /* One extent must cover the whole range. */
    Available = Extent.NextVcn - StartingVcn;
    if (Available > ~(ULONGLONG)0 / ClusterSize)
    {
        NtfsCountPagingWrite(NtfsPagingWriteNoExtent);
        return FALSE;
    }
    Available *= ClusterSize;
    if (Available < InClusterOffset ||
        Available - InClusterOffset < Length)
    {
        NtfsCountPagingWrite(NtfsPagingWriteSplit);
        return FALSE;
    }

    PhysicalCluster = StartingVcn - ReturnedStartingVcn;
    if ((ULONGLONG)Extent.Lcn > ~(ULONGLONG)0 - PhysicalCluster)
    {
        NtfsCountPagingWrite(NtfsPagingWriteNoExtent);
        return FALSE;
    }
    PhysicalCluster += (ULONGLONG)Extent.Lcn;
    if (PhysicalCluster > ~(ULONGLONG)0 / ClusterSize)
    {
        NtfsCountPagingWrite(NtfsPagingWriteNoExtent);
        return FALSE;
    }
    Offset = PhysicalCluster * ClusterSize;
    if (Offset > ~(ULONGLONG)0 - InClusterOffset)
    {
        NtfsCountPagingWrite(NtfsPagingWriteNoExtent);
        return FALSE;
    }

    *DiskOffset = Offset + InClusterOffset;
    return TRUE;
}

static
NTSTATUS
NtfsFinishDirectPagingWrite(
    _In_ PVolumeContextBlock VolCB,
    _In_ PFileContextBlock FileCB,
    _In_ PUCHAR Buffer,
    _In_ ULONG Length,
    _In_ ULONGLONG DiskOffset)
{
    PVOID BounceBuffer;
    NTSTATUS Status;

    /* As on the locked path: the storage stack must not probe and lock the
     * section pages Mm already locked for this transfer. */
    BounceBuffer = ExAllocatePoolUninitialized(NonPagedPoolNx, Length, TAG_NTFS);
    if (BounceBuffer)
        RtlCopyMemory(BounceBuffer, Buffer, Length);

    /* Coherent with the library's block cache: held blocks in the range are
     * committed or updated, so metadata reads never see stale data. */
    Status = NtfsDiskWriteKm(VolCB->StorageDevice,
                             VolCB->BytesPerSector,
                             DiskOffset,
                             Length,
                             BounceBuffer ? BounceBuffer : Buffer);
    if (BounceBuffer)
        ExFreePoolWithTag(BounceBuffer, TAG_NTFS);

    /* A write the block cache absorbed after the shutdown clean pass began
     * has no later flush to carry it. */
    if (NT_SUCCESS(Status) &&
        InterlockedCompareExchange(&VolCB->ShutdownStarted, 0, 0) != 0)
    {
        Status = NtfsDiskFlushVolumeKm(VolCB->StorageDevice);
    }

    if (NT_SUCCESS(Status))
        NtfsCountPagingWrite(NtfsPagingWriteDirect);
    else
    {
        NtfsCountPagingWrite(NtfsPagingWriteDirectFailed);
    }
    return Status;
}

_Function_class_(IRP_MJ_WRITE)
_Function_class_(DRIVER_DISPATCH)
NTSTATUS
NTAPI
NtfsFsdWrite(_In_ PDEVICE_OBJECT VolumeDeviceObject,
             _Inout_ PIRP Irp)
{
    if (VolumeDeviceObject != NtfsDiskFileSystemDeviceObject)
        NtfsBindVolumeDisk((PVolumeContextBlock)VolumeDeviceObject->DeviceExtension);
    /* Overview:
     * Handles write requests.
     * See: https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/irp-mj-write
     */
    NTSTATUS Status;
    PIO_STACK_LOCATION IrpSp;
    PUCHAR Buffer;
    LARGE_INTEGER ByteOffset;
    ULONG Length;
    PFileContextBlock FileCB;
    PFILE_OBJECT FileObj;
    PVolumeContextBlock VolCB;
    PNtfsVolume DiskVolume;
    PNtfsFileRecord FileRec;
    AttributeType RequestedType;
    PWSTR RequestedStream;
    BOOLEAN ResourceAcquired = FALSE;
    BOOLEAN PagingIo = FALSE;
    PVOID BounceBuffer = NULL;
    PMDL LockMdl = NULL;
    BOOLEAN LockFailed = FALSE;
    ULONGLONG DirectDiskOffset;

    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    FileObj = IrpSp->FileObject;
    if (!FileObj ||
        !VolumeDeviceObject->DeviceExtension ||
        !FileObj->FsContext)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Complete;
    }

    FileCB = NtfsGetFileContext(FileObj);
    VolCB = (PVolumeContextBlock)VolumeDeviceObject->DeviceExtension;
    NTFS_CHECK_FILE_VOLUME(VolCB, FileCB, "write");
    if (FileCB->IsVolumeOpen)
        return NtfsForwardVolumeIo(VolCB, FileCB, Irp, TRUE);

    FileRec = FileCB->FileRec;
    if (!FileRec)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Complete;
    }
    if ((NtfsFileRecordGetHeader(FileRec)->Flags & FR_IS_DIRECTORY) &&
        !FileCB->RequestedStream &&
        !(Irp->Flags & IRP_PAGING_IO))
    {
        Status = STATUS_INVALID_DEVICE_REQUEST;
        goto Complete;
    }

    DiskVolume = VolCB->DiskVolume;
    if (!DiskVolume)
    {
        Status = STATUS_INVALID_DEVICE_STATE;
        goto Complete;
    }

    if (NtfsVolumeIsReadOnly(DiskVolume))
    {
        Status = STATUS_MEDIA_WRITE_PROTECTED;
        goto Complete;
    }

    if (!BooleanFlagOn(Irp->Flags, IRP_PAGING_IO) &&
        !(FileCB->DesiredAccess &
          (FILE_WRITE_DATA | FILE_APPEND_DATA)))
    {
        Status = STATUS_ACCESS_DENIED;
        goto Complete;
    }

    Buffer = (PUCHAR)GetBuffer(Irp);
    Length = IrpSp->Parameters.Write.Length;
    if (Length != 0 && !Buffer)
    {
        Status = STATUS_INVALID_USER_BUFFER;
        goto Complete;
    }

    ByteOffset = IrpSp->Parameters.Write.ByteOffset;
    RequestedType = FileCB->RequestedType;
    RequestedStream = FileCB->RequestedStream;

    /* Only a handle granted append access *without* plain write access is
     * append-only; FILE_GENERIC_WRITE includes FILE_APPEND_DATA, so testing
     * that bit alone would redirect every ordinary write to end of file. */
    if ((FileCB->DesiredAccess & FILE_APPEND_DATA) &&
        !(FileCB->DesiredAccess & FILE_WRITE_DATA))
    {
        ByteOffset.HighPart = -1;
        ByteOffset.LowPart = FILE_WRITE_TO_END_OF_FILE;
    }
    if (!BooleanFlagOn(Irp->Flags, IRP_PAGING_IO) &&
        FileCB->StreamCB &&
        !FsRtlCheckLockForWriteAccess(&FileCB->StreamCB->FileLock, Irp))
    {
        Status = STATUS_FILE_LOCK_CONFLICT;
        goto Complete;
    }

    if (!BooleanFlagOn(Irp->Flags, IRP_PAGING_IO | IRP_NOCACHE) &&
        !BooleanFlagOn(FileObj->Flags, FO_NO_INTERMEDIATE_BUFFERING | FO_WRITE_THROUGH) &&
        Length != 0 &&
        RequestedType == TypeData &&
        FileObj->SectionObjectPointer != NULL &&
        NtfsCachedWrite(VolCB, FileCB, FileObj, Irp, Buffer, Length, &ByteOffset, &Status))
    {
        goto Complete;
    }

    /* Paging I/O is serviced while a caller may already hold MainResource,
     * so it synchronizes on PagingIoResource instead. */
    PagingIo = BooleanFlagOn(Irp->Flags, IRP_PAGING_IO);
    KeEnterCriticalRegion();
    if (PagingIo)
        ExAcquireResourceSharedLite(NtfsGetPagingIoResource(FileCB), TRUE);
    else
        ExAcquireResourceExclusiveLite(NtfsGetMainResource(FileCB), TRUE);
    ResourceAcquired = TRUE;

    if (PagingIo)
    {
        LONGLONG FileSize = NtfsGetCommonFcbHeader(FileCB)->FileSize.QuadPart;

        if (FileCB->StreamCB && FileCB->StreamCB->Deleted)
        {
            Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = Length;
            goto Complete;
        }
        if (ByteOffset.QuadPart >= FileSize)
            Length = 0;
        else if (ByteOffset.QuadPart + Length > FileSize)
            Length = (ULONG)(FileSize - ByteOffset.QuadPart);
    }
    else if (Length != 0 &&
             FileObj->SectionObjectPointer != NULL &&
             (FileObj->SectionObjectPointer->SharedCacheMap != NULL ||
              FileObj->SectionObjectPointer->DataSectionObject != NULL))
    {
        IO_STATUS_BLOCK FlushStatus;

        CcFlushCache(FileObj->SectionObjectPointer, NULL, 0, &FlushStatus);
        if (!NT_SUCCESS(FlushStatus.Status))
        {
            Status = FlushStatus.Status;
            goto Complete;
        }
    }

    NtfsAcquireMetadata(VolCB);
    if (PagingIo && FileCB->StreamCB && FileCB->StreamCB->Deleted)
    {
        NtfsReleaseMetadata(VolCB);
        Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = Length;
        goto Complete;
    }
    if (PagingIo && Length != 0 && gPagingWriteOutsideLock &&
        RequestedType == TypeData)
    {
        Status = NtfsRefreshDirectoryRecord(VolCB, FileCB);
        if (NT_SUCCESS(Status) &&
            NtfsTryDirectPagingWrite(VolCB,
                                     FileCB,
                                     (ULONGLONG)ByteOffset.QuadPart,
                                     Length,
                                     &DirectDiskOffset))
        {
            /* A record update in the block cache, not a disk transfer. */
            if (NtfsPagingWriteStampsTimes(FileCB) &&
                NT_SUCCESS(NtfsFileRecordUpdateAutomaticTimestamps(
                    FileRec,
                    NTFS_BASIC_INFO_LAST_WRITE_TIME | NTFS_BASIC_INFO_CHANGE_TIME)))
            {
                InterlockedIncrement(&VolCB->DirGeneration);
                NtfsCountPagingWrite(NtfsPagingWriteMappedStamp);
            }
            NtfsReleaseMetadata(VolCB);
            Status = NtfsFinishDirectPagingWrite(VolCB,
                                                 FileCB,
                                                 Buffer,
                                                 Length,
                                                 DirectDiskOffset);
            goto Written;
        }
        if (!NT_SUCCESS(Status))
        {
            NtfsReleaseMetadata(VolCB);
            goto Written;
        }
    }
    /*
     * A paging write hands us the section's own pages. Passing them straight
     * through means the storage stack probes and locks pages that Mm has
     * already locked for this transfer, so copy through pool memory instead.
     */
    if (PagingIo && Length != 0)
    {
        BounceBuffer = ExAllocatePoolUninitialized(NonPagedPoolNx, Length, TAG_NTFS);
        if (BounceBuffer)
        {
            RtlCopyMemory(BounceBuffer, Buffer, Length);
            Buffer = BounceBuffer;
        }
    }

    if (!PagingIo && Length != 0 && Buffer != NULL &&
        Irp->RequestorMode == UserMode &&
        !Irp->MdlAddress && !Irp->AssociatedIrp.SystemBuffer)
    {
        LockMdl = IoAllocateMdl(Buffer, Length, FALSE, FALSE, NULL);
        if (LockMdl)
        {
            _SEH2_TRY
            {
                MmProbeAndLockPages(LockMdl, UserMode, IoReadAccess);
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                IoFreeMdl(LockMdl);
                LockMdl = NULL;
                LockFailed = TRUE;
            }
            _SEH2_END;
            if (LockMdl)
            {
                PVOID SystemBuffer = MmGetSystemAddressForMdlSafe(LockMdl, NormalPagePriority);
                if (SystemBuffer)
                {
                    Buffer = SystemBuffer;
                }
                else
                {
                    MmUnlockPages(LockMdl);
                    IoFreeMdl(LockMdl);
                    LockMdl = NULL;
                    LockFailed = TRUE;
                }
            }
        }
        else
        {
            LockFailed = TRUE;
        }
    }

    if (LockFailed)
    {
        Status = STATUS_INVALID_USER_BUFFER;
    }
    else
    {
        BOOLEAN TimesSuppressed = FALSE;

        Status = NtfsRefreshDirectoryRecord(VolCB, FileCB);
        if (NT_SUCCESS(Status) && PagingIo && gPagingWriteOutsideLock)
        {
            /* Only for this call: the refresh above sets the handle's own
             * mask again before any other use of the record. */
            if (!NtfsPagingWriteStampsTimes(FileCB))
            {
                Status = NtfsFileRecordSetAutomaticTimestampMask(
                    FileRec,
                    FileCB->AutomaticTimestampMask &
                        ~(NTFS_BASIC_INFO_LAST_WRITE_TIME | NTFS_BASIC_INFO_CHANGE_TIME));
                TimesSuppressed = NT_SUCCESS(Status);
            }
            else
            {
                NtfsCountPagingWrite(NtfsPagingWriteMappedStamp);
            }
        }
        if (NT_SUCCESS(Status))
            Status = NtfsFileRecordWriteFileData(FileRec,
                                                 RequestedType,
                                                 RequestedStream,
                                                 Buffer,
                                                 &Length,
                                                 &ByteOffset);
        if (TimesSuppressed)
            (void)NtfsFileRecordSetAutomaticTimestampMask(FileRec, FileCB->AutomaticTimestampMask);

        /* The library updates duplicated $FILE_NAME information through a
         * separate parent record. Invalidate parsed directory snapshots
         * before releasing MetadataResource or a later open can reuse the
         * pre-write sizes from CachedLookupParent. */
        if (NT_SUCCESS(Status) && Length != 0 &&
            ((RequestedType == TypeData && !RequestedStream) ||
             (NtfsFileRecordGetHeader(FileRec)->Flags & FR_IS_DIRECTORY)))
            InterlockedIncrement(&VolCB->DirGeneration);
    }

    if (LockMdl)
    {
        MmUnlockPages(LockMdl);
        IoFreeMdl(LockMdl);
        LockMdl = NULL;
    }
    if (BounceBuffer)
    {
        ExFreePoolWithTag(BounceBuffer, TAG_NTFS);
        BounceBuffer = NULL;
    }
    NtfsReleaseMetadata(VolCB);

Written:
    if (NT_SUCCESS(Status))
    {
        if (!PagingIo)
            NtfsRefreshFileSizes(VolCB, FileCB, FileObj);
        if (!PagingIo && Length != 0 && RequestedType == TypeData)
            NtfsPurgeStreamCache(FileCB, FileObj, &ByteOffset, Length);
        /* A paging write is not a modification by the file object's owner
         * (it may be any handle's file object backing the cache). */
        if (!PagingIo || !gPagingWriteOutsideLock)
        {
            FileObj->Flags |=
                FO_FILE_MODIFIED |
                FO_FILE_SIZE_CHANGED;
        }
        /* The library stamped this write; the handle's cleanup stamps again. */
        if (!PagingIo && Length != 0 && gPagingWriteOutsideLock)
            FileCB->WriteTimesStamped = TRUE;
        if (!PagingIo)
            FileCB->NotifyFilter |= FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;

        /* A cache flush must not change the application's file pointer. */
        if (!PagingIo && (IrpSp->FileObject->Flags & FO_SYNCHRONOUS_IO))
        {
            // Advance file pointer
            IrpSp->FileObject->CurrentByteOffset.QuadPart = ByteOffset.QuadPart + Length;
        }

        Irp->IoStatus.Information = Length;

        /* FIXME: other uncached writes smaller than a block stay in the library block cache until a flush or its lazy writer */
        if (!PagingIo && Length != 0 &&
            (BooleanFlagOn(FileObj->Flags, FO_WRITE_THROUGH) || BooleanFlagOn(IrpSp->Flags, SL_WRITE_THROUGH)))
        {
            Status = NtfsCommitFileRange(VolCB, FileCB, (ULONGLONG)ByteOffset.QuadPart, Length);
        }
    }
    else
    {
        Irp->IoStatus.Information = 0;
    }

    ExReleaseResourceLite(PagingIo ? NtfsGetPagingIoResource(FileCB)
                                   : NtfsGetMainResource(FileCB));
    KeLeaveCriticalRegion();
    ResourceAcquired = FALSE;

Complete:
    if (ResourceAcquired)
    {
        ExReleaseResourceLite(PagingIo ? NtfsGetPagingIoResource(FileCB)
                                       : NtfsGetMainResource(FileCB));
        KeLeaveCriticalRegion();
    }

    if (!NT_SUCCESS(Status))
        Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_DISK_INCREMENT);
    return Status;
}
