/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NTFS filesystem driver
 * COPYRIGHT:   Copyright 2024 Carl Bialorucki <carl.bialorucki@reactos.org>
 *              Copyright 2024 Justin Miller <justin.miller@reactos.org>
 */

#include "ntfslib_new.h"
#include "ntfslib_new_internal.h"

#define MFT_ALLOCATION_BITMAP_CHUNK_SIZE 0x10000
#define MFT_FIRST_ORDINARY_FILE_RECORD NTFS_FIRST_USER_FILE_RECORD

/*
 * Reserve $MFT data in coarse chunks so its records stay in long runs
 * instead of interleaving one mapping pair per newly touched cluster.
 */
#define MFT_ALLOCATION_GROWTH_CLUSTERS 256
#define NTFS_MAX_SIGNED_OFFSET \
    ((ULONGLONG)0x7fffffffffffffffULL)

/*
 * Every WriteVolume issued while one of these is alive goes through
 * WriteVolumeThrough: the bytes are on the disk when the call returns, even
 * where the environment holds ordinary metadata writes back in a cache.
 */
struct VolumeWriteThroughScope
{
    PVolume Target;

    explicit VolumeWriteThroughScope(_In_ PVolume TargetVolume)
        : Target(TargetVolume)
    {
        if (Target)
            Target->WriteThroughDepth++;
    }

    ~VolumeWriteThroughScope()
    {
        if (Target)
            Target->WriteThroughDepth--;
    }
};

static NTSTATUS
InitializeFileRecordBuffer(
    _In_ PVolume DiskVolume,
    _In_ ULONG FileRecordSize,
    _In_ ULONG FileRecordNumber,
    _In_ USHORT SequenceNumber,
    _In_ ULONGLONG BaseFileReference,
    _In_ BOOLEAN InUse,
    _Inout_ PFileRecord File)
{
    ULONG AttributeOffset;
    ULONG UsaBytes;
    ULONG UsaCount;

    if (!DiskVolume || !File || !File->Data ||
        FileRecordSize < sizeof(FileRecordHeader) ||
        DiskVolume->BytesPerSector == 0 ||
        FileRecordSize % DiskVolume->BytesPerSector != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    UsaCount =
        FileRecordSize / DiskVolume->BytesPerSector + 1;
    if (UsaCount > MAXUSHORT ||
        UsaCount >
            (MAXULONG -
             FIELD_OFFSET(FileRecordHeader,
                          MFTRecordNumber) -
             sizeof(ULONG)) /
                sizeof(USHORT))
    {
        return STATUS_FILE_TOO_LARGE;
    }
    UsaBytes = UsaCount * sizeof(USHORT);
    AttributeOffset = ALIGN_UP_BY(
        FIELD_OFFSET(FileRecordHeader,
                     MFTRecordNumber) +
            sizeof(ULONG) + UsaBytes,
        sizeof(ULONGLONG));
    if (AttributeOffset > MAXUSHORT ||
        AttributeOffset >
            FileRecordSize - 2 * sizeof(ULONG))
    {
        return STATUS_FILE_TOO_LARGE;
    }

    RtlZeroMemory(File->Data, FileRecordSize);
    File->Header =
        reinterpret_cast<PFileRecordHeader>(
            File->Data);
    RtlCopyMemory(File->Header->Header.TypeID,
                  "FILE",
                  sizeof(File->Header->Header.TypeID));
    File->Header->Header.UpdateSequenceOffset =
        (USHORT)(FIELD_OFFSET(FileRecordHeader,
                             MFTRecordNumber) +
                 sizeof(ULONG));
    File->Header->Header.SizeOfUpdateSequence =
        (USHORT)UsaCount;
    File->Header->SequenceNumber =
        SequenceNumber != 0 ? SequenceNumber : 1;
    File->Header->HardLinkCount = 0;
    File->Header->AttributeOffset =
        (USHORT)AttributeOffset;
    File->Header->Flags =
        InUse ? FR_IN_USE : 0;
    File->Header->ActualSize =
        AttributeOffset + 2 * sizeof(ULONG);
    File->Header->AllocatedSize =
        FileRecordSize;
    File->Header->BaseFileRecord =
        InUse ? BaseFileReference : 0;
    File->Header->NextAttributeID = 0;
    File->Header->MFTRecordNumber =
        FileRecordNumber;
    File->RecordNumber = FileRecordNumber;

    *reinterpret_cast<PULONG>(
        File->Data + AttributeOffset) =
            TypeAttributeEndMarker;
    return STATUS_SUCCESS;
}

static NTSTATUS
ReadMftBitmapByte(
    _In_ PFileRecord MftFile,
    _In_ PAttribute BitmapAttribute,
    _In_ ULONGLONG ByteOffset,
    _Out_ PUCHAR Value)
{
    ULONG Length = 1;
    NTSTATUS Status;

    if (!MftFile || !BitmapAttribute || !Value)
        return STATUS_INVALID_PARAMETER;

    Status = MftFile->CopyData(BitmapAttribute,
                               Value,
                               &Length,
                               ByteOffset);
    if (!NT_SUCCESS(Status))
        return Status;
    return Length == 0
        ? STATUS_SUCCESS
        : STATUS_END_OF_FILE;
}

static NTSTATUS
WriteMftBitmapByte(
    _In_ PFileRecord MftFile,
    _In_ ULONGLONG ByteOffset,
    _In_ UCHAR Value)
{
    LARGE_INTEGER Offset;
    ULONG Length = 1;

    if (!MftFile ||
        ByteOffset > NTFS_MAX_SIGNED_OFFSET)
        return STATUS_INVALID_PARAMETER;
    Offset.QuadPart = (LONGLONG)ByteOffset;
    return MftFile->WriteFileData(TypeBitmap,
                                  NULL,
                                  &Value,
                                  &Length,
                                  &Offset);
}

MasterFileTable::MasterFileTable(_In_ PVolume TargetVolume,
                                 _In_ UINT64 MFTLCN,
                                 _In_ UINT64 MFTMirrLCN,
                                 _In_ INT8   ClustersPerFileRecord)
{
    NTSTATUS Status;

    DiskVolume = TargetVolume;
    this->MFTLCN = MFTLCN;
    this->MFTMirrLCN = MFTMirrLCN;

    /* Set the file record size, in bytes.
     * If clusters per file record is less than 0, the file record size is 2^(-ClustersPerFileRecord).
     * Otherwise, the file record size is ClustersPerFileRecord * SectorsPerCluster * BytesPerSector.
     */
    FileRecordSize = ClustersPerFileRecord < 0 ?
                     1 << (-(ClustersPerFileRecord))
                     : ClustersPerFileRecord * BytesPerCluster(DiskVolume);

    // Initialize $MFT
    Status = GetFileRecord(_MFT, &MFTFile);
    if (!NT_SUCCESS(Status))
        DPRINT1("Failed to get $MFT!\n");

    /*
     * Bootstrap the main runlist before loading record 1. $MFTMirr starts
     * with a copy of MFT record 0; it is not the file record describing
     * $MFTMirr. That descriptor is MFT record 1 in the main $MFT stream.
     */
    if (MFTFile)
        MFTDataAttr = MFTFile->GetAttribute(TypeData, NULL);

    // Initialize $MFTMirr
    Status = GetFileRecord(_MFTMirr, &MFTMirrFile);
    if (!NT_SUCCESS(Status))
        DPRINT1("Failed to get $MFTMirr!\n");

    /* Cache the $DATA attributes so GetFileRecord() doesn't re-walk the
     * attribute list on every call. The persistent records memoize their
     * own decoded run lists.
     */
    if (MFTMirrFile)
        MFTMirrDataAttr = MFTMirrFile->GetAttribute(TypeData, NULL);
}

MasterFileTable::~MasterFileTable()
{
    delete[] AllocationScanBuffer;
    AllocationScanBuffer = NULL;

    delete MFTFile;
    delete MFTMirrFile;
}

NTSTATUS
MasterFileTable::QueryVolumeInformation(
    _Out_ PNtfsVolumeInformation Information)
{
    if (!Information)
        return STATUS_INVALID_PARAMETER;
    if (!MFTFile || !MFTDataAttr ||
        !MFTDataAttr->IsNonResident ||
        FileRecordSize == 0)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    Information->BytesPerFileRecordSegment =
        FileRecordSize;
    Information->MftValidDataLength =
        MFTDataAttr->NonResident.DataSize;
    Information->MftStartLcn = MFTLCN;
    Information->Mft2StartLcn = MFTMirrLCN;

    /*
     * The MFT-zone bounds deliberately remain zero.
     * This allocator does not reserve or enforce an MFT zone, so reporting
     * guessed bounds would promise space that is not actually protected.
     */
    return STATUS_SUCCESS;
}

NTSTATUS
MasterFileTable::GetFileRecordDiskOffset(_In_ ULONGLONG RecordNumber,
                                         _Out_ PULONGLONG DiskOffset)
{
    ULONGLONG Offset;
    PDataRun Run;

    if (!MFTFile || !MFTDataAttr || !FileRecordSize ||
        RecordNumber > ~(ULONGLONG)0 / FileRecordSize)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Offset = RecordNumber * FileRecordSize;
    for (Run = MFTFile->GetCachedDataRuns(MFTDataAttr); Run; Run = Run->NextRun)
    {
        ULONGLONG RunSize = GetRunSize(Run);

        if (Offset < RunSize)
        {
            if (Run->IsSparse || RunSize - Offset < FileRecordSize)
                return STATUS_NOT_IMPLEMENTED;
            *DiskOffset = GetOffset(Run->LCN) + Offset;
            return STATUS_SUCCESS;
        }
        Offset -= RunSize;
    }
    return STATUS_NOT_FOUND;
}

NTSTATUS
MasterFileTable::GetMftBitmapByteDiskOffset(_In_ PFileRecord MftFile,
                                            _In_ PAttribute BitmapAttribute,
                                            _In_ ULONGLONG ByteOffset,
                                            _Out_ PULONGLONG DiskOffset,
                                            _Out_ PULONG Length)
{
    ULONGLONG Offset;
    PDataRun Run;

    if (!MftFile || !BitmapAttribute || !DiskVolume || !DiskOffset || !Length)
        return STATUS_INVALID_PARAMETER;

    if (!BitmapAttribute->IsNonResident)
    {
        /* A resident $Bitmap lives inside $MFT record 0; order on that record. */
        ULONGLONG RecordDiskOffset;
        NTSTATUS Status = GetFileRecordDiskOffset(0, &RecordDiskOffset);
        if (!NT_SUCCESS(Status))
            return Status;
        *DiskOffset = RecordDiskOffset;
        *Length = FileRecordSize;
        return STATUS_SUCCESS;
    }

    Offset = ByteOffset;
    for (Run = MftFile->GetCachedDataRuns(BitmapAttribute); Run; Run = Run->NextRun)
    {
        ULONGLONG RunSize = GetRunSize(Run);

        if (Offset < RunSize)
        {
            ULONGLONG Disk;
            ULONG Bpc;

            if (Run->IsSparse)
                return STATUS_NOT_IMPLEMENTED;
            Disk = GetOffset(Run->LCN) + Offset;
            Bpc = BytesPerCluster(DiskVolume);
            if (Bpc == 0)
                Bpc = 512;
            *DiskOffset = Disk - (Disk % Bpc);
            *Length = Bpc;
            return STATUS_SUCCESS;
        }
        Offset -= RunSize;
    }
    return STATUS_NOT_FOUND;
}

NTSTATUS
MasterFileTable::GetAttributeRangeDiskOffset(_In_ PFileRecord File,
                                             _In_ PAttribute Attribute,
                                             _In_ ULONGLONG ByteOffset,
                                             _In_ ULONG Length,
                                             _Out_ PULONGLONG DiskOffset,
                                             _Out_ PULONG OutLength)
{
    ULONGLONG Offset;
    PDataRun Run;

    if (!File || !Attribute || !DiskVolume || !DiskOffset || !OutLength ||
        !Attribute->IsNonResident)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Offset = ByteOffset;
    for (Run = File->GetCachedDataRuns(Attribute); Run; Run = Run->NextRun)
    {
        ULONGLONG RunSize = GetRunSize(Run);

        if (Offset < RunSize)
        {
            if (Run->IsSparse)
                return STATUS_NOT_IMPLEMENTED;
            if (RunSize - Offset < Length)
                return STATUS_NOT_FOUND; /* range crosses a run boundary */
            *DiskOffset = GetOffset(Run->LCN) + Offset;
            *OutLength = Length;
            return STATUS_SUCCESS;
        }
        Offset -= RunSize;
    }
    return STATUS_NOT_FOUND;
}

/*
 * The bitmap extension and every bit set in it are sub-block writes, which the
 * kernel environment holds back in its block cache. Commit them so the bits
 * that describe a new chunk are on the disk before a record from that chunk
 * is handed out; a cut in between otherwise leaves the chunk's bytes as the
 * clusters' prior content under in-use bits with zero records behind them.
 */
NTSTATUS
MasterFileTable::FlushMftBitmap()
{
    PAttribute BitmapAttribute;
    ULONGLONG Remaining;
    NTSTATUS Status;

    if (!MFTFile || !DiskVolume)
        return STATUS_INVALID_PARAMETER;

    BitmapAttribute = MFTFile->GetAttribute(TypeBitmap, NULL);
    if (!BitmapAttribute)
        return STATUS_FILE_CORRUPT_ERROR;

    if (!BitmapAttribute->IsNonResident)
    {
        /* A resident bitmap lives in the $MFT record itself. */
        ULONGLONG DiskOffset;

        Status = GetFileRecordDiskOffset(MFTFile->RecordNumber, &DiskOffset);
        if (!NT_SUCCESS(Status))
            return Status;
        return DiskVolume->FlushVolumeRange(DiskOffset, FileRecordSize);
    }

    if (BytesPerCluster(DiskVolume) == 0)
        return STATUS_FILE_CORRUPT_ERROR;

    Remaining = BitmapAttribute->NonResident.AllocatedSize;
    for (PDataRun Run = MFTFile->GetCachedDataRuns(BitmapAttribute);
         Run && Remaining != 0;
         Run = Run->NextRun)
    {
        ULONGLONG RunBytes;
        ULONGLONG Done = 0;

        if (Run->Length == 0 ||
            Run->Length > ~(ULONGLONG)0 / BytesPerCluster(DiskVolume))
        {
            return STATUS_FILE_CORRUPT_ERROR;
        }
        RunBytes = min(GetRunSize(Run), Remaining);
        while (!Run->IsSparse && Done < RunBytes)
        {
            ULONG Chunk = (ULONG)min(RunBytes - Done, (ULONGLONG)0x100000);

            Status = DiskVolume->FlushVolumeRange(
                GetOffset(Run->LCN) + Done,
                Chunk);
            if (!NT_SUCCESS(Status))
                return Status;
            Done += Chunk;
        }
        Remaining -= RunBytes;
    }
    if (Remaining != 0)
        return STATUS_FILE_CORRUPT_ERROR;

    return STATUS_SUCCESS;
}

NTSTATUS
MasterFileTable::WriteFileRecordToMFT(_In_ PFileRecord File)
{
    PUCHAR CommittedImage = NULL;
    PUCHAR SourceImage;
    NTSTATUS Status, WriteStatus;
    LARGE_INTEGER FileRecordOffset;
    ULONGLONG IndexDiskOffset = 0;
    BOOLEAN SelfReferential;
    BOOLEAN IndexBarrierAttempted = FALSE;
    BOOLEAN MftWriteAttempted = FALSE;

    // TODO: Add logging here

    if (!File || !File->Data || !File->Header ||
        FileRecordSize == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Status = File->CommitFixup();
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Unable to commit fixup! (Status: 0x%X)\n", Status);
        return Status;
    }

    /*
     * The write routes through the $MFT/$MFTMirr attribute walkers. When
     * the record being persisted IS one of those metadata records, the
     * walk would read its own buffer with update-sequence bytes spliced
     * over live attribute bytes, so that one case serializes into a private
     * snapshot and restores the live record before writing. Every ordinary
     * record shares no buffer with the walker and writes its committed
     * buffer directly, reverting the fixup afterward.
     */
    SelfReferential =
        (File == MFTFile || File == MFTMirrFile);
    if (SelfReferential)
    {
        CommittedImage =
            new(PagedPool, TAG_MFT) UCHAR[FileRecordSize];
        if (!CommittedImage)
        {
            (void)File->ApplyFixup();
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(CommittedImage,
                      File->Data,
                      FileRecordSize);
        Status = File->ApplyFixup();
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Unable to revert fixup! (Status: 0x%X)\n", Status);
            delete[] CommittedImage;
            return Status;
        }
        SourceImage = CommittedImage;
    }
    else
    {
        SourceImage = File->Data;
    }

    ULONG FRSize = FileRecordSize;
    ASSERT(File->RecordNumber != MAXULONG);
    FileRecordOffset.QuadPart =
        (LONGLONG)((ULONGLONG)
            File->RecordNumber *
            FileRecordSize);

    /*
     * Directory name insert, barrier mode: while AddFileToDirectory has
     * IndexBarrierRecordOffset armed, every record written through here is
     * part of that insert (the directory record carrying a resident
     * $INDEX_ROOT entry, or the directory record grown by a node split) and
     * names a file record added in the same operation. Order this record
     * after that one so a crash never leaves the entry naming a not-yet-
     * durable record. The constraint goes in BEFORE the write below dirties
     * this record's block: once a block is dirty and unconstrained the lazy
     * writer may take it at any moment, and a constraint added afterwards
     * cannot recall a write already on its way to the device.
     *
     * When the ordering cannot be installed (this record's disk offset is
     * unresolvable, its block is not cached, or the barrier refuses it), the
     * file record is made durable synchronously first, so this record can
     * still be written in any order afterwards. The write never proceeds
     * unordered, and no failure on this path is ignored. The target record
     * itself (same offset) needs no ordering against itself.
     */
    WriteStatus = STATUS_SUCCESS;
    if (DiskVolume->IndexBarrierRecordOffset != 0 &&
        File->RecordNumber != MAXULONG)
    {
        /* RecordNumber is a real MFT index here (the armed window only writes
         * directory/split records, and the ASSERT above holds it). Guarding on
         * the sentinel documents that and skips a needless offset resolve plus
         * record-first commit for a write that carries no index entry. */
        WriteStatus = GetFileRecordDiskOffset(File->RecordNumber, &IndexDiskOffset);
        if (NT_SUCCESS(WriteStatus) &&
            IndexDiskOffset != DiskVolume->IndexBarrierRecordOffset)
        {
            IndexBarrierAttempted = TRUE;
            WriteStatus = DiskVolume->BarrierAfter(
                IndexDiskOffset, FileRecordSize,
                DiskVolume->IndexBarrierRecordOffset, FileRecordSize);
        }
        if (!NT_SUCCESS(WriteStatus))
        {
            /* Record first, synchronously; then this write may go anywhere. */
            WriteStatus = DiskVolume->CommitRange(
                DiskVolume->IndexBarrierRecordOffset, FileRecordSize);
            if (WriteStatus == STATUS_NOT_IMPLEMENTED)
                WriteStatus = DiskVolume->FlushVolume();
            if (IndexBarrierAttempted)
            {
                DiskVolume->BarrierCancel(IndexDiskOffset, FileRecordSize);
                IndexBarrierAttempted = FALSE;
            }
        }
    }

    if (NT_SUCCESS(WriteStatus))
    {
        MftWriteAttempted = TRUE;
        WriteStatus = MFTFile->WriteFileData(TypeData,
                                             NULL,
                                             SourceImage,
                                             &FRSize,
                                             &FileRecordOffset);
    }

    if (!NT_SUCCESS(WriteStatus) && MftWriteAttempted)
    {
        DPRINT1("Unable to write file record %lu! (Status: 0x%X)\n",
                File->RecordNumber,
                WriteStatus);
    }

    if (NT_SUCCESS(WriteStatus) &&
        IsFileRecordInMFTMirr(File->RecordNumber))
    {
        FRSize = FileRecordSize;

        // Write file record to $MFTMirr.
        WriteStatus = MFTMirrFile->WriteFileData(TypeData,
                                                 NULL,
                                                 SourceImage,
                                                 &FRSize,
                                                 &FileRecordOffset);

        if (!NT_SUCCESS(WriteStatus))
        {
            DPRINT1("Unable to write to MFT Mirror! (Status: 0x%X)\n",
                    WriteStatus);
        }
    }

    /* A failed write leaves the block clean; release the constraint it was
     * installed for. A block the $MFT write did dirty keeps it. */
    if (!NT_SUCCESS(WriteStatus) && IndexBarrierAttempted)
        DiskVolume->BarrierCancel(IndexDiskOffset, FileRecordSize);

    if (SelfReferential)
    {
        delete[] CommittedImage;
    }
    else
    {
        Status = File->ApplyFixup();
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Unable to revert fixup! (Status: 0x%X)\n", Status);
            return Status;
        }
    }

    /*
     * User-visible mutation paths prepare their applicable timestamps in the
     * same record commit. This low-level writer also persists NTFS metadata
     * records and therefore must not invent semantic timestamp updates.
     */

    return WriteStatus;
}

NTSTATUS
MasterFileTable::IsFileRecordNumberInUse(_In_  ULONG FileRecordNumber,
                                         _Out_ PBOOLEAN InUse)
{
    NTSTATUS Status;
    UCHAR Bitmask;
    UCHAR BitmapSection;
    ULONG Size;

    if (!MFTFile)
        return STATUS_INVALID_DEVICE_STATE;

    Size = 1;
    Status = MFTFile->CopyData(TypeBitmap,
                               NULL,
                               &BitmapSection,
                               &Size,
                               FileRecordNumber >> 3);

    Bitmask = 1 << (FileRecordNumber % 8);

    if (!NT_SUCCESS(Status) || Size != 0)
    {
        DPRINT1("Failed to get bitmap!\n");
        return NT_SUCCESS(Status) ? STATUS_END_OF_FILE : Status;
    }

    *InUse = !!(BitmapSection & Bitmask);
    return STATUS_SUCCESS;
}

NTSTATUS
MasterFileTable::AllocateExtensionFileRecord(
    _In_ ULONGLONG BaseFileReference,
    _Out_ PFileRecord* File)
{
    if (!File)
        return STATUS_INVALID_PARAMETER;
    *File = NULL;
    if (GetSequenceFromFileRef(
            BaseFileReference) == 0 ||
        GetFRNFromFileRef(
            BaseFileReference) == _MFT ||
        GetFRNFromFileRef(
            BaseFileReference) > MAXULONG)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return AllocateFileRecord(
        BaseFileReference,
        FALSE,
        File);
}

NTSTATUS
MasterFileTable::AllocateBaseFileRecord(
    _In_ BOOLEAN IsDirectory,
    _Out_ PFileRecord* File)
{
    if (!File)
        return STATUS_INVALID_PARAMETER;
    *File = NULL;
    return AllocateFileRecord(
        0,
        IsDirectory,
        File);
}

NTSTATUS
MasterFileTable::GetFileRecordInDirectory(
    _In_  PFileRecord ParentDirectory,
    _In_  PWCHAR Name,
    _Out_ PFileRecord* File)
{
    Directory ParentIndex(DiskVolume);
    PFileRecord FoundFile;
    ULONGLONG FileReference;
    NTSTATUS Status;

    if (!ParentDirectory || !Name || !File)
        return STATUS_INVALID_PARAMETER;
    *File = NULL;

    Status = ParentIndex.FindNextFile(ParentDirectory,
                                      Name,
                                      &FileReference);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The high 16 bits are the sequence number, not part of the ordinal. */
    if (GetFRNFromFileRef(FileReference) > MAXULONG)
        return STATUS_NOT_IMPLEMENTED;
    Status = GetFileRecord(
        (ULONG)(FileReference & 0xFFFFFFFFFFFFULL),
        &FoundFile);
    if (!NT_SUCCESS(Status))
        return Status;
    if (GetSequenceFromFileRef(FileReference) != 0 &&
        FoundFile->Header->SequenceNumber !=
            GetSequenceFromFileRef(FileReference))
    {
        delete FoundFile;
        return STATUS_FILE_CORRUPT_ERROR;
    }

    *File = FoundFile;
    return STATUS_SUCCESS;
}

static NTSTATUS
ExtendMftDataToAllocation(
    _In_ PVolume DiskVolume,
    _In_ PFileRecord MFTFile,
    _In_ ULONG FileRecordSize)
{
    static const UCHAR Zeroes[
        MFT_ALLOCATION_BITMAP_CHUNK_SIZE] = {};
    PAttribute DataAttribute;
    PAttribute BitmapAttribute;
    ULONGLONG Position;
    ULONGLONG RecordBytes;
    ULONGLONG RequiredBitmapLength;
    NTSTATUS Status;

    /*
     * The zeroes must be on the disk before the records and bitmap bits that
     * live in these bytes exist; a cut otherwise leaves whatever the clusters
     * held before, read back as in-use bits over zero records (J36).
     */
    VolumeWriteThroughScope Through(DiskVolume);

    DataAttribute =
        MFTFile->GetAttribute(TypeData, NULL);
    if (!DataAttribute ||
        !DataAttribute->IsNonResident)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    Position =
        DataAttribute->NonResident.DataSize;
    RecordBytes =
        DataAttribute->NonResident.AllocatedSize -
        DataAttribute->NonResident.AllocatedSize %
            FileRecordSize;

    while (Position < RecordBytes)
    {
        LARGE_INTEGER Offset;
        ULONG Length = (ULONG)min(
            RecordBytes - Position,
            (ULONGLONG)sizeof(Zeroes));

        if (Position > NTFS_MAX_SIGNED_OFFSET)
            return STATUS_FILE_TOO_LARGE;
        Offset.QuadPart = (LONGLONG)Position;
        Status = MFTFile->WriteFileData(
            TypeData,
            NULL,
            const_cast<PUCHAR>(Zeroes),
            &Length,
            &Offset);
        if (!NT_SUCCESS(Status))
            return Status;
        Position += Length;
    }

    BitmapAttribute =
        MFTFile->GetAttribute(TypeBitmap, NULL);
    if (!BitmapAttribute)
        return STATUS_FILE_CORRUPT_ERROR;
    Position = GetAttributeDataSize(BitmapAttribute);
    RequiredBitmapLength = ALIGN_UP_BY(
        (RecordBytes / FileRecordSize + 7) / 8,
        sizeof(ULONGLONG));

    while (Position < RequiredBitmapLength)
    {
        LARGE_INTEGER Offset;
        ULONG Length = (ULONG)min(
            RequiredBitmapLength - Position,
            (ULONGLONG)sizeof(Zeroes));

        Offset.QuadPart = (LONGLONG)Position;
        Status = MFTFile->WriteFileData(
            TypeBitmap,
            NULL,
            const_cast<PUCHAR>(Zeroes),
            &Length,
            &Offset);
        if (!NT_SUCCESS(Status))
            return Status;
        Position += Length;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
MasterFileTable::AllocateFileRecord(
    _In_ ULONGLONG BaseFileReference,
    _In_ BOOLEAN IsDirectory,
    _Out_ PFileRecord* File)
{
    PAttribute BitmapAttribute;
    PFileRecord NewFile = NULL;
    PUCHAR BitmapBuffer = NULL;
    ULONGLONG BitmapDataLength;
    ULONGLONG BitmapByteCount;
    ULONGLONG ByteOffset;
    ULONGLONG Candidate = 0;
    ULONGLONG RecordCount;
    ULONGLONG RequiredBitmapLength;
    ULONG ChunkLength;
    ULONG ReadLength;
    USHORT SequenceNumber = 1;
    BOOLEAN Found = FALSE;
    BOOLEAN RecordWritten = FALSE;
    UCHAR BitmapByte;
    UCHAR BitMask;
    NTSTATUS Status;

    if (!File)
        return STATUS_INVALID_PARAMETER;
    *File = NULL;
    if (!DiskVolume || DiskVolume->IsReadOnly ||
        !MFTFile || !MFTDataAttr ||
        !MFTDataAttr->IsNonResident ||
        FileRecordSize == 0)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (BaseFileReference != 0 &&
        (GetSequenceFromFileRef(
             BaseFileReference) == 0 ||
         GetFRNFromFileRef(
             BaseFileReference) == _MFT ||
         GetFRNFromFileRef(
             BaseFileReference) > MAXULONG))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (MFTDataAttr->NonResident.DataSize <
            FileRecordSize ||
        MFTDataAttr->NonResident.DataSize %
            FileRecordSize != 0)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    RecordCount =
        MFTDataAttr->NonResident.DataSize /
        FileRecordSize;
    if (RecordCount <
        MFT_FIRST_ORDINARY_FILE_RECORD)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    BitmapAttribute =
        MFTFile->GetAttribute(TypeBitmap, NULL);
    if (!BitmapAttribute)
        return STATUS_FILE_CORRUPT_ERROR;
    BitmapDataLength =
        GetAttributeDataSize(BitmapAttribute);
    BitmapByteCount =
        (RecordCount + 7) / 8;
    if (BitmapDataLength < BitmapByteCount)
        return STATUS_FILE_CORRUPT_ERROR;
    if (!AllocationScanBuffer)
    {
        AllocationScanBuffer =
            new(PagedPool, TAG_MFT)
                UCHAR[MFT_ALLOCATION_BITMAP_CHUNK_SIZE];
    }
    BitmapBuffer = AllocationScanBuffer;
    if (!BitmapBuffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    /*
     * Start where the last allocation left off. If that finds nothing, the
     * second pass covers the skipped region, so a full bitmap is still
     * detected and freed records are still reused.
     */
    ULONGLONG ScanStart = MFT_FIRST_ORDINARY_FILE_RECORD / 8;
    ULONGLONG ScanLimit = BitmapByteCount;
    BOOLEAN SecondPass = FALSE;

    if (FreeRecordHint / 8 > ScanStart &&
        FreeRecordHint / 8 < BitmapByteCount)
    {
        ScanStart = FreeRecordHint / 8;
    }

ScanAgain:
    ByteOffset = ScanStart;
    while (ByteOffset < ScanLimit)
    {
        ChunkLength = (ULONG)min(
            BitmapByteCount - ByteOffset,
            (ULONGLONG)
                MFT_ALLOCATION_BITMAP_CHUNK_SIZE);
        ReadLength = ChunkLength;
        Status = MFTFile->CopyData(
            BitmapAttribute,
            BitmapBuffer,
            &ReadLength,
            ByteOffset);
        if (!NT_SUCCESS(Status) || ReadLength != 0)
        {
            if (NT_SUCCESS(Status))
                Status = STATUS_END_OF_FILE;
            goto Done;
        }

        for (ULONG Index = 0;
             Index < ChunkLength;
             Index++)
        {
            UCHAR ValidMask = 0xff;
            ULONGLONG FirstRecord =
                (ByteOffset + Index) * 8;

            if (FirstRecord + 8 > RecordCount)
            {
                ULONG ValidBits =
                    (ULONG)(RecordCount -
                            FirstRecord);
                ValidMask = (UCHAR)(
                    (1u << ValidBits) - 1u);
            }
            if ((BitmapBuffer[Index] &
                 ValidMask) != ValidMask)
            {
                for (ULONG Bit = 0;
                     Bit < 8;
                     Bit++)
                {
                    if ((ValidMask & (1u << Bit)) &&
                        !(BitmapBuffer[Index] &
                          (1u << Bit)) &&
                        FirstRecord + Bit >=
                            MFT_FIRST_ORDINARY_FILE_RECORD)
                    {
                        Candidate =
                            FirstRecord + Bit;
                        Found = TRUE;
                        break;
                    }
                }
            }
            if (Found)
                break;
        }
        if (Found)
            break;
        ByteOffset += ChunkLength;
    }

    if (!Found && !SecondPass &&
        ScanStart > MFT_FIRST_ORDINARY_FILE_RECORD / 8)
    {
        /* Cover the region the hint skipped over. */
        SecondPass = TRUE;
        ScanLimit = ScanStart;
        ScanStart = MFT_FIRST_ORDINARY_FILE_RECORD / 8;
        goto ScanAgain;
    }

    if (Found)
        FreeRecordHint = Candidate + 1;

    if (!Found)
    {
        Candidate = RecordCount;
        if (Candidate > MAXULONG ||
            Candidate >
                NTFS_MAX_SIGNED_OFFSET /
                    FileRecordSize)
        {
            Status = STATUS_FILE_TOO_LARGE;
            goto Done;
        }
    }

    RequiredBitmapLength =
        ALIGN_UP_BY((Candidate + 8) / 8,
                    sizeof(ULONGLONG));
    if (RequiredBitmapLength > BitmapDataLength)
    {
        static const UCHAR Zeroes[
            MFT_ALLOCATION_BITMAP_CHUNK_SIZE] = {};
        ULONGLONG Position = BitmapDataLength;

        while (Position < RequiredBitmapLength)
        {
            LARGE_INTEGER Offset;
            ULONG Length = (ULONG)min(
                RequiredBitmapLength - Position,
                (ULONGLONG)sizeof(Zeroes));

            if (Position >
                NTFS_MAX_SIGNED_OFFSET)
            {
                Status = STATUS_FILE_TOO_LARGE;
                goto Done;
            }
            Offset.QuadPart = (LONGLONG)Position;
            Status = MFTFile->WriteFileData(
                TypeBitmap,
                NULL,
                const_cast<PUCHAR>(Zeroes),
                &Length,
                &Offset);
            if (!NT_SUCCESS(Status))
                goto Done;
            Position += Length;
        }
        BitmapAttribute =
            MFTFile->GetAttribute(TypeBitmap,
                                  NULL);
        if (!BitmapAttribute ||
            GetAttributeDataSize(BitmapAttribute) <
                RequiredBitmapLength)
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            goto Done;
        }
    }

    if (Candidate < RecordCount)
    {
        PUCHAR RawRecord = NULL;

        /* Record-sized scratch comes from the volume's free list; a pool
         * round trip per create costs more than the sequence check. */
        NtfsAcquireMutex(&DiskVolume->RecordPoolMutex);
        if (DiskVolume->RecordPoolHead &&
            DiskVolume->RecordPoolBufferSize == FileRecordSize)
        {
            RawRecord = DiskVolume->RecordPoolHead;
            DiskVolume->RecordPoolHead = *(PUCHAR*)RawRecord;
            DiskVolume->RecordPoolCount--;
        }
        NtfsReleaseMutex(&DiskVolume->RecordPoolMutex);
        if (!RawRecord)
        {
            RawRecord = new(PagedPool, TAG_MFT) UCHAR[FileRecordSize];
        }
        if (!RawRecord)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Done;
        }
        ReadLength = FileRecordSize;
        Status = MFTFile->CopyData(
            MFTDataAttr,
            RawRecord,
            &ReadLength,
            Candidate * FileRecordSize);
        if (NT_SUCCESS(Status) &&
            ReadLength == 0 &&
            RtlCompareMemory(RawRecord,
                             "FILE",
                             4) == 4)
        {
            PFileRecordHeader OldHeader =
                reinterpret_cast<PFileRecordHeader>(
                    RawRecord);
            if ((!FileRecordHeaderHasNumber(OldHeader) ||
                 OldHeader->MFTRecordNumber ==
                    Candidate) &&
                OldHeader->SequenceNumber != 0)
            {
                SequenceNumber =
                    OldHeader->SequenceNumber;
            }
        }
        {
            BOOLEAN Pooled = FALSE;

            NtfsAcquireMutex(&DiskVolume->RecordPoolMutex);
            if (DiskVolume->RecordPoolCount < 512 &&
                (DiskVolume->RecordPoolCount == 0 ||
                 DiskVolume->RecordPoolBufferSize == FileRecordSize))
            {
                *(PUCHAR*)RawRecord = DiskVolume->RecordPoolHead;
                DiskVolume->RecordPoolHead = RawRecord;
                DiskVolume->RecordPoolBufferSize = FileRecordSize;
                DiskVolume->RecordPoolCount++;
                Pooled = TRUE;
            }
            NtfsReleaseMutex(&DiskVolume->RecordPoolMutex);
            if (!Pooled)
                delete[] RawRecord;
        }
        if (!NT_SUCCESS(Status) ||
            ReadLength != 0)
        {
            if (NT_SUCCESS(Status))
                Status = STATUS_END_OF_FILE;
            goto Done;
        }
    }

    NewFile = new(PagedPool, TAG_MFT)
        FileRecord(DiskVolume, FileRecordSize);
    if (!NewFile)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }
    Status = InitializeFileRecordBuffer(
        DiskVolume,
        FileRecordSize,
        (ULONG)Candidate,
        SequenceNumber,
        BaseFileReference,
        TRUE,
        NewFile);
    if (!NT_SUCCESS(Status))
        goto Done;
    if (IsDirectory)
        NewFile->Header->Flags |= FR_IS_DIRECTORY;

    /*
     * Growing $MFT one record at a time interleaves its clusters with
     * ordinary allocations and floods record 0 with mapping pairs.
     * Reserve allocation in coarse chunks so the stream stays in long
     * runs; the record write below then never extends on its own.
     */
    {
        const ULONGLONG GrowthChunk =
            (ULONGLONG)MFT_ALLOCATION_GROWTH_CLUSTERS *
            BytesPerCluster(DiskVolume);
        ULONGLONG RecordEnd =
            ((ULONGLONG)Candidate + 1) *
            FileRecordSize;
        PAttribute CurrentDataAttr =
            MFTFile->GetAttribute(TypeData,
                                  NULL);

        if (CurrentDataAttr &&
            CurrentDataAttr->IsNonResident &&
            RecordEnd >
                CurrentDataAttr->
                    NonResident.AllocatedSize &&
            RecordEnd <=
                ~(ULONGLONG)0 - GrowthChunk)
        {
            NTSTATUS GrowthStatus =
                MFTFile->SetFileAllocationSize(
                    TypeData,
                    NULL,
                    ALIGN_UP_BY(RecordEnd,
                                GrowthChunk));
            if (!NT_SUCCESS(GrowthStatus))
            {
                DPRINT1(
                    "Chunked $MFT preallocation "
                    "failed: 0x%lx.\n",
                    GrowthStatus);
            }
            else
            {
                Status = ExtendMftDataToAllocation(
                    DiskVolume,
                    MFTFile,
                    FileRecordSize);
                if (!NT_SUCCESS(Status))
                    goto Done;
                Status = FlushMftBitmap();
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1(
                        "Flushing the $MFT bitmap after "
                        "growth failed: 0x%lx.\n",
                        Status);
                    goto Done;
                }
            }
        }
    }

    Status = WriteFileRecordToMFT(NewFile);
    if (!NT_SUCCESS(Status))
        goto Done;
    RecordWritten = TRUE;

    /*
     * A record past the old end of the $MFT data means the MFT just grew.
     * The zero-filled clusters went straight to the disk, but the grown
     * data size in record 0, the bitmap extension and this record sit in
     * the write-back cache, which commits blocks in no particular order. A
     * power cut that kept the bitmap bit set below but lost record 0 would
     * leave the bit naming a record past the on-disk end, which reads as
     * zeros, and later index entries pointing at it. Make all of it durable
     * before the bit is set. MFT growth happens once per growth chunk, so
     * the extra flush is rare.
     */
    if (Candidate >= RecordCount)
    {
        Status = DiskVolume->FlushVolume();
        if (!NT_SUCCESS(Status))
            goto Done;
    }
    else
    {
        /*
         * Inside a chunk that has already grown, the record, its bitmap bit
         * and the parent index entry are separate blocks of the write-back
         * cache, committed in no particular order. A power cut that kept
         * the bit and the index entry but not the record left a zeroed
         * record that both name (JOB28 PC2-P: 136 records). Commit the
         * record's own block first, so nothing can name it before it is on
         * the disk. Only this record's range is written, not the volume.
         */
        /* Barrier OFF (default): synchronous CommitRange, as bc86485f. Barrier
         * ON: no synchronous write here; the record-before-bit ordering is
         * installed below, right before the bitmap bit is written
         * (BarrierAfter). */
        if (!DiskVolume->WriteBarrierEnabled())
        {
            ULONGLONG RecordDiskOffset;

            Status = GetFileRecordDiskOffset(Candidate, &RecordDiskOffset);
            if (NT_SUCCESS(Status))
                Status = DiskVolume->CommitRange(RecordDiskOffset, FileRecordSize);
            else if (Status == STATUS_NOT_IMPLEMENTED)
                Status = DiskVolume->FlushVolume(); /* record spans a run boundary */
            if (!NT_SUCCESS(Status))
                goto Done;
        }
    }

    BitmapAttribute =
        MFTFile->GetAttribute(TypeBitmap, NULL);
    if (!BitmapAttribute)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Done;
    }
    ByteOffset = Candidate >> 3;
    Status = ReadMftBitmapByte(
        MFTFile,
        BitmapAttribute,
        ByteOffset,
        &BitmapByte);
    if (!NT_SUCCESS(Status))
        goto Done;
    BitMask = (UCHAR)(1u << (Candidate & 7));
    if (BitmapByte & BitMask)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Done;
    }
    {
        ULONGLONG BarrierRecordOffset = 0;
        ULONGLONG BarrierBitOffset = 0;
        ULONG BarrierBitLength = 0;
        BOOLEAN BarrierAttempted = FALSE;

        /*
         * Barrier ON: the bit block cannot reach the device until the record
         * block is durable, with no synchronous write. The constraint is
         * installed BEFORE the bit is written: a block that is dirty and
         * unconstrained, even briefly, may be taken by the lazy writer, and
         * a constraint added afterwards cannot recall it. When the ordering
         * cannot be installed (an offset is unresolvable, the bit block is
         * not cached, or the barrier refuses it) the record is committed
         * synchronously first, and only then is the bit written; a record
         * that spans a run boundary is made durable by a full flush. On a
         * crash the bit is never on disk without the record.
         */
        if (Candidate < RecordCount && DiskVolume->WriteBarrierEnabled())
        {
            Status = GetFileRecordDiskOffset(Candidate, &BarrierRecordOffset);
            if (Status == STATUS_NOT_IMPLEMENTED)
            {
                /* Record spans a run boundary: no single range to order on. */
                Status = DiskVolume->FlushVolume();
                if (!NT_SUCCESS(Status))
                    goto Done;
            }
            else
            {
                if (!NT_SUCCESS(Status))
                    goto Done;
                Status = GetMftBitmapByteDiskOffset(
                    MFTFile, BitmapAttribute, ByteOffset,
                    &BarrierBitOffset, &BarrierBitLength);
                if (NT_SUCCESS(Status))
                {
                    BarrierAttempted = TRUE;
                    Status = DiskVolume->BarrierAfter(
                        BarrierBitOffset, BarrierBitLength,
                        BarrierRecordOffset, FileRecordSize);
                }
                if (!NT_SUCCESS(Status))
                {
                    /* Record first, synchronously; then the bit may go anywhere. */
                    Status = DiskVolume->CommitRange(BarrierRecordOffset, FileRecordSize);
                    if (Status == STATUS_NOT_IMPLEMENTED)
                        Status = DiskVolume->FlushVolume();
                    if (BarrierAttempted)
                    {
                        DiskVolume->BarrierCancel(BarrierBitOffset, BarrierBitLength);
                        BarrierAttempted = FALSE;
                    }
                    if (!NT_SUCCESS(Status))
                        goto Done;
                }
            }
        }

        Status = WriteMftBitmapByte(
            MFTFile,
            ByteOffset,
            (UCHAR)(BitmapByte | BitMask));
        if (!NT_SUCCESS(Status))
        {
            /* The bit block stayed clean; release the constraint. */
            if (BarrierAttempted)
                DiskVolume->BarrierCancel(BarrierBitOffset, BarrierBitLength);
            goto Done;
        }
    }

    *File = NewFile;
    NewFile = NULL;
    Status = STATUS_SUCCESS;

Done:
    if (!NT_SUCCESS(Status) &&
        RecordWritten && NewFile)
    {
        (void)InitializeFileRecordBuffer(
            DiskVolume,
            FileRecordSize,
            NewFile->RecordNumber,
            NewFile->Header->SequenceNumber,
            0,
            FALSE,
            NewFile);
        (void)WriteFileRecordToMFT(NewFile);
    }
    delete NewFile;
    /* BitmapBuffer is the table's persistent scratch; it stays. */
    return Status;
}

NTSTATUS
MasterFileTable::DeallocateExtensionFileRecord(
    _Inout_ PFileRecord File)
{
    if (!File || !File->Header ||
        File->Header->BaseFileRecord == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return DeallocateFileRecord(File);
}

NTSTATUS
MasterFileTable::DeallocateBaseFileRecord(
    _Inout_ PFileRecord File)
{
    if (!File || !File->Header ||
        File->Header->BaseFileRecord != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return DeallocateFileRecord(File);
}

NTSTATUS
MasterFileTable::DeallocateFileRecord(
    _Inout_ PFileRecord File)
{
    PAttribute BitmapAttribute;
    ULONG FileRecordNumber;
    USHORT SequenceNumber;
    ULONGLONG ByteOffset;
    UCHAR BitmapByte;
    UCHAR BitMask;
    NTSTATUS Status;

    if (!DiskVolume || DiskVolume->IsReadOnly ||
        !MFTFile || !File || !File->Header ||
        !(File->Header->Flags & FR_IN_USE) ||
        File->RecordNumber <
            MFT_FIRST_ORDINARY_FILE_RECORD)
    {
        return STATUS_INVALID_PARAMETER;
    }

    FileRecordNumber =
        File->RecordNumber;

    /* The slot is free again, so let the next allocation see it. */
    if (File->RecordNumber < FreeRecordHint)
        FreeRecordHint = File->RecordNumber;
    BitmapAttribute =
        MFTFile->GetAttribute(TypeBitmap, NULL);
    if (!BitmapAttribute)
        return STATUS_FILE_CORRUPT_ERROR;
    ByteOffset = FileRecordNumber >> 3;
    Status = ReadMftBitmapByte(
        MFTFile,
        BitmapAttribute,
        ByteOffset,
        &BitmapByte);
    if (!NT_SUCCESS(Status))
        return Status;

    BitMask =
        (UCHAR)(1u << (FileRecordNumber & 7));
    if (!(BitmapByte & BitMask))
        return STATUS_NOT_FOUND;

    SequenceNumber =
        File->Header->SequenceNumber + 1;
    if (SequenceNumber == 0)
        SequenceNumber = 1;
    Status = InitializeFileRecordBuffer(
        DiskVolume,
        FileRecordSize,
        FileRecordNumber,
        SequenceNumber,
        0,
        FALSE,
        File);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = WriteFileRecordToMFT(File);
    if (!NT_SUCCESS(Status))
        return Status;

    /*
     * Publish the record as free only after its in-use contents are gone.
     * A bitmap write failure therefore leaks a blank record rather than
     * allowing a later allocation to reuse a still-live extension record.
     */
    return WriteMftBitmapByte(
        MFTFile,
        ByteOffset,
        (UCHAR)(BitmapByte & ~BitMask));
}
