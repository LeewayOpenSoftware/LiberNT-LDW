/*
 * PROJECT:     ReactOS NTFS library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Mutate the NTFS $I30 directory index
 */

#include "ntfslib_new.h"
#include "ntfslib_new_internal.h"

#define NTFS_INDEX_HEADER_LARGE 1

/*
 * Multi-record updates below are sequenced so every intermediate on-disk
 * state keeps the tree readable: new index buffers become reachable only
 * after their content and bitmap bits are durable, and the root switch is
 * always the final single-record commit. Crash atomicity across the whole
 * sequence still requires the future LFS transaction work.
 */

static BOOLEAN
IsIndexEntryValid(
    _In_ PIndexEntry Entry,
    _In_ ULONG Remaining,
    _In_ BOOLEAN ViewIndex)
{
    ULONG HeaderSize = FIELD_OFFSET(IndexEntry, IndexStream);
    ULONG PayloadEnd;

    if (!ViewIndex)
        return NtfsIsDirectoryIndexEntryValid(Entry, Remaining);

    if (Remaining < HeaderSize ||
        Entry->EntryLength < HeaderSize ||
        (Entry->EntryLength & 7) != 0 ||
        Entry->EntryLength > Remaining ||
        (Entry->Flags & ~(INDEX_ENTRY_NODE | INDEX_ENTRY_END)) != 0)
    {
        return FALSE;
    }

    PayloadEnd = Entry->EntryLength;
    if (Entry->Flags & INDEX_ENTRY_NODE)
    {
        if (PayloadEnd < HeaderSize + sizeof(ULONGLONG))
            return FALSE;
        PayloadEnd -= sizeof(ULONGLONG);
    }
    if (Entry->StreamLength > PayloadEnd - HeaderSize)
        return FALSE;
    if (Entry->Flags & INDEX_ENTRY_END)
        return Entry->StreamLength == 0;

    if (Entry->Data.ViewIndex.DataLength == 0)
        return TRUE;
    return Entry->Data.ViewIndex.DataOffset >=
               HeaderSize + Entry->StreamLength &&
           Entry->Data.ViewIndex.DataOffset <= PayloadEnd &&
           Entry->Data.ViewIndex.DataLength <=
               PayloadEnd - Entry->Data.ViewIndex.DataOffset;
}

static ULONG
ReadIndexKeyUlong(
    _In_reads_bytes_(sizeof(ULONG)) const UCHAR* Value)
{
    return (ULONG)Value[0] |
           ((ULONG)Value[1] << 8) |
           ((ULONG)Value[2] << 16) |
           ((ULONG)Value[3] << 24);
}

static LONG
CompareIndexUlongs(
    _In_ ULONG Left,
    _In_ ULONG Right)
{
    return Left < Right ? -1 : (Left > Right ? 1 : 0);
}

static NTSTATUS
CompareIndexKey(
    _In_ PVolume DiskVolume,
    _In_ const IndexSearchKey* Key,
    _In_ PIndexEntry Entry,
    _Out_ LONG* Comparison)
{
    PFileNameEx IndexedName;
    UNICODE_STRING IndexedString;

    switch (Key->CollationRule)
    {
    case ATTRDEF_COLLATION_FILENAME:
        IndexedName =
            reinterpret_cast<PFileNameEx>(
                Entry->IndexStream);
        IndexedString =
            NtfsMakeCountedUnicodeString(
                IndexedName->Name,
                IndexedName->NameLength *
                    sizeof(WCHAR));
        return DiskVolume->CollateFileNames(
            Key->Name,
            &IndexedString,
            Key->CaseSensitive,
            Comparison);

    case ATTRDEF_COLLATION_ULONG:
        if (Key->ValueLength != sizeof(ULONG) ||
            Entry->StreamLength != sizeof(ULONG))
        {
            return STATUS_FILE_CORRUPT_ERROR;
        }
        *Comparison = CompareIndexUlongs(
            ReadIndexKeyUlong(Key->Value),
            ReadIndexKeyUlong(Entry->IndexStream));
        return STATUS_SUCCESS;

    case ATTRDEF_COLLATION_SEC_HASH:
        if (Key->ValueLength != 2 * sizeof(ULONG) ||
            Entry->StreamLength != 2 * sizeof(ULONG))
        {
            return STATUS_FILE_CORRUPT_ERROR;
        }
        *Comparison = CompareIndexUlongs(
            ReadIndexKeyUlong(Key->Value),
            ReadIndexKeyUlong(Entry->IndexStream));
        if (*Comparison == 0)
        {
            *Comparison = CompareIndexUlongs(
                ReadIndexKeyUlong(Key->Value + sizeof(ULONG)),
                ReadIndexKeyUlong(Entry->IndexStream + sizeof(ULONG)));
        }
        return STATUS_SUCCESS;

    case ATTRDEF_COLLATION_ULONG_MULTI:
        if (Key->ValueLength == 0 ||
            Key->ValueLength % sizeof(ULONG) != 0 ||
            Entry->StreamLength != Key->ValueLength)
        {
            return STATUS_FILE_CORRUPT_ERROR;
        }
        *Comparison = 0;
        for (ULONG Offset = 0;
             Offset < Key->ValueLength && *Comparison == 0;
             Offset += sizeof(ULONG))
        {
            *Comparison = CompareIndexUlongs(
                ReadIndexKeyUlong(Key->Value + Offset),
                ReadIndexKeyUlong(Entry->IndexStream + Offset));
        }
        return STATUS_SUCCESS;

    default:
        return STATUS_NOT_IMPLEMENTED;
    }
}

/*
 * Locates where Name belongs in one index node. Insertion callers leave
 * FoundExact NULL and receive STATUS_OBJECT_NAME_COLLISION for an exact
 * match; removal callers receive the matched entry's offset instead.
 */
static NTSTATUS
FindIndexInsertionPoint(
    _In_ PVolume DiskVolume,
    _In_ PIndexNodeHeader Header,
    _In_ ULONG HeaderBytes,
    _In_ const IndexSearchKey* Key,
    _Out_ PULONG InsertionOffset,
    _Out_ PULONGLONG ChildVcn,
    _Out_ PBOOLEAN Descend,
    _Out_opt_ PBOOLEAN FoundExact = NULL)
{
    PIndexEntry Entry;
    ULONG_PTR End;
    LONG Comparison;
    BOOLEAN Large;
    BOOLEAN FoundEnd = FALSE;
    NTSTATUS Status;

    if (!DiskVolume || !Header || !Key ||
        !InsertionOffset || !ChildVcn ||
        !Descend)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *InsertionOffset = 0;
    *ChildVcn = 0;
    *Descend = FALSE;
    if (FoundExact)
        *FoundExact = FALSE;

    if (HeaderBytes < sizeof(*Header) ||
        Header->IndexOffset < sizeof(*Header) ||
        Header->IndexOffset >
            Header->TotalIndexSize ||
        Header->TotalIndexSize >
            Header->AllocatedSize ||
        Header->AllocatedSize >
            HeaderBytes ||
        (Header->Flags &
         ~NTFS_INDEX_HEADER_LARGE) != 0)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    Large =
        !!(Header->Flags &
           NTFS_INDEX_HEADER_LARGE);

    Entry =
        reinterpret_cast<PIndexEntry>(
            reinterpret_cast<PUCHAR>(Header) +
            Header->IndexOffset);
    End =
        reinterpret_cast<ULONG_PTR>(Header) +
        Header->TotalIndexSize;
    while (reinterpret_cast<ULONG_PTR>(Entry) <
           End)
    {
        BOOLEAN HasChild;

        if (!IsIndexEntryValid(
                Entry,
                (ULONG)(End -
                    reinterpret_cast<ULONG_PTR>(
                        Entry)),
                Key->CollationRule !=
                    ATTRDEF_COLLATION_FILENAME))
        {
            return STATUS_FILE_CORRUPT_ERROR;
        }
        HasChild =
            !!(Entry->Flags & INDEX_ENTRY_NODE);
        if (HasChild != Large)
            return STATUS_FILE_CORRUPT_ERROR;

        if (Entry->Flags & INDEX_ENTRY_END)
        {
            if (reinterpret_cast<ULONG_PTR>(Entry) +
                    Entry->EntryLength != End)
            {
                return STATUS_FILE_CORRUPT_ERROR;
            }
            FoundEnd = TRUE;
            if (HasChild)
            {
                *ChildVcn =
                    *GetSubnodeVCN(Entry);
                *Descend = TRUE;
            }
            else
            {
                *InsertionOffset =
                    (ULONG)(
                        reinterpret_cast<PUCHAR>(
                            Entry) -
                        reinterpret_cast<PUCHAR>(
                            Header));
            }
            break;
        }

        Status = CompareIndexKey(
            DiskVolume,
            Key,
            Entry,
            &Comparison);
        if (!NT_SUCCESS(Status))
            return Status;
        if (Comparison == 0)
        {
            if (!FoundExact)
                return STATUS_OBJECT_NAME_COLLISION;
            *FoundExact = TRUE;
            *InsertionOffset =
                (ULONG)(
                    reinterpret_cast<PUCHAR>(
                        Entry) -
                    reinterpret_cast<PUCHAR>(
                        Header));
            return STATUS_SUCCESS;
        }
        if (Comparison < 0)
        {
            if (HasChild)
            {
                *ChildVcn =
                    *GetSubnodeVCN(Entry);
                *Descend = TRUE;
            }
            else
            {
                *InsertionOffset =
                    (ULONG)(
                        reinterpret_cast<PUCHAR>(
                            Entry) -
                        reinterpret_cast<PUCHAR>(
                            Header));
            }
            return STATUS_SUCCESS;
        }

        Entry =
            reinterpret_cast<PIndexEntry>(
                reinterpret_cast<PUCHAR>(Entry) +
                Entry->EntryLength);
    }

    return FoundEnd
        ? STATUS_SUCCESS
        : STATUS_FILE_CORRUPT_ERROR;
}

static NTSTATUS
FindChildLinkOffset(
    _In_ PIndexNodeHeader Header,
    _In_ ULONGLONG ChildVcn,
    _Out_ PULONG LinkOffset,
    _In_ BOOLEAN ViewIndex)
{
    PIndexEntry Entry;
    ULONG_PTR End;

    *LinkOffset = 0;
    if ((Header->Flags &
         NTFS_INDEX_HEADER_LARGE) == 0)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    Entry =
        reinterpret_cast<PIndexEntry>(
            reinterpret_cast<PUCHAR>(Header) +
            Header->IndexOffset);
    End =
        reinterpret_cast<ULONG_PTR>(Header) +
        Header->TotalIndexSize;
    while (reinterpret_cast<ULONG_PTR>(Entry) <
           End)
    {
        if (!IsIndexEntryValid(
                Entry,
                (ULONG)(End -
                    reinterpret_cast<ULONG_PTR>(
                        Entry)),
                ViewIndex) ||
            !(Entry->Flags & INDEX_ENTRY_NODE))
        {
            return STATUS_FILE_CORRUPT_ERROR;
        }
        if (*GetSubnodeVCN(Entry) == ChildVcn)
        {
            *LinkOffset =
                (ULONG)(
                    reinterpret_cast<PUCHAR>(
                        Entry) -
                    reinterpret_cast<PUCHAR>(
                        Header));
            return STATUS_SUCCESS;
        }
        if (Entry->Flags & INDEX_ENTRY_END)
            break;
        Entry =
            reinterpret_cast<PIndexEntry>(
                reinterpret_cast<PUCHAR>(Entry) +
                Entry->EntryLength);
    }

    return STATUS_FILE_CORRUPT_ERROR;
}

static NTSTATUS
BuildIndexEntry(
    _In_ ULONGLONG FileReference,
    _In_ PFileNameEx FileName,
    _Outptr_result_bytebuffer_(*EntryLength)
        PIndexEntry* NewEntry,
    _Out_ PULONG EntryLength)
{
    ULONG FileNameLength;
    ULONG Length;

    if (!FileName || !NewEntry ||
        !EntryLength ||
        FileName->NameLength == 0 ||
        FileName->NameType >
            NAME_TYPE_WIN32_AND_DOS ||
        GetSequenceFromFileRef(
            FileReference) == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *NewEntry = NULL;
    *EntryLength = 0;

    FileNameLength =
        FIELD_OFFSET(FileNameEx, Name) +
        FileName->NameLength * sizeof(WCHAR);
    if (FileNameLength > MAXUSHORT ||
        FileNameLength >
            MAXULONG -
            FIELD_OFFSET(IndexEntry,
                         IndexStream))
    {
        return STATUS_NAME_TOO_LONG;
    }
    Length = ALIGN_UP_BY(
        FIELD_OFFSET(IndexEntry,
                     IndexStream) +
            FileNameLength,
        sizeof(ULONGLONG));
    if (Length > MAXUSHORT)
        return STATUS_NAME_TOO_LONG;

    *NewEntry =
        reinterpret_cast<PIndexEntry>(
            NtfsAllocatePoolWithTag(
                PagedPool,
                Length,
                TAG_BTREE));
    if (!*NewEntry)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(*NewEntry,
                  Length);
    (*NewEntry)->Data.Directory.IndexedFile =
        FileReference;
    (*NewEntry)->EntryLength =
        (USHORT)Length;
    (*NewEntry)->StreamLength =
        (USHORT)FileNameLength;
    RtlCopyMemory((*NewEntry)->IndexStream,
                  FileName,
                  FileNameLength);
    *EntryLength = Length;
    return STATUS_SUCCESS;
}

/*
 * Copies one node's complete entry list (terminating END entry included)
 * while splicing PendingEntry in front of the entry at InsertionOffset.
 * With Retarget set, the displaced entry additionally receives a new
 * child VCN; this is how a split publishes its right node in the parent.
 */
static NTSTATUS
ComposeEntryList(
    _In_ PIndexNodeHeader Header,
    _In_ ULONG InsertionOffset,
    _In_ PIndexEntry PendingEntry,
    _In_ ULONG PendingLength,
    _In_ BOOLEAN Retarget,
    _In_ ULONGLONG RetargetVcn,
    _Out_writes_bytes_(*ListBytes) PUCHAR List,
    _Out_ PULONG ListBytes)
{
    PIndexEntry Displaced;
    ULONG ListStart = Header->IndexOffset;
    ULONG ListEnd = Header->TotalIndexSize;
    ULONG Prefix;

    if (InsertionOffset < ListStart ||
        InsertionOffset >= ListEnd)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    Prefix = InsertionOffset - ListStart;

    RtlCopyMemory(
        List,
        reinterpret_cast<PUCHAR>(Header) +
            ListStart,
        Prefix);
    RtlCopyMemory(List + Prefix,
                  PendingEntry,
                  PendingLength);
    RtlCopyMemory(
        List + Prefix + PendingLength,
        reinterpret_cast<PUCHAR>(Header) +
            InsertionOffset,
        ListEnd - InsertionOffset);
    *ListBytes =
        ListEnd - ListStart + PendingLength;

    if (Retarget)
    {
        Displaced =
            reinterpret_cast<PIndexEntry>(
                List + Prefix + PendingLength);
        if (!(Displaced->Flags &
              INDEX_ENTRY_NODE))
        {
            return STATUS_FILE_CORRUPT_ERROR;
        }
        *GetSubnodeVCN(Displaced) = RetargetVcn;
    }
    return STATUS_SUCCESS;
}

/*
 * Selects the promoted median of an entry list that no longer fits its
 * node: the first entry carrying at least half of the accumulated entry
 * bytes. Both resulting halves may be empty of ordinary entries; the
 * terminating END entry always stays with the right half.
 */
static NTSTATUS
SplitEntryList(
    _In_reads_bytes_(ListBytes) PUCHAR List,
    _In_ ULONG ListBytes,
    _Out_ PULONG LeftBytes,
    _Out_ PULONG MedianOffset,
    _Out_ PULONG MedianLength,
    _In_ BOOLEAN ViewIndex)
{
    PIndexEntry Entry;
    ULONG Offset = 0;
    ULONG PayloadBytes = 0;
    ULONG Accumulated = 0;

    *LeftBytes = 0;
    *MedianOffset = 0;
    *MedianLength = 0;

    while (Offset < ListBytes)
    {
        Entry =
            reinterpret_cast<PIndexEntry>(
                List + Offset);
        if (!IsIndexEntryValid(
                Entry,
                ListBytes - Offset,
                ViewIndex))
        {
            return STATUS_FILE_CORRUPT_ERROR;
        }
        if (Entry->Flags & INDEX_ENTRY_END)
        {
            if (Offset + Entry->EntryLength !=
                ListBytes)
            {
                return STATUS_FILE_CORRUPT_ERROR;
            }
            break;
        }
        PayloadBytes += Entry->EntryLength;
        Offset += Entry->EntryLength;
    }
    if (Offset >= ListBytes ||
        PayloadBytes == 0)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    Offset = 0;
    for (;;)
    {
        Entry =
            reinterpret_cast<PIndexEntry>(
                List + Offset);
        if (Entry->Flags & INDEX_ENTRY_END)
            return STATUS_FILE_CORRUPT_ERROR;
        Accumulated += Entry->EntryLength;
        if (Accumulated * 2 >= PayloadBytes)
        {
            *LeftBytes = Offset;
            *MedianOffset = Offset;
            *MedianLength = Entry->EntryLength;
            return STATUS_SUCCESS;
        }
        Offset += Entry->EntryLength;
    }
}

/*
 * Builds a complete INDX record image around one entry list. The list
 * either already carries its END entry, or a fresh END is appended for
 * the left half of a split, pointing at the median's former subtree.
 */
static NTSTATUS
BuildIndexBufferImage(
    _Out_writes_bytes_(RecordSize) PUCHAR Image,
    _In_ ULONG RecordSize,
    _In_ ULONG BytesPerSector,
    _In_ ULONGLONG Vcn,
    _In_ BOOLEAN Internal,
    _In_reads_bytes_(EntriesBytes)
        const UCHAR* Entries,
    _In_ ULONG EntriesBytes,
    _In_ BOOLEAN BuildEnd,
    _In_ ULONGLONG EndChildVcn)
{
    PIndexBuffer Buffer =
        reinterpret_cast<PIndexBuffer>(Image);
    PIndexEntry EndEntry;
    PUSHORT UpdateSequence;
    ULONG UpdateSequenceCount;
    ULONG EntriesOffset;
    ULONG EndLength;
    ULONG Total;

    if (RecordSize < BytesPerSector ||
        RecordSize % BytesPerSector != 0)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    UpdateSequenceCount =
        RecordSize / BytesPerSector + 1;

    RtlZeroMemory(Image, RecordSize);
    RtlCopyMemory(Buffer->RecordHeader.TypeID,
                  "INDX",
                  4);
    Buffer->RecordHeader.UpdateSequenceOffset =
        (USHORT)(
            FIELD_OFFSET(IndexBuffer,
                         IndexHeader) +
            sizeof(IndexNodeHeader));
    Buffer->RecordHeader.SizeOfUpdateSequence =
        (USHORT)UpdateSequenceCount;
    Buffer->VCN = Vcn;
    UpdateSequence =
        reinterpret_cast<PUSHORT>(
            Image +
            Buffer->
                RecordHeader.
                    UpdateSequenceOffset);
    UpdateSequence[0] = 1;

    EntriesOffset = ALIGN_UP_BY(
        sizeof(IndexNodeHeader) +
            UpdateSequenceCount *
                sizeof(USHORT),
        sizeof(ULONGLONG));
    EndLength = BuildEnd
        ? FIELD_OFFSET(IndexEntry,
                       IndexStream) +
            (Internal
                ? sizeof(ULONGLONG)
                : 0)
        : 0;
    Total = EntriesOffset + EntriesBytes +
        EndLength;
    if (RecordSize <
            FIELD_OFFSET(IndexBuffer,
                         IndexHeader) ||
        Total >
            RecordSize -
            FIELD_OFFSET(IndexBuffer,
                         IndexHeader))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    Buffer->IndexHeader.IndexOffset =
        EntriesOffset;
    Buffer->IndexHeader.TotalIndexSize = Total;
    Buffer->IndexHeader.AllocatedSize =
        RecordSize -
        FIELD_OFFSET(IndexBuffer, IndexHeader);
    Buffer->IndexHeader.Flags =
        Internal ? NTFS_INDEX_HEADER_LARGE : 0;

    RtlCopyMemory(
        reinterpret_cast<PUCHAR>(
            &Buffer->IndexHeader) +
            EntriesOffset,
        Entries,
        EntriesBytes);
    if (BuildEnd)
    {
        EndEntry =
            reinterpret_cast<PIndexEntry>(
                reinterpret_cast<PUCHAR>(
                    &Buffer->IndexHeader) +
                EntriesOffset + EntriesBytes);
        EndEntry->EntryLength =
            (USHORT)EndLength;
        EndEntry->Flags = INDEX_ENTRY_END |
            (Internal ? INDEX_ENTRY_NODE : 0);
        if (Internal)
        {
            *GetSubnodeVCN(EndEntry) =
                EndChildVcn;
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
WriteIndexNode(
    _In_ PVolume DiskVolume,
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONGLONG Vcn,
    _In_ ULONGLONG AllocationUnit,
    _In_ ULONG RecordSize,
    _Inout_updates_bytes_(RecordSize)
        PUCHAR Image)
{
    LARGE_INTEGER WriteOffset;
    ULONGLONG AllocationOffset;
    ULONGLONG NodeDiskOffset = 0;
    ULONG NodeLength = 0;
    ULONG WriteLength = RecordSize;
    NTSTATUS Status;
    BOOLEAN BarrierAttempted = FALSE;

    if (Vcn > ~(ULONGLONG)0 / AllocationUnit)
        return STATUS_FILE_CORRUPT_ERROR;
    AllocationOffset = Vcn * AllocationUnit;
    if (AllocationOffset >
        0x7fffffffffffffffULL - RecordSize)
    {
        return STATUS_FILE_TOO_LARGE;
    }

    Status = NtfsCommitFixup(
        reinterpret_cast<PNTFSRecordHeader>(
            Image),
        RecordSize,
        DiskVolume->BytesPerSector);
    if (!NT_SUCCESS(Status))
        return Status;

    /*
     * Directory name insert, barrier mode: this index-allocation node names a
     * file record added in the same operation (IndexBarrierRecordOffset, set
     * by AddFileToDirectory). Order the node after that record so the node
     * cannot reach the device before the record is durable; otherwise a crash
     * leaves a dangling index entry to a zeroed record (the J32 grow/
     * residual the record-before-bitmap fix did not cover).
     *
     * The constraint is installed BEFORE the write below dirties the node's
     * block. A block that is dirty and unconstrained, even for the moment
     * between the write and a constraint added afterwards, may be taken by
     * the lazy writer, and nothing can recall a write already on its way to
     * the device. The node's disk offset comes from the $INDEX_ALLOCATION
     * run list exactly as the bitmap barrier does. When the ordering cannot
     * be installed (no allocation attribute, the node crosses a run boundary
     * or sits in a sparse run, its block is not cached, or the barrier
     * refuses it), the record is committed synchronously first and only then
     * is the node written. The write never proceeds unordered, and no
     * failure on this path is ignored.
     */
    if (DiskVolume->IndexBarrierRecordOffset != 0)
    {
        PAttribute IndexAllocationAttribute =
            DirectoryFile->GetAttribute(
                TypeIndexAllocation,
                const_cast<PWSTR>(IndexName));

        if (IndexAllocationAttribute)
        {
            Status = DiskVolume->MFT->GetAttributeRangeDiskOffset(
                DirectoryFile, IndexAllocationAttribute,
                AllocationOffset, RecordSize,
                &NodeDiskOffset, &NodeLength);
        }
        else
        {
            Status = STATUS_NOT_FOUND;
        }
        if (NT_SUCCESS(Status))
        {
            BarrierAttempted = TRUE;
            Status = DiskVolume->BarrierAfter(
                NodeDiskOffset, NodeLength,
                DiskVolume->IndexBarrierRecordOffset,
                DiskVolume->MFT->FileRecordSize);
        }
        if (!NT_SUCCESS(Status))
        {
            /* Record first, synchronously; then the node may go anywhere. */
            Status = DiskVolume->CommitRange(
                DiskVolume->IndexBarrierRecordOffset,
                DiskVolume->MFT->FileRecordSize);
            if (Status == STATUS_NOT_IMPLEMENTED)
                Status = DiskVolume->FlushVolume();
            if (BarrierAttempted)
            {
                DiskVolume->BarrierCancel(NodeDiskOffset, NodeLength);
                BarrierAttempted = FALSE;
            }
            if (!NT_SUCCESS(Status))
                return Status;
        }
    }

    WriteOffset.QuadPart =
        (LONGLONG)AllocationOffset;
    Status = DirectoryFile->WriteFileData(
        TypeIndexAllocation,
        const_cast<PWSTR>(IndexName),
        Image,
        &WriteLength,
        &WriteOffset);
    if (NT_SUCCESS(Status) &&
        WriteLength != RecordSize)
    {
        Status = STATUS_END_OF_FILE;
    }
    /* A failed write leaves the block clean; release the constraint it was
     * installed for. A block the write did dirty keeps it. */
    if (!NT_SUCCESS(Status) && BarrierAttempted)
        DiskVolume->BarrierCancel(NodeDiskOffset, NodeLength);
    return Status;
}

/*
 * Pointer-after-pointee ordering for the index tree. The kernel environment
 * holds every write of one block or less back in its cache and commits the
 * dirty blocks in slot, eviction or range order, so the order the functions
 * below issue their writes in (child node, bitmap bit, then the parent's
 * pointer) says nothing about the order the blocks reach the disk. A cut can
 * then leave a root or a parent node pointing at an INDX block that never
 * landed, with its $I30 bit set (J36 r149: root -> VCN 5, 21 of 23 blocks
 * never written, 174 of 200 names unreachable). Each helper resolves the
 * child node's disk range from the $INDEX_ALLOCATION run list and defers to
 * Volume::OrderAfter; a node whose range cannot be resolved (sparse, or
 * crossing a run boundary) is made durable by a full flush, so nothing is
 * ordered silently.
 */
static NTSTATUS
ResolveIndexNodeRange(
    _In_ PVolume DiskVolume,
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONGLONG Vcn,
    _In_ ULONGLONG AllocationUnit,
    _In_ ULONG RecordSize,
    _Out_ PULONGLONG DiskOffset,
    _Out_ PULONG Length)
{
    PAttribute IndexAllocationAttribute;

    *DiskOffset = 0;
    *Length = 0;
    if (AllocationUnit == 0 ||
        Vcn > ~(ULONGLONG)0 / AllocationUnit)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    IndexAllocationAttribute =
        DirectoryFile->GetAttribute(
            TypeIndexAllocation,
            const_cast<PWSTR>(IndexName));
    if (!IndexAllocationAttribute)
        return STATUS_NOT_FOUND;
    return DiskVolume->MFT->GetAttributeRangeDiskOffset(
        DirectoryFile,
        IndexAllocationAttribute,
        Vcn * AllocationUnit,
        RecordSize,
        DiskOffset,
        Length);
}

/*
 * Orders the pointer range after each node in Vcns. PointerLength 0 commits
 * the nodes synchronously (the pointer is unknown, is a block the cache does
 * not hold yet, or is spread over several blocks).
 */
static NTSTATUS
OrderAfterIndexNodes(
    _In_ PVolume DiskVolume,
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_reads_(Count) const ULONGLONG* Vcns,
    _In_ ULONG Count,
    _In_ ULONGLONG AllocationUnit,
    _In_ ULONG RecordSize,
    _In_ ULONGLONG PointerDiskOffset,
    _In_ ULONG PointerLength)
{
    NTSTATUS Status = STATUS_SUCCESS;

    for (ULONG Index = 0; Index < Count; Index++)
    {
        ULONGLONG NodeDiskOffset;
        ULONG NodeLength;

        Status = ResolveIndexNodeRange(
            DiskVolume,
            DirectoryFile,
            IndexName,
            Vcns[Index],
            AllocationUnit,
            RecordSize,
            &NodeDiskOffset,
            &NodeLength);
        if (!NT_SUCCESS(Status))
        {
            /* Unresolvable: everything held back goes out, nodes included. */
            return DiskVolume->FlushVolume();
        }
        Status = DiskVolume->OrderAfter(
            PointerDiskOffset,
            PointerLength,
            NodeDiskOffset,
            NodeLength);
        if (!NT_SUCCESS(Status))
            break;
    }
    return Status;
}

/* The pointer is a file record (the directory's, carrying $INDEX_ROOT and
 * $BITMAP, or whichever record owns the root). NULL or an unresolvable
 * record commits the nodes synchronously. */
static NTSTATUS
OrderRecordAfterIndexNodes(
    _In_ PVolume DiskVolume,
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_reads_(Count) const ULONGLONG* Vcns,
    _In_ ULONG Count,
    _In_ ULONGLONG AllocationUnit,
    _In_ ULONG RecordSize,
    _In_opt_ PFileRecord PointerRecord)
{
    ULONGLONG RecordDiskOffset = 0;
    ULONG PointerLength = 0;

    if (PointerRecord &&
        PointerRecord->RecordNumber != MAXULONG &&
        NT_SUCCESS(DiskVolume->MFT->GetFileRecordDiskOffset(
            PointerRecord->RecordNumber,
            &RecordDiskOffset)))
    {
        PointerLength = DiskVolume->MFT->FileRecordSize;
    }
    return OrderAfterIndexNodes(
        DiskVolume,
        DirectoryFile,
        IndexName,
        Vcns,
        Count,
        AllocationUnit,
        RecordSize,
        RecordDiskOffset,
        PointerLength);
}

/* The pointer is the index node at NodeVcn (a parent about to be rewritten
 * with a link to the children in Vcns). */
static NTSTATUS
OrderNodeAfterIndexNodes(
    _In_ PVolume DiskVolume,
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONGLONG NodeVcn,
    _In_reads_(Count) const ULONGLONG* Vcns,
    _In_ ULONG Count,
    _In_ ULONGLONG AllocationUnit,
    _In_ ULONG RecordSize)
{
    ULONGLONG NodeDiskOffset = 0;
    ULONG NodeLength = 0;

    if (!NT_SUCCESS(ResolveIndexNodeRange(
            DiskVolume,
            DirectoryFile,
            IndexName,
            NodeVcn,
            AllocationUnit,
            RecordSize,
            &NodeDiskOffset,
            &NodeLength)))
    {
        NodeLength = 0;
    }
    return OrderAfterIndexNodes(
        DiskVolume,
        DirectoryFile,
        IndexName,
        Vcns,
        Count,
        AllocationUnit,
        RecordSize,
        NodeDiskOffset,
        NodeLength);
}

/*
 * The reverse pair: a node that references a VCN must not reach the disk
 * before the record whose $INDEX_ALLOCATION run list covers that VCN. The
 * growth of the allocation (ExtendNonResidentData, via WriteIndexNode of a
 * new block) rewrites the record, and the parent that points at the new
 * block is written next; committed out of order, the parent references a
 * VCN beyond the allocation on the disk (J36 shape scan type B: 1-28 record
 * 15668 references VCN 15-22 while only 0-14 exist). Orders the pointer
 * range after the record; a record whose offset cannot be resolved is made
 * durable by a full flush.
 */
static NTSTATUS
OrderPointerAfterRecord(
    _In_ PVolume DiskVolume,
    _In_ ULONGLONG PointerDiskOffset,
    _In_ ULONG PointerLength,
    _In_opt_ PFileRecord PointeeRecord)
{
    ULONGLONG RecordDiskOffset = 0;

    if (!PointeeRecord ||
        PointeeRecord->RecordNumber == MAXULONG ||
        !NT_SUCCESS(DiskVolume->MFT->GetFileRecordDiskOffset(
            PointeeRecord->RecordNumber,
            &RecordDiskOffset)))
    {
        return DiskVolume->FlushVolume();
    }
    return DiskVolume->OrderAfter(
        PointerDiskOffset,
        PointerLength,
        RecordDiskOffset,
        DiskVolume->MFT->FileRecordSize);
}

/* The pointer is the index node at NodeVcn. */
static NTSTATUS
OrderNodeAfterRecord(
    _In_ PVolume DiskVolume,
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONGLONG NodeVcn,
    _In_ ULONGLONG AllocationUnit,
    _In_ ULONG RecordSize,
    _In_opt_ PFileRecord PointeeRecord)
{
    ULONGLONG NodeDiskOffset = 0;
    ULONG NodeLength = 0;

    if (!NT_SUCCESS(ResolveIndexNodeRange(
            DiskVolume,
            DirectoryFile,
            IndexName,
            NodeVcn,
            AllocationUnit,
            RecordSize,
            &NodeDiskOffset,
            &NodeLength)))
    {
        NodeLength = 0;
    }
    return OrderPointerAfterRecord(
        DiskVolume,
        NodeDiskOffset,
        NodeLength,
        PointeeRecord);
}

/* The pointer is a record (the one owning $INDEX_ROOT); nothing to do when
 * it is the record owning the allocation, they are one write. */
static NTSTATUS
OrderRecordAfterRecord(
    _In_ PVolume DiskVolume,
    _In_opt_ PFileRecord PointerRecord,
    _In_opt_ PFileRecord PointeeRecord)
{
    ULONGLONG RecordDiskOffset = 0;
    ULONG PointerLength = 0;

    if (PointerRecord == PointeeRecord)
        return STATUS_SUCCESS;
    if (PointerRecord &&
        PointerRecord->RecordNumber != MAXULONG &&
        NT_SUCCESS(DiskVolume->MFT->GetFileRecordDiskOffset(
            PointerRecord->RecordNumber,
            &RecordDiskOffset)))
    {
        PointerLength = DiskVolume->MFT->FileRecordSize;
    }
    return OrderPointerAfterRecord(
        DiskVolume,
        RecordDiskOffset,
        PointerLength,
        PointeeRecord);
}

/* Current data size of the $INDEX_ALLOCATION, 0 when there is none. */
static ULONGLONG
IndexAllocationSize(
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName)
{
    PAttribute IndexAllocationAttribute =
        DirectoryFile->GetAttribute(
            TypeIndexAllocation,
            const_cast<PWSTR>(IndexName));

    return IndexAllocationAttribute
        ? GetAttributeDataSize(IndexAllocationAttribute)
        : 0;
}

/*
 * Freeing an index block. The write that drops the last pointer to the
 * block (a parent node, or the record holding $INDEX_ROOT) must be durable
 * before the block's $I30 bit is cleared, in both barrier modes. The bit
 * clears in memory at once, so the next split can reuse the block and write
 * a node of another subtree into it; were the old pointer still on the disk
 * when that content landed, a cut would leave a parent pointing into a
 * foreign subtree. No constraint can express "before the block is reused",
 * so this is the one ordering that stays a synchronous commit with the
 * barrier on. A block is freed only when a leaf empties, so the cost is
 * bounded. PointerRecord names the record that held the pointer; NULL means
 * the node at PointerVcn held it.
 */
static NTSTATUS
CommitIndexPointerHolder(
    _In_ PVolume DiskVolume,
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONGLONG AllocationUnit,
    _In_ ULONG RecordSize,
    _In_opt_ PFileRecord PointerRecord,
    _In_ ULONGLONG PointerVcn)
{
    if (PointerRecord)
    {
        return OrderPointerAfterRecord(
            DiskVolume,
            0,
            0,
            PointerRecord);
    }
    return OrderAfterIndexNodes(
        DiskVolume,
        DirectoryFile,
        IndexName,
        &PointerVcn,
        1,
        AllocationUnit,
        RecordSize,
        0,
        0);
}

/*
 * A node write that RemoveMaxFromSubtree leaves to its caller. The node
 * gives up its collation maximum, which the caller writes into an ancestor
 * as the replacement key; the node must not reach the disk before that
 * ancestor, or a cut leaves the key in no node. The caller orders and
 * writes the image, then releases ChainVcn (an END-only chain the node no
 * longer references) once that write is durable.
 */
typedef struct _DEFERRED_INDEX_WRITE
{
    PUCHAR Image;          /* caller-owned, RecordSize bytes */
    ULONGLONG Vcn;
    BOOLEAN Valid;
    ULONGLONG ChainVcn;
    BOOLEAN ChainValid;
} DEFERRED_INDEX_WRITE, *PDEFERRED_INDEX_WRITE;

/*
 * A failure after the entry is out of the index. The removal has happened,
 * so the caller must see success; an error here made every caller abort
 * half way (the name gone, the record and its DOS alias kept, the deleted
 * file back at the next check). What is left behind is a duplicated key or
 * blocks whose bits stayed set, both repaired by a check: keep the volume
 * dirty through a clean dismount so the next boot runs one, and name the
 * site in the debug log.
 */
static void
NoteIndexDamage(
    _In_ PVolume DiskVolume,
    _In_ const char* What,
    _In_ ULONGLONG Vcn,
    _In_ NTSTATUS Status)
{
    DPRINT1(
        "Index %s at VCN %llu failed after the entry "
        "was removed: 0x%lx; the volume stays dirty "
        "for a check.\n",
        What,
        Vcn,
        Status);
    /* The print is debug-only; the builds without it must not warn. */
    (void)What;
    (void)Vcn;
    (void)Status;
    DiskVolume->RetainDirty(TRUE);
}

/*
 * A left half that a split holds back. Right, parent, then left: the left
 * node may be rewritten with only its half of the entries once the node
 * that links the new right node is written, else a cut that lands the left
 * node first leaves the moved entries unreachable although every record
 * exists.
 */
typedef struct _DEFERRED_LEFT_NODE
{
    PUCHAR Image;       /* RecordSize bytes, the rewritten left half */
    ULONGLONG LeftVcn;
    ULONGLONG RightVcn; /* the new right sibling of that level */
} DEFERRED_LEFT_NODE, *PDEFERRED_LEFT_NODE;

/*
 * Writes the left halves a split held back, highest level first. Each is
 * ordered after its own right sibling (the moved entries must be on the
 * disk first) and after the nodes that may link that sibling: the left and
 * right nodes of the level above, or, for the highest level, the node or
 * record the split ended in (TopParentVcn when TopParentIsNode, else
 * TopParentRecord). Only then may a left half land, so a cut can duplicate
 * the moved entries but never lose them.
 */
static NTSTATUS
WriteDeferredLeftNodes(
    _In_ PVolume DiskVolume,
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONGLONG AllocationUnit,
    _In_ ULONG RecordSize,
    _In_reads_(Count) const DEFERRED_LEFT_NODE* Deferred,
    _In_ ULONG Count,
    _In_ BOOLEAN TopParentIsNode,
    _In_ ULONGLONG TopParentVcn,
    _In_opt_ PFileRecord TopParentRecord)
{
    NTSTATUS Status = STATUS_SUCCESS;

    for (ULONG Index = Count; Index-- > 0;)
    {
        ULONGLONG Pointees[3];
        ULONG PointeeCount = 0;

        Pointees[PointeeCount++] = Deferred[Index].RightVcn;
        if (Index + 1 < Count)
        {
            Pointees[PointeeCount++] = Deferred[Index + 1].LeftVcn;
            Pointees[PointeeCount++] = Deferred[Index + 1].RightVcn;
        }
        else if (TopParentIsNode)
        {
            Pointees[PointeeCount++] = TopParentVcn;
        }
        Status = OrderNodeAfterIndexNodes(
            DiskVolume,
            DirectoryFile,
            IndexName,
            Deferred[Index].LeftVcn,
            Pointees,
            PointeeCount,
            AllocationUnit,
            RecordSize);
        if (NT_SUCCESS(Status) &&
            Index + 1 == Count &&
            !TopParentIsNode &&
            TopParentRecord)
        {
            Status = OrderNodeAfterRecord(
                DiskVolume,
                DirectoryFile,
                IndexName,
                Deferred[Index].LeftVcn,
                AllocationUnit,
                RecordSize,
                TopParentRecord);
        }
        if (!NT_SUCCESS(Status))
            break;
        Status = WriteIndexNode(
            DiskVolume,
            DirectoryFile,
            IndexName,
            Deferred[Index].LeftVcn,
            AllocationUnit,
            RecordSize,
            Deferred[Index].Image);
        if (!NT_SUCCESS(Status))
            break;
    }
    return Status;
}

static NTSTATUS
SetIndexRecordBitmapBit(
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONGLONG RecordOrdinal,
    _In_ BOOLEAN Value)
{
    PAttribute BitmapAttribute;
    LARGE_INTEGER WriteOffset;
    ULONGLONG BitmapLength;
    ULONGLONG ByteOffset;
    ULONG BytesRemaining;
    ULONG WriteLength;
    UCHAR GrowthChunk[8] = {};
    UCHAR BitmapByte;
    UCHAR BitmapMask;
    NTSTATUS Status;

    ByteOffset = RecordOrdinal >> 3;
    BitmapMask =
        (UCHAR)(1u << (RecordOrdinal & 7));

    BitmapAttribute =
        DirectoryFile->GetAttribute(
            TypeBitmap,
            const_cast<PWSTR>(IndexName));
    if (!BitmapAttribute)
        return STATUS_FILE_CORRUPT_ERROR;
    BitmapLength =
        GetAttributeDataSize(BitmapAttribute);

    if (ByteOffset >= BitmapLength)
    {
        if (!Value)
            return STATUS_SUCCESS;
        if (ByteOffset - BitmapLength >=
            sizeof(GrowthChunk))
        {
            return STATUS_FILE_CORRUPT_ERROR;
        }
        WriteLength = sizeof(GrowthChunk);
        WriteOffset.QuadPart =
            (LONGLONG)BitmapLength;
        Status = DirectoryFile->WriteFileData(
            TypeBitmap,
            const_cast<PWSTR>(IndexName),
            GrowthChunk,
            &WriteLength,
            &WriteOffset);
        if (!NT_SUCCESS(Status))
            return Status;
        if (WriteLength != sizeof(GrowthChunk))
            return STATUS_END_OF_FILE;
        BitmapAttribute =
            DirectoryFile->GetAttribute(
                TypeBitmap,
                const_cast<PWSTR>(IndexName));
        if (!BitmapAttribute)
            return STATUS_FILE_CORRUPT_ERROR;
    }

    BytesRemaining = 1;
    Status = DirectoryFile->CopyData(
        BitmapAttribute,
        &BitmapByte,
        &BytesRemaining,
        ByteOffset);
    if (!NT_SUCCESS(Status))
        return Status;
    if (BytesRemaining != 0)
        return STATUS_END_OF_FILE;

    if (Value)
    {
        if (BitmapByte & BitmapMask)
            return STATUS_FILE_CORRUPT_ERROR;
        BitmapByte |= BitmapMask;
    }
    else
    {
        BitmapByte &= (UCHAR)~BitmapMask;
    }

    WriteLength = 1;
    WriteOffset.QuadPart = (LONGLONG)ByteOffset;
    Status = DirectoryFile->WriteFileData(
        TypeBitmap,
        const_cast<PWSTR>(IndexName),
        &BitmapByte,
        &WriteLength,
        &WriteOffset);
    if (NT_SUCCESS(Status) && WriteLength != 1)
        Status = STATUS_END_OF_FILE;
    return Status;
}

/*
 * Chooses the target position for a new index record: the first free
 * bitmap slot inside the current allocation, or the append position at
 * its end. Nothing is made durable here; the record becomes owned once
 * its image is written and its bitmap bit is set.
 */
static NTSTATUS
SelectIndexRecordSlot(
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONG RecordSize,
    _In_ ULONGLONG AllocationUnit,
    _Out_ PULONGLONG Vcn,
    _Out_ PULONGLONG RecordOrdinal)
{
    PAttribute IndexAllocationAttribute;
    PAttribute BitmapAttribute;
    UCHAR Chunk[64];
    ULONGLONG AllocationLength;
    ULONGLONG BitmapLength;
    ULONGLONG RecordCount;
    ULONGLONG Ordinal = 0;
    ULONGLONG ByteOffset = 0;
    NTSTATUS Status;

    *Vcn = 0;
    *RecordOrdinal = 0;

    IndexAllocationAttribute =
        DirectoryFile->GetAttribute(
            TypeIndexAllocation,
            const_cast<PWSTR>(IndexName));
    BitmapAttribute =
        DirectoryFile->GetAttribute(
            TypeBitmap,
            const_cast<PWSTR>(IndexName));
    if (!IndexAllocationAttribute ||
        !IndexAllocationAttribute->
            IsNonResident ||
        !BitmapAttribute)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    AllocationLength =
        GetAttributeDataSize(
            IndexAllocationAttribute);
    BitmapLength =
        GetAttributeDataSize(BitmapAttribute);
    if (AllocationLength % RecordSize != 0)
        return STATUS_FILE_CORRUPT_ERROR;
    RecordCount =
        AllocationLength / RecordSize;

    while (Ordinal < RecordCount &&
           ByteOffset < BitmapLength)
    {
        ULONG BytesRemaining =
            (ULONG)(
                BitmapLength - ByteOffset <
                    sizeof(Chunk)
                ? BitmapLength - ByteOffset
                : sizeof(Chunk));
        ULONG Read = BytesRemaining;

        Status = DirectoryFile->CopyData(
            BitmapAttribute,
            Chunk,
            &BytesRemaining,
            ByteOffset);
        if (!NT_SUCCESS(Status))
            return Status;
        if (BytesRemaining != 0)
            return STATUS_END_OF_FILE;

        for (ULONG Index = 0;
             Index < Read &&
                Ordinal < RecordCount;
             Index++)
        {
            for (ULONG Bit = 0;
                 Bit < 8 &&
                    Ordinal < RecordCount;
                 Bit++, Ordinal++)
            {
                if (!(Chunk[Index] &
                      (1u << Bit)))
                {
                    *RecordOrdinal = Ordinal;
                    *Vcn = Ordinal *
                        (RecordSize /
                         AllocationUnit);
                    return STATUS_SUCCESS;
                }
            }
        }
        ByteOffset += Read;
    }

    *RecordOrdinal = RecordCount;
    *Vcn = RecordCount *
        (RecordSize / AllocationUnit);
    return STATUS_SUCCESS;
}

/*
 * Replaces the complete $INDEX_ROOT value with backup/restore so a
 * failed record commit leaves the previous root intact.
 */
/*
 * Whether the root of a large index may grow to ValueLength bytes. While
 * the directory has no attribute list, the root must leave the base record
 * the room a mapping growth needs to create one: a root that fills the
 * record leaves the next index block with neither room for its mapping nor
 * for a list, and every insert fails STATUS_BUFFER_TOO_SMALL (Lab03 L1-B,
 * about 1400 entries with 200-character names). When it returns FALSE the
 * caller pushes the root down, as for a full record. Once a list exists,
 * a mapping that does not fit moves to an extension record instead.
 */
BOOLEAN
Directory::RootGrowthKeepsListRoom(
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONG ValueLength)
{
    PFileRecordHeader Header = DirectoryFile->Header;
    PAttribute Root;
    PAttribute Allocation;
    ULONG OldLength;
    ULONG NewLength;

    if (!Header || !DirectoryFile->Data || Header->BaseFileRecord != 0 ||
        Header->ActualSize > Header->AllocatedSize ||
        DirectoryFile->FindAttributeInRecord(TypeAttributeList, NULL, NULL))
    {
        return TRUE;
    }
    Root = DirectoryFile->FindAttributeInRecord(TypeIndexRoot, const_cast<PWSTR>(IndexName), NULL);
    Allocation = DirectoryFile->FindAttributeInRecord(TypeIndexAllocation, const_cast<PWSTR>(IndexName), NULL);
    if (!Root || Root->IsNonResident || !Allocation || !Allocation->IsNonResident ||
        ValueLength > MAXULONG - 8 - Root->Resident.DataOffset)
    {
        return TRUE;
    }
    OldLength = Root->Length;
    NewLength = ALIGN_UP_BY(Root->Resident.DataOffset + ValueLength, sizeof(ULONGLONG));
    if (NewLength <= OldLength)
        return TRUE;
    return Header->AllocatedSize - Header->ActualSize >= NewLength - OldLength + NTFS_MAPPING_GROWTH_HEADROOM;
}

NTSTATUS
Directory::ReplaceIndexRootValue(
    _In_ PVolume DiskVolume,
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_reads_bytes_(ValueLength) PUCHAR Value,
    _In_ ULONG ValueLength)
{
    PAttribute IndexRootAttribute;
    PFileRecord IndexRootOwner;
    PUCHAR RecordBackup;
    NTSTATUS Status;

    IndexRootAttribute =
        DirectoryFile->GetAttribute(
            TypeIndexRoot,
            const_cast<PWSTR>(IndexName));
    if (!IndexRootAttribute ||
        IndexRootAttribute->IsNonResident)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    IndexRootOwner =
        DirectoryFile->GetAttributeOwner(
            IndexRootAttribute);
    if (!IndexRootOwner)
        return STATUS_FILE_CORRUPT_ERROR;

    RecordBackup =
        new(PagedPool, TAG_FILE_RECORD)
            UCHAR[IndexRootOwner->
                RecordBufferSize];
    if (!RecordBackup)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(RecordBackup,
                  IndexRootOwner->Data,
                  IndexRootOwner->
                      RecordBufferSize);

    Status = IndexRootOwner->
        ReplaceResidentData(
            IndexRootAttribute,
            Value,
            ValueLength);
    if (NT_SUCCESS(Status))
    {
        Status = DiskVolume->MFT->
            WriteFileRecordToMFT(
                IndexRootOwner);
    }
    if (!NT_SUCCESS(Status))
    {
        RtlCopyMemory(
            IndexRootOwner->Data,
            RecordBackup,
            IndexRootOwner->RecordBufferSize);
        IndexRootOwner->Header =
            reinterpret_cast<PFileRecordHeader>(
                IndexRootOwner->Data);
        IndexRootOwner->ClearDataRunCache();
    }
    delete[] RecordBackup;
    return Status;
}

static NTSTATUS
BuildRootValueFromList(
    _In_ const IndexRootEx* OldRoot,
    _In_reads_bytes_(ListBytes)
        const UCHAR* List,
    _In_ ULONG ListBytes,
    _In_ BOOLEAN Large,
    _Outptr_result_bytebuffer_(*ValueLength)
        PUCHAR* Value,
    _Out_ PULONG ValueLength)
{
    const ULONG RootPrefix =
        FIELD_OFFSET(IndexRootEx, Header);
    PIndexRootEx NewRoot;
    ULONG Total;

    *Value = NULL;
    *ValueLength = 0;
    if (ListBytes >
        MAXULONG - RootPrefix -
            sizeof(IndexNodeHeader))
    {
        return STATUS_FILE_TOO_LARGE;
    }
    Total = RootPrefix +
        sizeof(IndexNodeHeader) + ListBytes;

    *Value =
        new(PagedPool, TAG_BTREE) UCHAR[Total];
    if (!*Value)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(*Value, Total);
    NewRoot =
        reinterpret_cast<PIndexRootEx>(*Value);
    RtlCopyMemory(NewRoot,
                  OldRoot,
                  RootPrefix);
    NewRoot->Header.IndexOffset =
        sizeof(IndexNodeHeader);
    NewRoot->Header.TotalIndexSize =
        sizeof(IndexNodeHeader) + ListBytes;
    NewRoot->Header.AllocatedSize =
        NewRoot->Header.TotalIndexSize;
    NewRoot->Header.Flags =
        Large ? NTFS_INDEX_HEADER_LARGE : 0;
    RtlCopyMemory(
        reinterpret_cast<PUCHAR>(
            &NewRoot->Header) +
            sizeof(IndexNodeHeader),
        List,
        ListBytes);
    *ValueLength = Total;
    return STATUS_SUCCESS;
}

NTSTATUS
Directory::ResetIndex(
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName)
{
    PAttribute IndexRootAttribute;
    PAttribute BitmapAttribute;
    IndexRootEx RootPrefix;
    UCHAR EndList[FIELD_OFFSET(IndexEntry, IndexStream)] = {};
    PIndexEntry End = reinterpret_cast<PIndexEntry>(EndList);
    PUCHAR RootValue = NULL;
    ULONG RootValueLength;
    NTSTATUS Status;

    IndexRootAttribute =
        DirectoryFile->GetAttribute(
            TypeIndexRoot,
            const_cast<PWSTR>(IndexName));
    if (!IndexRootAttribute ||
        IndexRootAttribute->IsNonResident ||
        IndexRootAttribute->Resident.DataLength < sizeof(IndexRootEx))
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    RtlCopyMemory(&RootPrefix,
                  reinterpret_cast<PUCHAR>(IndexRootAttribute) +
                      IndexRootAttribute->Resident.DataOffset,
                  sizeof(RootPrefix));

    /* Unlink every node first, then free the blocks: a cut in between leaves
     * blocks in use that nothing links, which the check releases, and never a
     * root that links a free block. */
    End->EntryLength = (USHORT)sizeof(EndList);
    End->Flags = INDEX_ENTRY_END;
    Status = BuildRootValueFromList(
        &RootPrefix,
        EndList,
        sizeof(EndList),
        FALSE,
        &RootValue,
        &RootValueLength);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = ReplaceIndexRootValue(
        DiskVolume,
        DirectoryFile,
        IndexName,
        RootValue,
        RootValueLength);
    delete[] RootValue;
    if (!NT_SUCCESS(Status))
        return Status;

    BitmapAttribute =
        DirectoryFile->GetAttribute(
            TypeBitmap,
            const_cast<PWSTR>(IndexName));
    if (BitmapAttribute)
    {
        ULONGLONG BitmapLength =
            GetAttributeDataSize(BitmapAttribute);
        ULONG WriteLength = (ULONG)BitmapLength;
        LARGE_INTEGER WriteOffset = {};
        PUCHAR Zeros;

        if (BitmapLength > MAXULONG)
            return STATUS_FILE_CORRUPT_ERROR;
        if (WriteLength)
        {
            Zeros = new(PagedPool, TAG_BTREE) UCHAR[WriteLength];
            if (!Zeros)
                return STATUS_INSUFFICIENT_RESOURCES;
            RtlZeroMemory(Zeros, WriteLength);
            Status = DirectoryFile->WriteFileData(
                TypeBitmap,
                const_cast<PWSTR>(IndexName),
                Zeros,
                &WriteLength,
                &WriteOffset);
            delete[] Zeros;
            if (NT_SUCCESS(Status) && WriteLength != (ULONG)BitmapLength)
                Status = STATUS_END_OF_FILE;
            if (!NT_SUCCESS(Status))
                return Status;
        }

        /* A resident bitmap lives in a record: commit it. */
        BitmapAttribute =
            DirectoryFile->GetAttribute(
                TypeBitmap,
                const_cast<PWSTR>(IndexName));
        if (BitmapAttribute && !BitmapAttribute->IsNonResident)
        {
            PFileRecord BitmapOwner =
                DirectoryFile->GetAttributeOwner(BitmapAttribute);

            if (!BitmapOwner)
                return STATUS_FILE_CORRUPT_ERROR;
            Status = DiskVolume->MFT->WriteFileRecordToMFT(BitmapOwner);
        }
    }
    return Status;
}

/*
 * Moves one complete root entry list into an index buffer and leaves the
 * root as a single END entry pointing at it. This serves both the first
 * promotion of a resident-only directory and later root overflows of an
 * already-large index. The buffer content becomes durable before the
 * root switch commits the new shape.
 */
NTSTATUS
Directory::PushDownRoot(
    _In_ PVolume DiskVolume,
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ const IndexRootEx* OldRoot,
    _In_reads_bytes_(ListBytes) PUCHAR List,
    _In_ ULONG ListBytes,
    _In_ BOOLEAN ListInternal,
    _In_ ULONG RecordSize,
    _In_ ULONGLONG AllocationUnit)
{
    PAttribute IndexAllocationAttribute;
    PAttribute BitmapAttribute;
    PIndexEntry RootEnd;
    PUCHAR Image = NULL;
    PUCHAR RootValue = NULL;
    UCHAR RootEndList[
        FIELD_OFFSET(IndexEntry, IndexStream) +
        sizeof(ULONGLONG)] = {};
    IndexRootEx RootPrefix;
    ULONGLONG Vcn = 0;
    ULONGLONG RecordOrdinal = 0;
    ULONG RootValueLength;
    NTSTATUS Status;

    /*
     * The record mutations below may relocate attribute values; keep the
     * fixed $INDEX_ROOT prefix in a local copy.
     */
    RtlCopyMemory(&RootPrefix,
                  OldRoot,
                  sizeof(RootPrefix));

    Image =
        new(PagedPool, TAG_BTREE)
            UCHAR[RecordSize];
    if (!Image)
        return STATUS_INSUFFICIENT_RESOURCES;

    IndexAllocationAttribute =
        DirectoryFile->GetAttribute(
            TypeIndexAllocation,
            const_cast<PWSTR>(IndexName));
    BitmapAttribute =
        DirectoryFile->GetAttribute(
            TypeBitmap,
            const_cast<PWSTR>(IndexName));
    if (!IndexAllocationAttribute !=
        !BitmapAttribute)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Done;
    }

    if (!IndexAllocationAttribute)
    {
        /*
         * First promotion. The full record has no room for the two new
         * attribute headers while the old root list is still resident,
         * so every edit stays in memory in this order: shrink the root
         * to its END-only form, add $BITMAP with the first bit set, add
         * the $INDEX_ALLOCATION stub, and let the stub's promotion write
         * the already-durable buffer image and commit the record once.
         */
        PAttribute IndexRootAttribute;
        PAttribute NewAttribute;
        PFileRecord RootOwner;
        PUCHAR BaseBackup = NULL;
        PUCHAR OwnerBackup = NULL;
        UCHAR InitialBitmap[8] = { 1 };

        Status = BuildIndexBufferImage(
            Image,
            RecordSize,
            DiskVolume->BytesPerSector,
            0,
            ListInternal,
            List,
            ListBytes,
            FALSE,
            0);
        if (!NT_SUCCESS(Status))
            goto Done;
        Status = NtfsCommitFixup(
            reinterpret_cast<PNTFSRecordHeader>(
                Image),
            RecordSize,
            DiskVolume->BytesPerSector);
        if (!NT_SUCCESS(Status))
            goto Done;

        RootEnd =
            reinterpret_cast<PIndexEntry>(
                RootEndList);
        RootEnd->EntryLength =
            (USHORT)sizeof(RootEndList);
        RootEnd->Flags =
            INDEX_ENTRY_END | INDEX_ENTRY_NODE;
        *GetSubnodeVCN(RootEnd) = 0;
        Status = BuildRootValueFromList(
            &RootPrefix,
            RootEndList,
            sizeof(RootEndList),
            TRUE,
            &RootValue,
            &RootValueLength);
        if (!NT_SUCCESS(Status))
            goto Done;

        IndexRootAttribute =
            DirectoryFile->GetAttribute(
                TypeIndexRoot,
                const_cast<PWSTR>(IndexName));
        if (!IndexRootAttribute ||
            IndexRootAttribute->IsNonResident)
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            goto Done;
        }
        RootOwner =
            DirectoryFile->GetAttributeOwner(
                IndexRootAttribute);
        if (!RootOwner)
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            goto Done;
        }

        BaseBackup =
            new(PagedPool, TAG_FILE_RECORD)
                UCHAR[DirectoryFile->
                    RecordBufferSize];
        if (!BaseBackup)
        {
            Status =
                STATUS_INSUFFICIENT_RESOURCES;
            goto Done;
        }
        RtlCopyMemory(
            BaseBackup,
            DirectoryFile->Data,
            DirectoryFile->RecordBufferSize);
        if (RootOwner != DirectoryFile)
        {
            OwnerBackup =
                new(PagedPool, TAG_FILE_RECORD)
                    UCHAR[RootOwner->
                        RecordBufferSize];
            if (!OwnerBackup)
            {
                delete[] BaseBackup;
                Status =
                    STATUS_INSUFFICIENT_RESOURCES;
                goto Done;
            }
            RtlCopyMemory(
                OwnerBackup,
                RootOwner->Data,
                RootOwner->RecordBufferSize);
        }

        Status = RootOwner->ReplaceResidentData(
            IndexRootAttribute,
            RootValue,
            RootValueLength);
        if (NT_SUCCESS(Status))
        {
            Status =
                DirectoryFile->
                    InsertResidentAttribute(
                        TypeBitmap,
                        const_cast<PWSTR>(
                            IndexName),
                        &NewAttribute);
        }
        if (NT_SUCCESS(Status))
        {
            Status =
                DirectoryFile->
                    ReplaceResidentData(
                        NewAttribute,
                        InitialBitmap,
                        sizeof(InitialBitmap));
        }
        if (NT_SUCCESS(Status))
        {
            Status =
                DirectoryFile->
                    InsertResidentAttribute(
                        TypeIndexAllocation,
                        const_cast<PWSTR>(
                            IndexName),
                        &NewAttribute);
        }
        if (NT_SUCCESS(Status))
        {
            Status =
                DirectoryFile->
                    PromoteResidentData(
                        NewAttribute,
                        Image,
                        RecordSize,
                        0,
                        RecordSize,
                        RecordSize);
            if (Status == STATUS_BUFFER_TOO_SMALL)
            {
                PFileRecord AllocationOwner = DirectoryFile;
                BOOLEAN ListCreated;

                Status =
                    DirectoryFile->
                        EnsureAttributeListForMappingGrowth(
                            TypeIndexAllocation,
                            const_cast<PWSTR>(
                                IndexName),
                            &NewAttribute,
                            &AllocationOwner,
                            &ListCreated);
                if (NT_SUCCESS(Status) && ListCreated)
                {
                    Status =
                        AllocationOwner->
                            PromoteResidentData(
                                NewAttribute,
                                Image,
                                RecordSize,
                                0,
                                RecordSize,
                                RecordSize);
                    if (NT_SUCCESS(Status) &&
                        AllocationOwner != DirectoryFile)
                    {
                        Status = DiskVolume->MFT->
                            WriteFileRecordToMFT(
                                AllocationOwner);
                    }
                }
                else if (NT_SUCCESS(Status))
                {
                    Status = STATUS_BUFFER_TOO_SMALL;
                }
            }
        }
        if (NT_SUCCESS(Status) &&
            RootOwner != DirectoryFile)
        {
            Status = DiskVolume->MFT->
                WriteFileRecordToMFT(RootOwner);
        }
        /*
         * The attribute list created above for the allocation's mapping
         * is only needed if the mapping went to an extension record. When
         * it still fit in the base record, every entry names the base
         * record and no later path frees an extension record to notice:
         * remove it now (base record first, then the list's clusters).
         */
        if (NT_SUCCESS(Status))
            (void)DirectoryFile->SettleGrowthAttributeList();
        if (!NT_SUCCESS(Status))
        {
            RtlCopyMemory(
                DirectoryFile->Data,
                BaseBackup,
                DirectoryFile->
                    RecordBufferSize);
            DirectoryFile->Header =
                reinterpret_cast<
                    PFileRecordHeader>(
                        DirectoryFile->Data);
            DirectoryFile->ClearDataRunCache();
            if (OwnerBackup)
            {
                RtlCopyMemory(
                    RootOwner->Data,
                    OwnerBackup,
                    RootOwner->RecordBufferSize);
                RootOwner->Header =
                    reinterpret_cast<
                        PFileRecordHeader>(
                            RootOwner->Data);
                RootOwner->ClearDataRunCache();
            }
        }
        delete[] OwnerBackup;
        delete[] BaseBackup;
        goto Done;
    }
    else
    {
        Status = SelectIndexRecordSlot(
            DirectoryFile,
            IndexName,
            RecordSize,
            AllocationUnit,
            &Vcn,
            &RecordOrdinal);
        if (!NT_SUCCESS(Status))
            goto Done;
        Status = BuildIndexBufferImage(
            Image,
            RecordSize,
            DiskVolume->BytesPerSector,
            Vcn,
            ListInternal,
            List,
            ListBytes,
            FALSE,
            0);
        if (!NT_SUCCESS(Status))
            goto Done;
        Status = WriteIndexNode(
            DiskVolume,
            DirectoryFile,
            IndexName,
            Vcn,
            AllocationUnit,
            RecordSize,
            Image);
        if (!NT_SUCCESS(Status))
            goto Done;

        /*
         * The directory record is about to name this node: the $BITMAP bit
         * below, then the root's subnode pointer. Order the record after the
         * node so a cut cannot leave root -> Vcn over a block that never
         * reached the disk. The root may live in another record than the
         * bitmap; order that one too.
         */
        {
            PAttribute IndexRootAttribute =
                DirectoryFile->GetAttribute(
                    TypeIndexRoot,
                    const_cast<PWSTR>(IndexName));
            PFileRecord RootOwner = IndexRootAttribute
                ? DirectoryFile->GetAttributeOwner(
                    IndexRootAttribute)
                : NULL;
            PAttribute AllocationAttribute =
                DirectoryFile->GetAttribute(
                    TypeIndexAllocation,
                    const_cast<PWSTR>(IndexName));
            PFileRecord AllocationOwner = AllocationAttribute
                ? DirectoryFile->GetAttributeOwner(
                    AllocationAttribute)
                : NULL;

            Status = OrderRecordAfterIndexNodes(
                DiskVolume,
                DirectoryFile,
                IndexName,
                &Vcn,
                1,
                AllocationUnit,
                RecordSize,
                DirectoryFile);
            if (NT_SUCCESS(Status) &&
                RootOwner &&
                RootOwner != DirectoryFile)
            {
                Status = OrderRecordAfterIndexNodes(
                    DiskVolume,
                    DirectoryFile,
                    IndexName,
                    &Vcn,
                    1,
                    AllocationUnit,
                    RecordSize,
                    RootOwner);
            }
            /*
             * The END -> Vcn root must not land before the run list that
             * covers Vcn when the two live in different records (the node
             * write above may have grown the allocation).
             */
            if (NT_SUCCESS(Status))
            {
                Status = OrderRecordAfterRecord(
                    DiskVolume,
                    RootOwner,
                    AllocationOwner);
            }
            if (!NT_SUCCESS(Status))
                goto Done;
        }

        Status = SetIndexRecordBitmapBit(
            DirectoryFile,
            IndexName,
            RecordOrdinal,
            TRUE);
        if (!NT_SUCCESS(Status))
            goto Done;
    }

    RootEnd =
        reinterpret_cast<PIndexEntry>(
            RootEndList);
    RootEnd->EntryLength =
        (USHORT)sizeof(RootEndList);
    RootEnd->Flags =
        INDEX_ENTRY_END | INDEX_ENTRY_NODE;
    *GetSubnodeVCN(RootEnd) = Vcn;

    Status = BuildRootValueFromList(
        &RootPrefix,
        RootEndList,
        sizeof(RootEndList),
        TRUE,
        &RootValue,
        &RootValueLength);
    if (!NT_SUCCESS(Status))
        goto Done;
    Status = ReplaceIndexRootValue(
        DiskVolume,
        DirectoryFile,
        IndexName,
        RootValue,
        RootValueLength);
    if (!NT_SUCCESS(Status))
    {
        /*
         * A failed root switch is no proof that the old root is what the
         * disk holds: ReplaceIndexRootValue restores only the in-memory
         * record, and the record write it attempted may have reached the
         * write-back cache or the disk in part. Keep the buffer allocated
         * (its $I30 bit stays set). If the old root stands, the buffer is a
         * block nothing names, which the check releases; if the new root
         * landed, the buffer it names stays valid. Releasing it here would
         * let the next split write another subtree into a block a durable
         * root may name.
         */
        DPRINT1(
            "Root switch failed (0x%lx): index "
            "record %llu stays allocated.\n",
            Status,
            RecordOrdinal);
    }

Done:
    if (NT_SUCCESS(Status))
    {
        /*
         * The root's pointer set changed wholesale. The record committed
         * before the push-down (for the pushed-down node's new VCNs)
         * carries the OLD root; until the new one lands, a cut shows that
         * old root over leaves that later splits rewrite as half-empty,
         * and everything under the new node is unreachable (Lab04: 65 of
         * 200 records reachable, in every directory). Commit the record
         * now, in both barrier modes: one write per directory lifetime,
         * and the old root is never on the disk after this returns. The
         * root may live in another record than the bitmap and run list,
         * so both are committed.
         */
        PAttribute RootAttribute =
            DirectoryFile->GetAttribute(
                TypeIndexRoot,
                const_cast<PWSTR>(IndexName));
        PFileRecord RootOwner = RootAttribute
            ? DirectoryFile->GetAttributeOwner(RootAttribute)
            : NULL;

        Status = OrderPointerAfterRecord(
            DiskVolume,
            0,
            0,
            DirectoryFile);
        if (NT_SUCCESS(Status) &&
            RootOwner &&
            RootOwner != DirectoryFile)
        {
            Status = OrderPointerAfterRecord(
                DiskVolume,
                0,
                0,
                RootOwner);
        }
    }
    delete[] RootValue;
    delete[] Image;
    return Status;
}

/*
 * Turns the median of a split into the entry promoted to the parent:
 * the median's key travels upward while its child slot points at the
 * left node that now holds every key collating before it.
 */
static NTSTATUS
BuildPromotedEntry(
    _In_reads_bytes_(MedianLength)
        const UCHAR* Median,
    _In_ ULONG MedianLength,
    _In_ BOOLEAN Internal,
    _In_ ULONGLONG LeftVcn,
    _Outptr_result_bytebuffer_(*PromotedLength)
        PIndexEntry* Promoted,
    _Out_ PULONG PromotedLength)
{
    PIndexEntry Entry;
    ULONG Length;

    *Promoted = NULL;
    *PromotedLength = 0;

    Length = Internal
        ? MedianLength
        : MedianLength + sizeof(ULONGLONG);
    if (Length > MAXUSHORT)
        return STATUS_NAME_TOO_LONG;
    Entry =
        reinterpret_cast<PIndexEntry>(
            NtfsAllocatePoolWithTag(
                PagedPool,
                Length,
                TAG_BTREE));
    if (!Entry)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Entry, Length);
    RtlCopyMemory(Entry, Median, MedianLength);
    Entry->EntryLength = (USHORT)Length;
    Entry->Flags |= INDEX_ENTRY_NODE;
    *GetSubnodeVCN(Entry) = LeftVcn;
    *Promoted = Entry;
    *PromotedLength = Length;
    return STATUS_SUCCESS;
}

static NTSTATUS
ReadIndexNode(
    _In_ PVolume DiskVolume,
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONG RecordSize,
    _In_ ULONGLONG AllocationUnit,
    _In_ ULONGLONG Vcn,
    _Out_writes_bytes_(RecordSize) PUCHAR Image)
{
    PAttribute IndexAllocationAttribute;
    PAttribute BitmapAttribute;
    PIndexBuffer NodeBuffer;
    ULONGLONG AllocationLength;
    ULONGLONG BitmapLength;
    ULONGLONG AllocationOffset;
    ULONGLONG RecordNumber;
    ULONGLONG BitmapByte;
    ULONG BytesRemaining;
    UCHAR BitmapValue;
    UCHAR BitmapMask;
    NTSTATUS Status;

    IndexAllocationAttribute =
        DirectoryFile->GetAttribute(
            TypeIndexAllocation,
            const_cast<PWSTR>(IndexName));
    BitmapAttribute =
        DirectoryFile->GetAttribute(
            TypeBitmap,
            const_cast<PWSTR>(IndexName));
    if (!IndexAllocationAttribute ||
        !IndexAllocationAttribute->
            IsNonResident ||
        !BitmapAttribute)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    AllocationLength =
        GetAttributeDataSize(
            IndexAllocationAttribute);
    BitmapLength =
        GetAttributeDataSize(BitmapAttribute);

    if (Vcn > ~(ULONGLONG)0 / AllocationUnit)
        return STATUS_FILE_CORRUPT_ERROR;
    AllocationOffset = Vcn * AllocationUnit;
    if (AllocationOffset % RecordSize != 0 ||
        AllocationOffset > AllocationLength ||
        RecordSize >
            AllocationLength -
            AllocationOffset)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    RecordNumber =
        AllocationOffset / RecordSize;
    BitmapByte = RecordNumber >> 3;
    if (BitmapByte >= BitmapLength)
        return STATUS_FILE_CORRUPT_ERROR;
    BytesRemaining = 1;
    Status = DirectoryFile->CopyData(
        BitmapAttribute,
        &BitmapValue,
        &BytesRemaining,
        BitmapByte);
    BitmapMask =
        (UCHAR)(1u << (RecordNumber & 7));
    if (!NT_SUCCESS(Status))
        return Status;
    if (BytesRemaining != 0 ||
        !(BitmapValue & BitmapMask))
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    BytesRemaining = RecordSize;
    Status = DirectoryFile->CopyData(
        IndexAllocationAttribute,
        Image,
        &BytesRemaining,
        AllocationOffset);
    if (!NT_SUCCESS(Status))
        return Status;
    if (BytesRemaining != 0)
        return STATUS_END_OF_FILE;
    NodeBuffer =
        reinterpret_cast<PIndexBuffer>(Image);
    Status = NtfsApplyFixup(
        &NodeBuffer->RecordHeader,
        RecordSize,
        DiskVolume->BytesPerSector);
    if (!NT_SUCCESS(Status))
        return Status;
    if (RtlCompareMemory(
            NodeBuffer->RecordHeader.TypeID,
            "INDX",
            4) != 4 ||
        NodeBuffer->VCN != Vcn)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    return STATUS_SUCCESS;
}

/*
 * Ascends the recorded descent path after a node overflow. Each level
 * splits around its median, publishes the right node, and retries the
 * promoted entry one level up until it fits, reaching the root at worst.
 */
NTSTATUS
Directory::SplitAndPromote(
    _In_ PVolume DiskVolume,
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ BOOLEAN ViewIndex,
    _In_ ULONG RecordSize,
    _In_ ULONGLONG AllocationUnit,
    _In_reads_(PathDepth)
        const ULONGLONG* PathVcns,
    _In_ ULONG PathDepth,
    _Inout_updates_bytes_(RecordSize)
        PUCHAR NodeImage,
    _In_ ULONG LeafInsertionOffset,
    _In_ PIndexEntry FirstPending,
    _In_ ULONG FirstPendingLength)
{
    PIndexEntry Pending = FirstPending;
    PIndexEntry Promoted = NULL;
    PUCHAR Scratch = NULL;
    PUCHAR SideImage = NULL;
    ULONG PendingLength = FirstPendingLength;
    ULONG InsertionOffset =
        LeafInsertionOffset;
    ULONG ScratchCapacity;
    ULONG Level = PathDepth;
    BOOLEAN Retarget = FALSE;
    ULONGLONG RetargetVcn = 0;
    /* The two nodes the previous level wrote (new right node, rewritten left
     * node); whatever names them next must be ordered after them. */
    ULONGLONG ChildVcns[2] = { 0, 0 };
    BOOLEAN HaveChildren = FALSE;
    /* Whether a new node of this split grew the $INDEX_ALLOCATION: the
     * record carrying the grown run list must then land before any node
     * that references the new VCN (OrderPointerAfterRecord). */
    ULONGLONG AllocationSizeAtEntry = 0;
    BOOLEAN AllocationGrew = FALSE;
    PFileRecord AllocationOwner = NULL;
    /* Left halves held back until the node that links their right sibling
     * is written (right, parent, then left); index 0 is the lowest level
     * split, DeferredCount - 1 the highest. */
    PDEFERRED_LEFT_NODE Deferred = NULL;
    ULONG DeferredCount = 0;
    NTSTATUS Status;

    /* One node's entry list (<= RecordSize) plus one pending entry. */
    ScratchCapacity =
        RecordSize + (MAXUSHORT + 1);
    Scratch =
        new(PagedPool, TAG_BTREE)
            UCHAR[ScratchCapacity];
    SideImage =
        new(PagedPool, TAG_BTREE)
            UCHAR[RecordSize];
    if (!Scratch || !SideImage)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }
    Deferred =
        new(PagedPool, TAG_BTREE)
            DEFERRED_LEFT_NODE[PathDepth != 0 ? PathDepth : 1];
    if (!Deferred)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }
    RtlZeroMemory(Deferred,
                  sizeof(DEFERRED_LEFT_NODE) *
                      (PathDepth != 0 ? PathDepth : 1));
    AllocationSizeAtEntry =
        IndexAllocationSize(DirectoryFile, IndexName);

    while (Level != 0)
    {
        PIndexBuffer NodeBuffer =
            reinterpret_cast<PIndexBuffer>(
                NodeImage);
        PIndexNodeHeader Header =
            &NodeBuffer->IndexHeader;
        ULONGLONG NodeVcn =
            PathVcns[Level - 1];
        ULONGLONG NewVcn;
        ULONGLONG NewOrdinal;
        ULONG ListBytes;
        ULONG LeftBytes;
        ULONG MedianOffset;
        ULONG MedianLength;
        BOOLEAN Internal;

        if (Header->TotalIndexSize -
                Header->IndexOffset >
            ScratchCapacity - PendingLength)
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            goto Done;
        }
        Status = ComposeEntryList(
            Header,
            InsertionOffset,
            Pending,
            PendingLength,
            Retarget,
            RetargetVcn,
            Scratch,
            &ListBytes);
        if (!NT_SUCCESS(Status))
            goto Done;
        Internal =
            !!(Header->Flags &
               NTFS_INDEX_HEADER_LARGE);

        if (Header->IndexOffset + ListBytes <=
            Header->AllocatedSize)
        {
            RtlCopyMemory(
                reinterpret_cast<PUCHAR>(
                    Header) +
                    Header->IndexOffset,
                Scratch,
                ListBytes);
            Header->TotalIndexSize =
                Header->IndexOffset +
                ListBytes;
            /*
             * This node names the two children the level below wrote. It
             * was ordered after the new right one, and after the record
             * that grew the allocation, before the left one was rewritten
             * (end of the split case below), so nothing that can fail
             * stands between a rewritten left node and this write. Landing
             * before the rewritten left node only duplicates the moved
             * entries, which a walk tolerates.
             */
            Status = WriteIndexNode(
                DiskVolume,
                DirectoryFile,
                IndexName,
                NodeVcn,
                AllocationUnit,
                RecordSize,
                NodeImage);
            if (NT_SUCCESS(Status))
            {
                /* The links are written; now the left halves may follow. */
                Status = WriteDeferredLeftNodes(
                    DiskVolume,
                    DirectoryFile,
                    IndexName,
                    AllocationUnit,
                    RecordSize,
                    Deferred,
                    DeferredCount,
                    TRUE,
                    NodeVcn,
                    NULL);
            }
            goto Done;
        }

        Status = SplitEntryList(
            Scratch,
            ListBytes,
            &LeftBytes,
            &MedianOffset,
            &MedianLength,
            ViewIndex);
        if (!NT_SUCCESS(Status))
            goto Done;

        Status = SelectIndexRecordSlot(
            DirectoryFile,
            IndexName,
            RecordSize,
            AllocationUnit,
            &NewVcn,
            &NewOrdinal);
        if (!NT_SUCCESS(Status))
            goto Done;

        /*
         * Both halves of this split will name the children the previous
         * level wrote, and the right half is a new block the cache does not
         * hold yet, so no constraint can be placed on it: commit the
         * children synchronously (PointerLength 0). A split above the leaf
         * level is rare.
         */
        if (HaveChildren)
        {
            Status = OrderAfterIndexNodes(
                DiskVolume,
                DirectoryFile,
                IndexName,
                ChildVcns,
                2,
                AllocationUnit,
                RecordSize,
                0,
                0);
            if (!NT_SUCCESS(Status))
                goto Done;
        }
        /* Same for the record that grew the allocation for the children. */
        if (AllocationGrew)
        {
            Status = OrderPointerAfterRecord(
                DiskVolume,
                0,
                0,
                AllocationOwner);
            if (!NT_SUCCESS(Status))
                goto Done;
        }

        /*
         * Publish the right node first: until the parent link switches,
         * it stays unreachable and a failure leaves the previous tree.
         */
        Status = BuildIndexBufferImage(
            SideImage,
            RecordSize,
            DiskVolume->BytesPerSector,
            NewVcn,
            Internal,
            Scratch + MedianOffset +
                MedianLength,
            ListBytes - MedianOffset -
                MedianLength,
            FALSE,
            0);
        if (!NT_SUCCESS(Status))
            goto Done;
        Status = WriteIndexNode(
            DiskVolume,
            DirectoryFile,
            IndexName,
            NewVcn,
            AllocationUnit,
            RecordSize,
            SideImage);
        if (!NT_SUCCESS(Status))
            goto Done;
        if (IndexAllocationSize(DirectoryFile, IndexName) >
            AllocationSizeAtEntry)
        {
            PAttribute AllocationAttribute =
                DirectoryFile->GetAttribute(
                    TypeIndexAllocation,
                    const_cast<PWSTR>(IndexName));

            /* Resolved after the growth: a mapping update may have moved
             * the run list into another record. */
            AllocationGrew = TRUE;
            AllocationOwner = AllocationAttribute
                ? DirectoryFile->GetAttributeOwner(
                    AllocationAttribute)
                : NULL;
        }
        Status = SetIndexRecordBitmapBit(
            DirectoryFile,
            IndexName,
            NewOrdinal,
            TRUE);
        if (!NT_SUCCESS(Status))
            goto Done;

        /*
         * The parent written next (the node above, or the record that owns
         * the root) will name the new right node and, when the allocation
         * grew for it, depends on the record carrying the grown run list.
         * Order it after them. The parent's own block is cached (it was
         * read on the way down), so the barrier can hold it; when the
         * parent splits as well, or the root is pushed down, the pointer
         * also lands in a new block and the children are committed
         * synchronously at that point.
         */
        if (Level - 1 != 0)
        {
            Status = OrderNodeAfterIndexNodes(
                DiskVolume,
                DirectoryFile,
                IndexName,
                PathVcns[Level - 2],
                &NewVcn,
                1,
                AllocationUnit,
                RecordSize);
            if (NT_SUCCESS(Status) && AllocationGrew)
            {
                Status = OrderNodeAfterRecord(
                    DiskVolume,
                    DirectoryFile,
                    IndexName,
                    PathVcns[Level - 2],
                    AllocationUnit,
                    RecordSize,
                    AllocationOwner);
            }
        }
        else
        {
            PAttribute RootAttribute =
                DirectoryFile->GetAttribute(
                    TypeIndexRoot,
                    const_cast<PWSTR>(IndexName));
            PFileRecord RootOwner = RootAttribute
                ? DirectoryFile->GetAttributeOwner(
                    RootAttribute)
                : NULL;

            Status = OrderRecordAfterIndexNodes(
                DiskVolume,
                DirectoryFile,
                IndexName,
                &NewVcn,
                1,
                AllocationUnit,
                RecordSize,
                RootOwner);
            if (NT_SUCCESS(Status) && AllocationGrew)
            {
                Status = OrderRecordAfterRecord(
                    DiskVolume,
                    RootOwner,
                    AllocationOwner);
            }
        }
        if (!NT_SUCCESS(Status))
            goto Done;

        Status = BuildPromotedEntry(
            Scratch + MedianOffset,
            MedianLength,
            Internal,
            NodeVcn,
            &Promoted,
            &PendingLength);
        if (!NT_SUCCESS(Status))
            goto Done;

        /*
         * The left half is held back (WriteDeferredLeftNodes): it may be
         * written only after the node that links the new right node, else
         * a cut that lands the left half first leaves the moved entries
         * unreachable although every record exists. Right, parent, left.
         */
        if (DeferredCount >= (PathDepth != 0 ? PathDepth : 1))
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            goto Done;
        }
        Deferred[DeferredCount].Image =
            new(PagedPool, TAG_BTREE)
                UCHAR[RecordSize];
        if (!Deferred[DeferredCount].Image)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Done;
        }
        Status = BuildIndexBufferImage(
            Deferred[DeferredCount].Image,
            RecordSize,
            DiskVolume->BytesPerSector,
            NodeVcn,
            Internal,
            Scratch,
            LeftBytes,
            TRUE,
            Internal
                ? *GetSubnodeVCN(
                    reinterpret_cast<
                        PIndexEntry>(
                            Scratch +
                            MedianOffset))
                : 0);
        if (!NT_SUCCESS(Status))
            goto Done;
        Deferred[DeferredCount].LeftVcn = NodeVcn;
        Deferred[DeferredCount].RightVcn = NewVcn;
        DeferredCount++;

        /* The parent written next names these two. */
        ChildVcns[0] = NewVcn;
        ChildVcns[1] = NodeVcn;
        HaveChildren = TRUE;

        if (Pending != FirstPending)
            NtfsFreePool(Pending);
        Pending = Promoted;
        Promoted = NULL;
        Retarget = TRUE;
        RetargetVcn = NewVcn;
        Level--;

        if (Level != 0)
        {
            PIndexNodeHeader ParentHeader;

            Status = ReadIndexNode(
                DiskVolume,
                DirectoryFile,
                IndexName,
                RecordSize,
                AllocationUnit,
                PathVcns[Level - 1],
                NodeImage);
            if (!NT_SUCCESS(Status))
                goto Done;
            ParentHeader =
                &reinterpret_cast<PIndexBuffer>(
                    NodeImage)->IndexHeader;
            Status = FindChildLinkOffset(
                ParentHeader,
                PathVcns[Level],
                &InsertionOffset,
                ViewIndex);
            if (!NT_SUCCESS(Status))
                goto Done;
        }
    }

    /* The promoted entry now belongs in the resident root. */
    {
        PAttribute IndexRootAttribute;
        PIndexRootEx IndexRoot;
        PUCHAR RootValue = NULL;
        ULONG RootValueLength;
        ULONG ListBytes;

        IndexRootAttribute =
            DirectoryFile->GetAttribute(
                TypeIndexRoot,
                const_cast<PWSTR>(IndexName));
        if (!IndexRootAttribute ||
            IndexRootAttribute->IsNonResident ||
            IndexRootAttribute->
                Resident.DataLength <
                    FIELD_OFFSET(IndexRootEx,
                                 Header) +
                    sizeof(IndexNodeHeader))
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            goto Done;
        }
        IndexRoot =
            reinterpret_cast<PIndexRootEx>(
                GetResidentDataPointer(
                    IndexRootAttribute));
        Status = FindChildLinkOffset(
            &IndexRoot->Header,
            PathVcns[0],
            &InsertionOffset,
            ViewIndex);
        if (!NT_SUCCESS(Status))
            goto Done;
        if (IndexRoot->Header.TotalIndexSize -
                IndexRoot->Header.IndexOffset >
            ScratchCapacity - PendingLength)
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            goto Done;
        }
        Status = ComposeEntryList(
            &IndexRoot->Header,
            InsertionOffset,
            Pending,
            PendingLength,
            Retarget,
            RetargetVcn,
            Scratch,
            &ListBytes);
        if (!NT_SUCCESS(Status))
            goto Done;

        Status = BuildRootValueFromList(
            IndexRoot,
            Scratch,
            ListBytes,
            TRUE,
            &RootValue,
            &RootValueLength);
        if (!NT_SUCCESS(Status))
            goto Done;
        /*
         * The record that owns the root names the two children the last
         * level wrote. It was ordered after the new right one, and after
         * the record that grew the allocation when that is another record,
         * before the left one was rewritten (end of the split case above).
         */
        Status = RootGrowthKeepsListRoom(DirectoryFile, IndexName, RootValueLength)
            ? ReplaceIndexRootValue(
                  DiskVolume,
                  DirectoryFile,
                  IndexName,
                  RootValue,
                  RootValueLength)
            : STATUS_BUFFER_TOO_SMALL;
        delete[] RootValue;
        if (NT_SUCCESS(Status))
        {
            /* The root links the new right node; the left halves follow. */
            IndexRootAttribute =
                DirectoryFile->GetAttribute(
                    TypeIndexRoot,
                    const_cast<PWSTR>(IndexName));
            Status = WriteDeferredLeftNodes(
                DiskVolume,
                DirectoryFile,
                IndexName,
                AllocationUnit,
                RecordSize,
                Deferred,
                DeferredCount,
                FALSE,
                0,
                IndexRootAttribute
                    ? DirectoryFile->GetAttributeOwner(
                        IndexRootAttribute)
                    : NULL);
        }
        if (Status == STATUS_BUFFER_TOO_SMALL)
        {
            /*
             * The root record itself is full: push the composed root
             * list one level down and re-point the root at it. The node
             * PushDownRoot writes is a new block that will name the
             * children, and the cache does not hold it yet, so the
             * children are committed synchronously first.
             */
            if (HaveChildren)
            {
                Status = OrderAfterIndexNodes(
                    DiskVolume,
                    DirectoryFile,
                    IndexName,
                    ChildVcns,
                    2,
                    AllocationUnit,
                    RecordSize,
                    0,
                    0);
                if (!NT_SUCCESS(Status))
                    goto Done;
            }
            /* The pushed-down node will reference the new VCNs too. */
            if (AllocationGrew)
            {
                Status = OrderPointerAfterRecord(
                    DiskVolume,
                    0,
                    0,
                    AllocationOwner);
                if (!NT_SUCCESS(Status))
                    goto Done;
            }
            Status = PushDownRoot(
                DiskVolume,
                DirectoryFile,
                IndexName,
                IndexRoot,
                Scratch,
                ListBytes,
                TRUE,
                RecordSize,
                AllocationUnit);
            if (NT_SUCCESS(Status))
            {
                /* The pushed-down node and the record that points at it
                 * are written (and committed); the left halves follow. */
                IndexRootAttribute =
                    DirectoryFile->GetAttribute(
                        TypeIndexRoot,
                        const_cast<PWSTR>(IndexName));
                Status = WriteDeferredLeftNodes(
                    DiskVolume,
                    DirectoryFile,
                    IndexName,
                    AllocationUnit,
                    RecordSize,
                    Deferred,
                    DeferredCount,
                    FALSE,
                    0,
                    IndexRootAttribute
                        ? DirectoryFile->GetAttributeOwner(
                            IndexRootAttribute)
                        : NULL);
            }
        }
    }

Done:
    if (Pending && Pending != FirstPending)
        NtfsFreePool(Pending);
    if (Promoted)
        NtfsFreePool(Promoted);
    if (Deferred)
    {
        for (ULONG Index = 0; Index < DeferredCount; Index++)
            delete[] Deferred[Index].Image;
        delete[] Deferred;
    }
    delete[] SideImage;
    delete[] Scratch;
    return Status;
}

/*
 * Inserts one entry into a node's entry list at InsertionOffset (the mirror
 * of CutEntryInNodeHeader). The caller has already ensured the list has room;
 * AllocatedSize, if it changes, is the caller's concern.
 */
static void
SpliceEntryIntoHeader(
    _Inout_ PIndexNodeHeader Header,
    _In_ ULONG InsertionOffset,
    _In_ PIndexEntry Entry,
    _In_ ULONG EntryLength)
{
    RtlMoveMemory(
        reinterpret_cast<PUCHAR>(Header) +
            InsertionOffset + EntryLength,
        reinterpret_cast<PUCHAR>(Header) +
            InsertionOffset,
        Header->TotalIndexSize - InsertionOffset);
    RtlCopyMemory(
        reinterpret_cast<PUCHAR>(Header) +
            InsertionOffset,
        Entry,
        EntryLength);
    Header->TotalIndexSize += EntryLength;
}

NTSTATUS
Directory::PushDownResidentRoot(
    _In_ PFileRecord DirectoryFile)
{
    PAttribute IndexRootAttribute;
    PIndexRootEx IndexRoot;
    PUCHAR List;
    ULONG ListBytes;
    ULONG MinimumListBytes;
    ULONG IndexRecordSize;
    ULONGLONG AllocationUnit;
    BOOLEAN Large;
    NTSTATUS Status;

    if (!DiskVolume || !DirectoryFile ||
        !DirectoryFile->Header ||
        !DirectoryFile->Data ||
        !(DirectoryFile->Header->Flags &
          FR_IS_DIRECTORY) ||
        DiskVolume->IsReadOnly)
    {
        return STATUS_INVALID_PARAMETER;
    }

    IndexRootAttribute =
        DirectoryFile->GetAttribute(
            TypeIndexRoot,
            const_cast<PWSTR>(NtfsI30Name));
    if (!IndexRootAttribute ||
        IndexRootAttribute->IsNonResident ||
        IndexRootAttribute->
            Resident.DataOffset < 0x18 ||
        IndexRootAttribute->
            Resident.DataLength <
                FIELD_OFFSET(IndexRootEx,
                             Header) +
                sizeof(IndexNodeHeader) ||
        IndexRootAttribute->
            Resident.DataOffset >
                IndexRootAttribute->Length ||
        IndexRootAttribute->
            Resident.DataLength >
                IndexRootAttribute->Length -
                IndexRootAttribute->
                    Resident.DataOffset)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    IndexRoot =
        reinterpret_cast<PIndexRootEx>(
            GetResidentDataPointer(
                IndexRootAttribute));
    IndexRecordSize =
        BytesPerIndexRecord(DiskVolume);
    if (IndexRecordSize == 0 ||
        IndexRoot->AttributeType !=
            TypeFileName ||
        IndexRoot->CollationRule !=
            ATTRDEF_COLLATION_FILENAME ||
        IndexRoot->BytesPerIndexRec !=
            IndexRecordSize ||
        IndexRoot->ClusPerIndexRec !=
            (UCHAR)
                DiskVolume->
                    ClustersPerIndexRecord)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    AllocationUnit =
        IndexRecordSize <
            BytesPerCluster(DiskVolume)
            ? DiskVolume->BytesPerSector
            : BytesPerCluster(DiskVolume);
    if (AllocationUnit == 0 ||
        IndexRecordSize % AllocationUnit != 0 ||
        IndexRoot->Header.IndexOffset <
            sizeof(IndexNodeHeader) ||
        IndexRoot->Header.TotalIndexSize <=
            IndexRoot->Header.IndexOffset ||
        IndexRoot->Header.TotalIndexSize >
            IndexRootAttribute->
                Resident.DataLength -
                FIELD_OFFSET(IndexRootEx,
                             Header))
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    Large = !!(IndexRoot->Header.Flags &
               NTFS_INDEX_HEADER_LARGE);
    ListBytes =
        IndexRoot->Header.TotalIndexSize -
        IndexRoot->Header.IndexOffset;
    MinimumListBytes =
        FIELD_OFFSET(IndexEntry, IndexStream) +
        (Large ? sizeof(ULONGLONG) : 0);
    if (ListBytes <= MinimumListBytes)
        return STATUS_BUFFER_TOO_SMALL;

    List =
        new(PagedPool, TAG_BTREE)
            UCHAR[ListBytes];
    if (!List)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(List,
                  reinterpret_cast<PUCHAR>(
                      &IndexRoot->Header) +
                      IndexRoot->Header.IndexOffset,
                  ListBytes);

    Status = PushDownRoot(
        DiskVolume,
        DirectoryFile,
        NtfsI30Name,
        IndexRoot,
        List,
        ListBytes,
        Large,
        IndexRecordSize,
        AllocationUnit);
    delete[] List;
    return Status;
}

NTSTATUS
Directory::AddIndexEntry(
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONG IndexedAttributeType,
    _In_ const IndexSearchKey* Key,
    _In_ PIndexEntry NewEntry,
    _In_ ULONG EntryLength)
{
    NTSTATUS Status = AddIndexEntryInner(DirectoryFile, IndexName, IndexedAttributeType,
                                         Key, NewEntry, EntryLength);

    NTFS_VERIFY_SHAPE_AFTER(Status, DiskVolume, DirectoryFile->RecordNumber, IndexName);
    return Status;
}

NTSTATUS
Directory::AddIndexEntryInner(
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONG IndexedAttributeType,
    _In_ const IndexSearchKey* Key,
    _In_ PIndexEntry NewEntry,
    _In_ ULONG EntryLength)
{
    PAttribute IndexRootAttribute;
    PAttribute IndexAllocationAttribute;
    PAttribute BitmapAttribute;
    PIndexRootEx IndexRoot;
    PUCHAR NewRootData = NULL;
    PUCHAR IndexBufferData = NULL;
    ULONG InsertionOffset;
    ULONG NewRootDataLength;
    ULONG IndexRecordSize;
    ULONG VisitedCount = 0;
    ULONGLONG ChildVcn;
    ULONGLONG VisitedVcns[64];
    ULONGLONG AllocationUnit;
    BOOLEAN Descend;
    NTSTATUS Status;

    if (!DiskVolume || !DirectoryFile ||
        !DirectoryFile->Header ||
        !DirectoryFile->Data ||
        !IndexName || !Key || !NewEntry ||
        DirectoryFile->Header->SequenceNumber == 0 ||
        DiskVolume->IsReadOnly)
    {
        return STATUS_INVALID_PARAMETER;
    }

    IndexRootAttribute =
        DirectoryFile->GetAttribute(
            TypeIndexRoot,
            const_cast<PWSTR>(IndexName));
    if (!IndexRootAttribute ||
        IndexRootAttribute->IsNonResident ||
        IndexRootAttribute->
            Resident.DataOffset < 0x18 ||
        IndexRootAttribute->
            Resident.DataLength <
                FIELD_OFFSET(IndexRootEx,
                             Header) +
                sizeof(IndexNodeHeader) ||
        IndexRootAttribute->
            Resident.DataOffset >
                IndexRootAttribute->Length ||
        IndexRootAttribute->
            Resident.DataLength >
                IndexRootAttribute->Length -
                IndexRootAttribute->
                    Resident.DataOffset)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Done;
    }
    IndexRoot =
        reinterpret_cast<PIndexRootEx>(
            GetResidentDataPointer(
                IndexRootAttribute));
    IndexRecordSize =
        BytesPerIndexRecord(DiskVolume);
    if (IndexRecordSize == 0 ||
        IndexRoot->AttributeType !=
            IndexedAttributeType ||
        IndexRoot->CollationRule !=
            Key->CollationRule ||
        IndexRoot->BytesPerIndexRec !=
            IndexRecordSize ||
        IndexRoot->ClusPerIndexRec !=
            (UCHAR)
                DiskVolume->
                    ClustersPerIndexRecord)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Done;
    }
    AllocationUnit =
        IndexRecordSize <
            BytesPerCluster(DiskVolume)
            ? DiskVolume->BytesPerSector
            : BytesPerCluster(DiskVolume);
    if (AllocationUnit == 0 ||
        IndexRecordSize % AllocationUnit != 0)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Done;
    }

    Status = FindIndexInsertionPoint(
        DiskVolume,
        &IndexRoot->Header,
        IndexRootAttribute->
            Resident.DataLength -
            FIELD_OFFSET(IndexRootEx,
                         Header),
        Key,
        &InsertionOffset,
        &ChildVcn,
        &Descend);
    if (!NT_SUCCESS(Status))
        goto Done;

    if (!Descend)
    {
        PIndexNodeHeader NewHeader;
        ULONG RootPrefix =
            FIELD_OFFSET(IndexRootEx, Header);
        ULONG NewTotal;

        if (IndexRoot->Header.TotalIndexSize >
                MAXULONG - EntryLength)
        {
            Status = STATUS_FILE_TOO_LARGE;
            goto Done;
        }
        NewTotal =
            IndexRoot->Header.TotalIndexSize +
            EntryLength;
        NewRootDataLength =
            RootPrefix + NewTotal;
        NewRootData =
            new(PagedPool, TAG_BTREE)
                UCHAR[NewRootDataLength];
        if (!NewRootData)
        {
            Status =
                STATUS_INSUFFICIENT_RESOURCES;
            goto Done;
        }
        RtlZeroMemory(NewRootData,
                      NewRootDataLength);
        RtlCopyMemory(NewRootData,
                      IndexRoot,
                      RootPrefix +
                      IndexRoot->
                          Header.TotalIndexSize);
        NewHeader =
            &reinterpret_cast<PIndexRootEx>(
                NewRootData)->Header;
        SpliceEntryIntoHeader(
            NewHeader,
            InsertionOffset,
            NewEntry,
            EntryLength);
        NewHeader->AllocatedSize = NewTotal;

        Status = RootGrowthKeepsListRoom(DirectoryFile, IndexName, NewRootDataLength)
            ? ReplaceIndexRootValue(
                  DiskVolume,
                  DirectoryFile,
                  IndexName,
                  NewRootData,
                  NewRootDataLength)
            : STATUS_BUFFER_TOO_SMALL;
        if (Status == STATUS_BUFFER_TOO_SMALL)
        {
            /*
             * The base record cannot hold the grown root: promote the
             * composed entry list into the first index buffer.
             */
            Status = PushDownRoot(
                DiskVolume,
                DirectoryFile,
                IndexName,
                IndexRoot,
                reinterpret_cast<PUCHAR>(
                    NewHeader) +
                    NewHeader->IndexOffset,
                NewHeader->TotalIndexSize -
                    NewHeader->IndexOffset,
                FALSE,
                IndexRecordSize,
                AllocationUnit);
        }
        if (!NT_SUCCESS(Status))
            goto Done;
        goto Done;
    }

    IndexAllocationAttribute =
        DirectoryFile->GetAttribute(
            TypeIndexAllocation,
            const_cast<PWSTR>(IndexName));
    BitmapAttribute =
        DirectoryFile->GetAttribute(
            TypeBitmap,
            const_cast<PWSTR>(IndexName));
    if (!IndexAllocationAttribute ||
        !IndexAllocationAttribute->
            IsNonResident ||
        !BitmapAttribute)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Done;
    }

    if (!DiskVolume->IndexWorkBuffer ||
        DiskVolume->IndexWorkBufferSize < IndexRecordSize)
    {
        delete[] DiskVolume->IndexWorkBuffer;
        DiskVolume->IndexWorkBuffer =
            new(PagedPool, TAG_BTREE) UCHAR[IndexRecordSize];
        DiskVolume->IndexWorkBufferSize =
            DiskVolume->IndexWorkBuffer ? IndexRecordSize : 0;
    }
    IndexBufferData = DiskVolume->IndexWorkBuffer;
    if (!IndexBufferData)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    while (Descend)
    {
        PIndexBuffer NodeBuffer;

        if (VisitedCount ==
            RTL_NUMBER_OF(VisitedVcns))
        {
            Status =
                STATUS_FILE_CORRUPT_ERROR;
            goto Done;
        }
        for (ULONG Index = 0;
             Index < VisitedCount;
             Index++)
        {
            if (VisitedVcns[Index] ==
                ChildVcn)
            {
                Status =
                    STATUS_FILE_CORRUPT_ERROR;
                goto Done;
            }
        }
        VisitedVcns[VisitedCount++] =
            ChildVcn;

        Status = ReadIndexNode(
            DiskVolume,
            DirectoryFile,
            IndexName,
            IndexRecordSize,
            AllocationUnit,
            ChildVcn,
            IndexBufferData);
        if (!NT_SUCCESS(Status))
            goto Done;
        NodeBuffer =
            reinterpret_cast<PIndexBuffer>(
                IndexBufferData);

        Descend = FALSE;
        Status = FindIndexInsertionPoint(
            DiskVolume,
            &NodeBuffer->IndexHeader,
            IndexRecordSize -
                FIELD_OFFSET(IndexBuffer,
                             IndexHeader),
            Key,
            &InsertionOffset,
            &ChildVcn,
            &Descend);
        if (!NT_SUCCESS(Status))
            goto Done;
        if (Descend)
            continue;

        if (NodeBuffer->
                IndexHeader.TotalIndexSize >
                    MAXULONG - EntryLength)
        {
            Status = STATUS_FILE_TOO_LARGE;
            goto Done;
        }
        if (NodeBuffer->
                IndexHeader.TotalIndexSize +
                    EntryLength >
            NodeBuffer->
                IndexHeader.AllocatedSize)
        {
            /*
             * The target leaf is full: split along the recorded path
             * and let the median climb toward the root.
             */
            Status = SplitAndPromote(
                DiskVolume,
                DirectoryFile,
                IndexName,
                Key->CollationRule != ATTRDEF_COLLATION_FILENAME,
                IndexRecordSize,
                AllocationUnit,
                VisitedVcns,
                VisitedCount,
                IndexBufferData,
                InsertionOffset,
                NewEntry,
                EntryLength);
            if (!NT_SUCCESS(Status))
                goto Done;
            break;
        }
        SpliceEntryIntoHeader(
            &NodeBuffer->IndexHeader,
            InsertionOffset,
            NewEntry,
            EntryLength);

        Status = WriteIndexNode(
            DiskVolume,
            DirectoryFile,
            IndexName,
            VisitedVcns[VisitedCount - 1],
            AllocationUnit,
            IndexRecordSize,
            IndexBufferData);
        if (!NT_SUCCESS(Status))
            goto Done;
        break;
    }

Done:
    /* Volume-owned scratch. */
    delete[] NewRootData;
    return Status;
}

NTSTATUS
Directory::AddFileToDirectory(
    _In_ PFileRecord DirectoryFile,
    _In_ ULONGLONG FileReference,
    _In_ PFileNameEx FileToAdd)
{
    IndexSearchKey Key;
    UNICODE_STRING Name;
    PIndexEntry NewEntry = NULL;
    ULONG EntryLength;
    NTSTATUS TimestampStatus;
    NTSTATUS Status;

    if (!DiskVolume || !DirectoryFile ||
        !DirectoryFile->Header ||
        !FileToAdd ||
        !(DirectoryFile->Header->Flags &
          FR_IS_DIRECTORY) ||
        FileToAdd->ParentFileReference !=
            MakeFileReference(DirectoryFile))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Status = BuildIndexEntry(
        FileReference,
        FileToAdd,
        &NewEntry,
        &EntryLength);
    if (!NT_SUCCESS(Status))
        return Status;
    Name = NtfsMakeCountedUnicodeString(
        FileToAdd->Name,
        FileToAdd->NameLength * sizeof(WCHAR));
    Key.CollationRule = ATTRDEF_COLLATION_FILENAME;
    Key.Name = &Name;
    Key.Value = NULL;
    Key.ValueLength = 0;
    Key.CaseSensitive = DirectoryFile->IsCaseSensitiveDirectory();

    /*
     * Order the index entry that names the new file after the file's own
     * record, so a crash cannot leave the entry naming a not-yet-durable
     * (zeroed) record (J32 grow/ residual). Barrier ON: arm
     * IndexBarrierRecordOffset; the index block writes (WriteIndexNode /
     * WriteFileRecordToMFT) install BarrierAfter before dirtying their
     * block. Barrier OFF: make the record durable synchronously before the
     * insert, as the record-before-bitmap fix does for the bitmap bit.
     *
     * The record's disk offset is what both modes order on. When it cannot
     * be resolved there is nothing to arm and nothing to commit by range: a
     * record spanning a run boundary is made durable by a full flush before
     * the insert (the proven record-first fallback), and any other failure
     * fails the insert. The insert never proceeds unordered.
     */
    {
        ULONGLONG TargetRecordOffset = 0;

        Status = DiskVolume->MFT->GetFileRecordDiskOffset(
            FileReference & 0x0000FFFFFFFFFFFFULL, &TargetRecordOffset);
        if (Status == STATUS_NOT_IMPLEMENTED)
        {
            /* No single range to order on: everything durable first. */
            Status = DiskVolume->FlushVolume();
        }
        else if (NT_SUCCESS(Status))
        {
            if (DiskVolume->WriteBarrierEnabled())
            {
                DiskVolume->IndexBarrierRecordOffset = TargetRecordOffset;
            }
            else
            {
                Status = DiskVolume->CommitRange(
                    TargetRecordOffset, DiskVolume->MFT->FileRecordSize);
                if (Status == STATUS_NOT_IMPLEMENTED)
                    Status = DiskVolume->FlushVolume();
            }
        }
        if (!NT_SUCCESS(Status))
        {
            NtfsFreePool(NewEntry);
            return Status;
        }
    }

    Status = AddIndexEntry(
        DirectoryFile,
        NtfsI30Name,
        TypeFileName,
        &Key,
        NewEntry,
        EntryLength);
    DiskVolume->IndexBarrierRecordOffset = 0;
    NtfsFreePool(NewEntry);
    if (!NT_SUCCESS(Status))
        return Status;

    TimestampStatus =
        DirectoryFile->TouchDirectory();
    if (!NT_SUCCESS(TimestampStatus))
    {
        DPRINT1(
            "Directory entry was committed but "
            "timestamp update failed: 0x%lx.\n",
            TimestampStatus);
    }
    return STATUS_SUCCESS;
}

static void
CutEntryInNodeHeader(
    _Inout_ PIndexNodeHeader Header,
    _In_ ULONG EntryOffset,
    _In_ ULONG EntryLength)
{
    RtlMoveMemory(
        reinterpret_cast<PUCHAR>(Header) +
            EntryOffset,
        reinterpret_cast<PUCHAR>(Header) +
            EntryOffset + EntryLength,
        Header->TotalIndexSize -
            EntryOffset - EntryLength);
    Header->TotalIndexSize -= EntryLength;
}

/*
 * Copies only the key of an index entry: file reference, stream, and no
 * child slot. The caller reattaches a child through BuildPromotedEntry
 * when the key replaces an internal entry.
 */
static NTSTATUS
BuildKeyOnlyEntry(
    _In_ PIndexEntry Source,
    _Outptr_result_bytebuffer_(*KeyLength)
        PIndexEntry* KeyEntry,
    _Out_ PULONG KeyLength)
{
    PIndexEntry Entry;
    ULONG Length;

    *KeyEntry = NULL;
    *KeyLength = 0;
    if (Source->StreamLength == 0)
        return STATUS_FILE_CORRUPT_ERROR;
    Length = ALIGN_UP_BY(
        FIELD_OFFSET(IndexEntry, IndexStream) +
            Source->StreamLength,
        sizeof(ULONGLONG));
    Entry =
        reinterpret_cast<PIndexEntry>(
            NtfsAllocatePoolWithTag(
                PagedPool,
                Length,
                TAG_BTREE));
    if (!Entry)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Entry, Length);
    Entry->Data.Directory.IndexedFile =
        Source->Data.Directory.IndexedFile;
    Entry->EntryLength = (USHORT)Length;
    Entry->StreamLength = Source->StreamLength;
    RtlCopyMemory(Entry->IndexStream,
                  Source->IndexStream,
                  Source->StreamLength);
    *KeyEntry = Entry;
    *KeyLength = Length;
    return STATUS_SUCCESS;
}

/*
 * Frees the $I30 bitmap bits of a subtree that holds no keys at all: a
 * straight chain of END-only nodes left behind by removals.
 */
static NTSTATUS
ReleaseEndOnlyChain(
    _In_ PVolume DiskVolume,
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONG RecordSize,
    _In_ ULONGLONG AllocationUnit,
    _In_ ULONGLONG ChainVcn)
{
    PUCHAR Image;
    ULONG Depth = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    Image =
        new(PagedPool, TAG_BTREE)
            UCHAR[RecordSize];
    if (!Image)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (;;)
    {
        PIndexBuffer NodeBuffer;
        PIndexEntry Entry;
        BOOLEAN Internal;

        if (Depth++ > 64)
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            break;
        }
        Status = ReadIndexNode(
            DiskVolume,
            DirectoryFile,
            IndexName,
            RecordSize,
            AllocationUnit,
            ChainVcn,
            Image);
        if (!NT_SUCCESS(Status))
            break;
        NodeBuffer =
            reinterpret_cast<PIndexBuffer>(Image);
        Entry =
            reinterpret_cast<PIndexEntry>(
                reinterpret_cast<PUCHAR>(
                    &NodeBuffer->IndexHeader) +
                NodeBuffer->
                    IndexHeader.IndexOffset);
        if (!NtfsIsDirectoryIndexEntryValid(
                Entry,
                NodeBuffer->
                    IndexHeader.TotalIndexSize -
                NodeBuffer->
                    IndexHeader.IndexOffset) ||
            !(Entry->Flags & INDEX_ENTRY_END))
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            break;
        }
        Internal =
            !!(Entry->Flags & INDEX_ENTRY_NODE);

        Status = SetIndexRecordBitmapBit(
            DirectoryFile,
            IndexName,
            (ChainVcn * AllocationUnit) /
                RecordSize,
            FALSE);
        if (!NT_SUCCESS(Status))
            break;
        if (!Internal)
            break;
        ChainVcn = *GetSubnodeVCN(Entry);
    }

    delete[] Image;
    return Status;
}

/*
 * Removes and returns the collation maximum of one subtree as a key-only
 * entry. When the subtree carries no keys at all, Found stays FALSE and
 * nothing is modified; the caller releases the chain. An internal node
 * whose END subtree runs dry donates its own last entry and adopts that
 * entry's child as the new rightmost subtree.
 */
static NTSTATUS
RemoveMaxFromSubtree(
    _In_ PVolume DiskVolume,
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONG RecordSize,
    _In_ ULONGLONG AllocationUnit,
    _In_ ULONGLONG Vcn,
    _In_ ULONG Depth,
    _Outptr_result_maybenull_
        PIndexEntry* KeyEntry,
    _Out_ PULONG KeyLength,
    _Out_ PBOOLEAN Found,
    _Out_ PBOOLEAN LeafEmptied,
    _Inout_ PDEFERRED_INDEX_WRITE Deferred)
{
    PIndexBuffer NodeBuffer;
    PIndexEntry Entry;
    PIndexEntry LastReal = NULL;
    PIndexEntry EndEntry = NULL;
    PUCHAR Image = NULL;
    ULONG_PTR End;
    ULONG LastRealOffset = 0;
    BOOLEAN Internal;
    NTSTATUS Status;

    *KeyEntry = NULL;
    *KeyLength = 0;
    *Found = FALSE;
    *LeafEmptied = FALSE;
    if (Depth > 64)
        return STATUS_FILE_CORRUPT_ERROR;

    Image =
        new(PagedPool, TAG_BTREE)
            UCHAR[RecordSize];
    if (!Image)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = ReadIndexNode(
        DiskVolume,
        DirectoryFile,
        IndexName,
        RecordSize,
        AllocationUnit,
        Vcn,
        Image);
    if (!NT_SUCCESS(Status))
        goto Done;
    NodeBuffer =
        reinterpret_cast<PIndexBuffer>(Image);

    Entry =
        reinterpret_cast<PIndexEntry>(
            reinterpret_cast<PUCHAR>(
                &NodeBuffer->IndexHeader) +
            NodeBuffer->IndexHeader.IndexOffset);
    End =
        reinterpret_cast<ULONG_PTR>(
            &NodeBuffer->IndexHeader) +
        NodeBuffer->IndexHeader.TotalIndexSize;
    while (reinterpret_cast<ULONG_PTR>(Entry) <
           End)
    {
        if (!NtfsIsDirectoryIndexEntryValid(
                Entry,
                (ULONG)(End -
                    reinterpret_cast<ULONG_PTR>(
                        Entry))))
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            goto Done;
        }
        if (Entry->Flags & INDEX_ENTRY_END)
        {
            EndEntry = Entry;
            break;
        }
        LastReal = Entry;
        LastRealOffset =
            (ULONG)(
                reinterpret_cast<PUCHAR>(Entry) -
                reinterpret_cast<PUCHAR>(
                    &NodeBuffer->IndexHeader));
        Entry =
            reinterpret_cast<PIndexEntry>(
                reinterpret_cast<PUCHAR>(Entry) +
                Entry->EntryLength);
    }
    if (!EndEntry)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Done;
    }
    Internal =
        !!(NodeBuffer->IndexHeader.Flags &
           NTFS_INDEX_HEADER_LARGE);

    if (Internal)
    {
        ULONGLONG RightVcn =
            *GetSubnodeVCN(EndEntry);

        Status = RemoveMaxFromSubtree(
            DiskVolume,
            DirectoryFile,
            IndexName,
            RecordSize,
            AllocationUnit,
            RightVcn,
            Depth + 1,
            KeyEntry,
            KeyLength,
            Found,
            LeafEmptied,
            Deferred);
        if (!NT_SUCCESS(Status) || *Found)
            goto Done;

        if (!LastReal)
        {
            /* This level holds no key either. */
            goto Done;
        }

        /*
         * The rightmost subtree is a bare END chain. Donate the last
         * entry's key upward and let its child become the new rightmost
         * subtree. The node image is handed to the caller (Deferred): it
         * must not reach the disk before the ancestor that takes the key,
         * and the chain's bits are released only once that node write is
         * durable, so a reused block can never be reached through a stale
         * pointer on the disk.
         */
        Status = BuildKeyOnlyEntry(
            LastReal,
            KeyEntry,
            KeyLength);
        if (!NT_SUCCESS(Status))
            goto Done;
        {
            ULONGLONG DonatedChild =
                *GetSubnodeVCN(LastReal);
            ULONG CutLength =
                LastReal->EntryLength;

            CutEntryInNodeHeader(
                &NodeBuffer->IndexHeader,
                LastRealOffset,
                CutLength);
            EndEntry =
                reinterpret_cast<PIndexEntry>(
                    reinterpret_cast<PUCHAR>(
                        &NodeBuffer->
                            IndexHeader) +
                    NodeBuffer->
                        IndexHeader.
                            TotalIndexSize -
                    (FIELD_OFFSET(IndexEntry,
                                  IndexStream) +
                     sizeof(ULONGLONG)));
            *GetSubnodeVCN(EndEntry) =
                DonatedChild;
        }
        RtlCopyMemory(Deferred->Image, Image, RecordSize);
        Deferred->Vcn = Vcn;
        Deferred->Valid = TRUE;
        Deferred->ChainVcn = RightVcn;
        Deferred->ChainValid = TRUE;
        *Found = TRUE;
        goto Done;
    }

    if (!LastReal)
        goto Done;
    Status = BuildKeyOnlyEntry(
        LastReal,
        KeyEntry,
        KeyLength);
    if (!NT_SUCCESS(Status))
        goto Done;
    CutEntryInNodeHeader(
        &NodeBuffer->IndexHeader,
        LastRealOffset,
        LastReal->EntryLength);
    *LeafEmptied =
        LastRealOffset ==
            NodeBuffer->IndexHeader.IndexOffset;
    /*
     * The leaf gives up its maximum; the caller writes it into an ancestor
     * first and this image after, ordered behind it (Deferred), so a cut
     * can duplicate the key but never lose it.
     */
    RtlCopyMemory(Deferred->Image, Image, RecordSize);
    Deferred->Vcn = Vcn;
    Deferred->Valid = TRUE;
    *Found = TRUE;

Done:
    if (!NT_SUCCESS(Status) || !*Found)
    {
        if (*KeyEntry)
        {
            NtfsFreePool(*KeyEntry);
            *KeyEntry = NULL;
            *KeyLength = 0;
        }
    }
    delete[] Image;
    return Status;
}

static NTSTATUS
FindTrailingIndexEntries(
    _In_ PIndexNodeHeader Header,
    _In_ ULONG HeaderBytes,
    _In_ BOOLEAN ViewIndex,
    _Out_ PIndexEntry* LastReal,
    _Out_ PIndexEntry* EndEntry)
{
    PIndexEntry Entry;
    ULONG_PTR End;

    *LastReal = NULL;
    *EndEntry = NULL;
    if (HeaderBytes < sizeof(*Header) ||
        Header->IndexOffset < sizeof(*Header) ||
        Header->IndexOffset >
            Header->TotalIndexSize ||
        Header->TotalIndexSize >
            Header->AllocatedSize ||
        Header->AllocatedSize > HeaderBytes)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    Entry =
        reinterpret_cast<PIndexEntry>(
            reinterpret_cast<PUCHAR>(Header) +
            Header->IndexOffset);
    End =
        reinterpret_cast<ULONG_PTR>(Header) +
        Header->TotalIndexSize;
    while (reinterpret_cast<ULONG_PTR>(Entry) <
           End)
    {
        if (!IsIndexEntryValid(
                Entry,
                (ULONG)(End -
                    reinterpret_cast<ULONG_PTR>(
                        Entry)),
                ViewIndex))
        {
            return STATUS_FILE_CORRUPT_ERROR;
        }
        if (Entry->Flags & INDEX_ENTRY_END)
        {
            *EndEntry = Entry;
            return STATUS_SUCCESS;
        }
        *LastReal = Entry;
        Entry =
            reinterpret_cast<PIndexEntry>(
                reinterpret_cast<PUCHAR>(Entry) +
                Entry->EntryLength);
    }
    return STATUS_FILE_CORRUPT_ERROR;
}

static void
MakeEntrySearchKey(
    _In_ ULONG CollationRule,
    _In_ PIndexEntry Entry,
    _Out_ PUNICODE_STRING NameStorage,
    _Out_ IndexSearchKey* Key)
{
    Key->CollationRule = CollationRule;
    Key->Name = NULL;
    Key->Value = NULL;
    Key->ValueLength = 0;
    Key->CaseSensitive = FALSE;
    if (CollationRule == ATTRDEF_COLLATION_FILENAME)
    {
        PFileNameEx IndexedName =
            reinterpret_cast<PFileNameEx>(
                Entry->IndexStream);

        *NameStorage =
            NtfsMakeCountedUnicodeString(
                IndexedName->Name,
                IndexedName->NameLength *
                    sizeof(WCHAR));
        Key->Name = NameStorage;
    }
    else
    {
        Key->Value = Entry->IndexStream;
        Key->ValueLength = Entry->StreamLength;
    }
}

NTSTATUS
Directory::ReleaseEmptyIndexNode(
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONG IndexedAttributeType,
    _In_ const IndexSearchKey* PathKey,
    _In_ ULONG IndexRecordSize,
    _In_ ULONGLONG AllocationUnit)
{
    const ULONG RootPrefix =
        FIELD_OFFSET(IndexRootEx, Header);
    const ULONG NodePrefix =
        FIELD_OFFSET(IndexBuffer, IndexHeader);
    const BOOLEAN ViewIndex =
        PathKey->CollationRule !=
            ATTRDEF_COLLATION_FILENAME;
    PAttribute RootAttribute;
    PIndexRootEx IndexRoot;
    PIndexNodeHeader ParentHeader;
    PIndexEntry Entry;
    PIndexEntry Link;
    PIndexEntry LastReal;
    PIndexEntry EndEntry;
    PUCHAR NodeImage = NULL;
    PUCHAR RootValue = NULL;
    PUCHAR RootBackup = NULL;
    ULONGLONG Path[64];
    ULONGLONG ChildVcn;
    ULONG PathCount = 0;
    ULONG Level;
    ULONG LinkOffset;
    ULONG RootBackupLength = 0;
    BOOLEAN Descend;
    BOOLEAN FoundExact;
    BOOLEAN Rightmost = FALSE;
    BOOLEAN ParentIsRoot;
    NTSTATUS BitmapStatus;
    NTSTATUS Status;

    /* Kept for the callers; the reinsert that used it is gone. */
    (void)IndexedAttributeType;

    RootAttribute =
        DirectoryFile->GetAttribute(
            TypeIndexRoot,
            const_cast<PWSTR>(IndexName));
    if (!RootAttribute ||
        RootAttribute->IsNonResident ||
        RootAttribute->Resident.DataLength <
            RootPrefix + sizeof(IndexNodeHeader))
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    IndexRoot =
        reinterpret_cast<PIndexRootEx>(
            GetResidentDataPointer(RootAttribute));
    Status = FindIndexInsertionPoint(
        DiskVolume,
        &IndexRoot->Header,
        RootAttribute->Resident.DataLength -
            RootPrefix,
        PathKey,
        &LinkOffset,
        &ChildVcn,
        &Descend,
        &FoundExact);
    if (!NT_SUCCESS(Status))
        return Status;
    if (FoundExact)
    {
        Entry =
            reinterpret_cast<PIndexEntry>(
                reinterpret_cast<PUCHAR>(
                    &IndexRoot->Header) +
                LinkOffset);
        if (!(Entry->Flags & INDEX_ENTRY_NODE))
            return STATUS_SUCCESS;
        ChildVcn = *GetSubnodeVCN(Entry);
        Rightmost = TRUE;
    }
    else if (!Descend)
    {
        return STATUS_SUCCESS;
    }

    NodeImage =
        new(PagedPool, TAG_BTREE)
            UCHAR[IndexRecordSize];
    if (!NodeImage)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (;;)
    {
        PIndexNodeHeader Header;

        if (PathCount == RTL_NUMBER_OF(Path))
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            goto Done;
        }
        for (ULONG Index = 0;
             Index < PathCount;
             Index++)
        {
            if (Path[Index] == ChildVcn)
            {
                Status = STATUS_FILE_CORRUPT_ERROR;
                goto Done;
            }
        }
        Path[PathCount++] = ChildVcn;

        Status = ReadIndexNode(
            DiskVolume,
            DirectoryFile,
            IndexName,
            IndexRecordSize,
            AllocationUnit,
            ChildVcn,
            NodeImage);
        if (!NT_SUCCESS(Status))
            goto Done;
        Header =
            &reinterpret_cast<PIndexBuffer>(
                NodeImage)->IndexHeader;
        if (Rightmost)
        {
            Status = FindTrailingIndexEntries(
                Header,
                IndexRecordSize - NodePrefix,
                ViewIndex,
                &LastReal,
                &EndEntry);
            if (!NT_SUCCESS(Status))
                goto Done;
            if (!(EndEntry->Flags & INDEX_ENTRY_NODE))
                break;
            ChildVcn = *GetSubnodeVCN(EndEntry);
            continue;
        }
        Status = FindIndexInsertionPoint(
            DiskVolume,
            Header,
            IndexRecordSize - NodePrefix,
            PathKey,
            &LinkOffset,
            &ChildVcn,
            &Descend,
            &FoundExact);
        if (!NT_SUCCESS(Status))
            goto Done;
        if (FoundExact)
        {
            Entry =
                reinterpret_cast<PIndexEntry>(
                    reinterpret_cast<PUCHAR>(Header) +
                    LinkOffset);
            if (!(Entry->Flags & INDEX_ENTRY_NODE))
                goto Done;
            ChildVcn = *GetSubnodeVCN(Entry);
            Rightmost = TRUE;
            continue;
        }
        if (!Descend)
            break;
    }

    Status = FindTrailingIndexEntries(
        &reinterpret_cast<PIndexBuffer>(
            NodeImage)->IndexHeader,
        IndexRecordSize - NodePrefix,
        ViewIndex,
        &LastReal,
        &EndEntry);
    if (!NT_SUCCESS(Status) || LastReal ||
        (EndEntry->Flags & INDEX_ENTRY_NODE))
    {
        goto Done;
    }

    Level = PathCount - 1;
    for (;;)
    {
        ParentIsRoot = Level == 0;
        if (ParentIsRoot)
        {
            RootAttribute =
                DirectoryFile->GetAttribute(
                    TypeIndexRoot,
                    const_cast<PWSTR>(IndexName));
            if (!RootAttribute ||
                RootAttribute->IsNonResident ||
                RootAttribute->Resident.DataLength <
                    RootPrefix +
                        sizeof(IndexNodeHeader))
            {
                Status = STATUS_FILE_CORRUPT_ERROR;
                goto Done;
            }
            RootBackupLength =
                RootAttribute->Resident.DataLength;
            RootBackup =
                new(PagedPool, TAG_BTREE)
                    UCHAR[RootBackupLength];
            RootValue =
                new(PagedPool, TAG_BTREE)
                    UCHAR[RootBackupLength];
            if (!RootBackup || !RootValue)
            {
                Status =
                    STATUS_INSUFFICIENT_RESOURCES;
                goto Done;
            }
            RtlCopyMemory(
                RootBackup,
                GetResidentDataPointer(RootAttribute),
                RootBackupLength);
            RtlCopyMemory(RootValue,
                          RootBackup,
                          RootBackupLength);
            ParentHeader =
                &reinterpret_cast<PIndexRootEx>(
                    RootValue)->Header;
            Status = FindTrailingIndexEntries(
                ParentHeader,
                RootBackupLength - RootPrefix,
                ViewIndex,
                &LastReal,
                &EndEntry);
        }
        else
        {
            Status = ReadIndexNode(
                DiskVolume,
                DirectoryFile,
                IndexName,
                IndexRecordSize,
                AllocationUnit,
                Path[Level - 1],
                NodeImage);
            if (!NT_SUCCESS(Status))
                goto Done;
            ParentHeader =
                &reinterpret_cast<PIndexBuffer>(
                    NodeImage)->IndexHeader;
            Status = FindTrailingIndexEntries(
                ParentHeader,
                IndexRecordSize - NodePrefix,
                ViewIndex,
                &LastReal,
                &EndEntry);
        }
        if (!NT_SUCCESS(Status))
            goto Done;
        Status = FindChildLinkOffset(
            ParentHeader,
            Path[Level],
            &LinkOffset,
            ViewIndex);
        if (!NT_SUCCESS(Status))
            goto Done;
        Link =
            reinterpret_cast<PIndexEntry>(
                reinterpret_cast<PUCHAR>(
                    ParentHeader) +
                LinkOffset);
        if (!(Link->Flags & INDEX_ENTRY_END) ||
            LastReal || ParentIsRoot)
        {
            break;
        }
        Level--;
    }

    if (!(Link->Flags & INDEX_ENTRY_END) || LastReal)
    {
        /*
         * Releasing the chain here would take a key out of the parent
         * (the link's own key, or the last entry whose child the END link
         * would adopt) and put it back through a fresh insert: two held-
         * back writes the cache may land in either order, and a cut
         * between them loses a live name from a tree that looks valid.
         * Leave the empty leaf and its END-only chain in place instead:
         * linked, bits set, no keys, a valid shape that the next insert in
         * its key range reuses and that is freed when its link key is
         * removed (RemoveIndexEntry). Only a chain that reaches the root
         * with no key left is released, below.
         */
        Status = STATUS_SUCCESS;
        goto Done;
    }

    EndEntry->EntryLength =
        FIELD_OFFSET(IndexEntry, IndexStream);
    EndEntry->Flags = INDEX_ENTRY_END;
    ParentHeader->TotalIndexSize -=
        sizeof(ULONGLONG);
    ParentHeader->Flags &=
        ~NTFS_INDEX_HEADER_LARGE;

    if (ParentIsRoot)
    {
        ParentHeader->AllocatedSize =
            ParentHeader->TotalIndexSize;
        Status = ReplaceIndexRootValue(
            DiskVolume,
            DirectoryFile,
            IndexName,
            RootValue,
            RootPrefix +
                ParentHeader->TotalIndexSize);
    }
    else
    {
        Status = WriteIndexNode(
            DiskVolume,
            DirectoryFile,
            IndexName,
            Path[Level - 1],
            AllocationUnit,
            IndexRecordSize,
            NodeImage);
    }
    if (!NT_SUCCESS(Status))
        goto Done;

    /*
     * The pointer to the freed chain is gone from the parent in memory.
     * Make that write durable before the bits are cleared
     * (CommitIndexPointerHolder): a cleared bit lets the next split reuse
     * a block at once, and a stale parent on the disk would then point
     * into a foreign subtree after a cut. Failing here leaks the blocks,
     * which is the safe side.
     */
    if (ParentIsRoot)
    {
        RootAttribute =
            DirectoryFile->GetAttribute(
                TypeIndexRoot,
                const_cast<PWSTR>(IndexName));
        Status = CommitIndexPointerHolder(
            DiskVolume,
            DirectoryFile,
            IndexName,
            AllocationUnit,
            IndexRecordSize,
            RootAttribute
                ? DirectoryFile->GetAttributeOwner(
                    RootAttribute)
                : NULL,
            0);
    }
    else
    {
        Status = CommitIndexPointerHolder(
            DiskVolume,
            DirectoryFile,
            IndexName,
            AllocationUnit,
            IndexRecordSize,
            NULL,
            Path[Level - 1]);
    }
    if (!NT_SUCCESS(Status))
        goto Done;

    BitmapStatus = STATUS_SUCCESS;
    for (ULONG Index = Level;
         Index < PathCount;
         Index++)
    {
        NTSTATUS ClearStatus =
            SetIndexRecordBitmapBit(
                DirectoryFile,
                IndexName,
                (Path[Index] * AllocationUnit) /
                    IndexRecordSize,
                FALSE);

        if (!NT_SUCCESS(ClearStatus))
            BitmapStatus = ClearStatus;
    }
    Status = BitmapStatus;

Done:
    delete[] RootBackup;
    delete[] RootValue;
    delete[] NodeImage;
    return Status;
}

NTSTATUS
Directory::RemoveFileFromDirectory(
    _In_ PFileRecord DirectoryFile,
    _In_ ULONGLONG FileReference,
    _In_ PUNICODE_STRING Name)
{
    IndexSearchKey Key = { ATTRDEF_COLLATION_FILENAME, Name, NULL, 0, FALSE };

    if (!DirectoryFile || !DirectoryFile->Header ||
        !Name || Name->Length == 0 ||
        !(DirectoryFile->Header->Flags & FR_IS_DIRECTORY))
    {
        return STATUS_INVALID_PARAMETER;
    }
    Key.CaseSensitive = DirectoryFile->IsCaseSensitiveDirectory();
    return RemoveIndexEntry(DirectoryFile,
                            NtfsI30Name,
                            TypeFileName,
                            &Key,
                            FileReference);
}

NTSTATUS
Directory::RemoveIndexEntry(
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONG IndexedAttributeType,
    _In_ const IndexSearchKey* SearchKey,
    _In_ ULONGLONG FileReference)
{
    NTSTATUS Status = RemoveIndexEntryInner(DirectoryFile, IndexName, IndexedAttributeType,
                                            SearchKey, FileReference);

    NTFS_VERIFY_SHAPE_AFTER(Status, DiskVolume, DirectoryFile->RecordNumber, IndexName);
    return Status;
}

NTSTATUS
Directory::RemoveIndexEntryInner(
    _In_ PFileRecord DirectoryFile,
    _In_ PCWSTR IndexName,
    _In_ ULONG IndexedAttributeType,
    _In_ const IndexSearchKey* SearchKey,
    _In_ ULONGLONG FileReference)
{
    IndexSearchKey Key = *SearchKey;
    IndexSearchKey MaxSearchKey;
    const IndexSearchKey* ReleaseKey = NULL;
    UNICODE_STRING MaxName;
    PAttribute IndexRootAttribute;
    PIndexRootEx IndexRoot;
    PIndexEntry Matched;
    PIndexEntry Replacement = NULL;
    PIndexEntry MaxKey = NULL;
    PUCHAR NodeImage = NULL;
    PUCHAR RootValue = NULL;
    PUCHAR Scratch = NULL;
    ULONG MatchOffset;
    ULONG MaxKeyLength = 0;
    ULONG ReplacementLength = 0;
    ULONG IndexRecordSize;
    ULONG VisitedCount = 0;
    ULONGLONG ChildVcn;
    ULONGLONG RemovedChild;
    ULONGLONG VisitedVcns[64];
    ULONGLONG AllocationUnit;
    BOOLEAN Descend;
    BOOLEAN FoundExact = FALSE;
    BOOLEAN MatchedInRoot;
    BOOLEAN MatchedInternal;
    BOOLEAN MaxFound = FALSE;
    BOOLEAN LeafEmptied = FALSE;
    BOOLEAN EntryCommitted = FALSE;
    /* A keyless subtree under the removed entry: its blocks are released
     * only after the write that dropped the pointer to it is durable. */
    BOOLEAN ReleaseChain = FALSE;
    /* The pointer removal went through a root push-down or a split, so
     * it is spread over new blocks; a full flush makes it durable. */
    BOOLEAN CompoundWrite = FALSE;
    /* The node that gave up the replacement key, written after the
     * ancestor that took it (RemoveMaxFromSubtree). */
    DEFERRED_INDEX_WRITE Deferred = {};
    NTSTATUS TimestampStatus;
    NTSTATUS ReleaseStatus;
    NTSTATUS Status;

    if (!DiskVolume || !DirectoryFile ||
        !DirectoryFile->Header ||
        !DirectoryFile->Data ||
        DiskVolume->IsReadOnly)
    {
        return STATUS_INVALID_PARAMETER;
    }

    IndexRootAttribute =
        DirectoryFile->GetAttribute(
            TypeIndexRoot,
            const_cast<PWSTR>(IndexName));
    if (!IndexRootAttribute ||
        IndexRootAttribute->IsNonResident ||
        IndexRootAttribute->
            Resident.DataLength <
                FIELD_OFFSET(IndexRootEx,
                             Header) +
                sizeof(IndexNodeHeader))
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    IndexRoot =
        reinterpret_cast<PIndexRootEx>(
            GetResidentDataPointer(
                IndexRootAttribute));
    IndexRecordSize =
        BytesPerIndexRecord(DiskVolume);
    if (IndexRecordSize == 0 ||
        IndexRoot->AttributeType !=
            IndexedAttributeType ||
        IndexRoot->CollationRule !=
            Key.CollationRule)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    AllocationUnit =
        IndexRecordSize <
            BytesPerCluster(DiskVolume)
            ? DiskVolume->BytesPerSector
            : BytesPerCluster(DiskVolume);
    if (AllocationUnit == 0 ||
        IndexRecordSize % AllocationUnit != 0)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    Status = FindIndexInsertionPoint(
        DiskVolume,
        &IndexRoot->Header,
        IndexRootAttribute->
            Resident.DataLength -
            FIELD_OFFSET(IndexRootEx, Header),
        &Key,
        &MatchOffset,
        &ChildVcn,
        &Descend,
        &FoundExact);
    if (!NT_SUCCESS(Status))
        return Status;
    MatchedInRoot = FoundExact;

    NodeImage =
        new(PagedPool, TAG_BTREE)
            UCHAR[IndexRecordSize];
    if (!NodeImage)
        return STATUS_INSUFFICIENT_RESOURCES;
    Deferred.Image =
        new(PagedPool, TAG_BTREE)
            UCHAR[IndexRecordSize];
    if (!Deferred.Image)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    while (!FoundExact)
    {
        PIndexBuffer NodeBuffer;

        if (!Descend)
        {
            Status = STATUS_NOT_FOUND;
            goto Done;
        }
        if (VisitedCount ==
            RTL_NUMBER_OF(VisitedVcns))
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            goto Done;
        }
        for (ULONG Index = 0;
             Index < VisitedCount;
             Index++)
        {
            if (VisitedVcns[Index] == ChildVcn)
            {
                Status =
                    STATUS_FILE_CORRUPT_ERROR;
                goto Done;
            }
        }
        VisitedVcns[VisitedCount++] = ChildVcn;

        Status = ReadIndexNode(
            DiskVolume,
            DirectoryFile,
            IndexName,
            IndexRecordSize,
            AllocationUnit,
            ChildVcn,
            NodeImage);
        if (!NT_SUCCESS(Status))
            goto Done;
        NodeBuffer =
            reinterpret_cast<PIndexBuffer>(
                NodeImage);
        Descend = FALSE;
        Status = FindIndexInsertionPoint(
            DiskVolume,
            &NodeBuffer->IndexHeader,
            IndexRecordSize -
                FIELD_OFFSET(IndexBuffer,
                             IndexHeader),
            &Key,
            &MatchOffset,
            &ChildVcn,
            &Descend,
            &FoundExact);
        if (!NT_SUCCESS(Status))
            goto Done;
    }

    /* MatchOffset addresses the entry inside its owning node header. */
    Matched = MatchedInRoot
        ? reinterpret_cast<PIndexEntry>(
              reinterpret_cast<PUCHAR>(
                  &IndexRoot->Header) +
              MatchOffset)
        : reinterpret_cast<PIndexEntry>(
              reinterpret_cast<PUCHAR>(
                  &reinterpret_cast<PIndexBuffer>(
                      NodeImage)->IndexHeader) +
              MatchOffset);
    if (Key.CollationRule == ATTRDEF_COLLATION_FILENAME &&
        Matched->Data.Directory.IndexedFile !=
        FileReference)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Done;
    }
    MatchedInternal =
        !!(Matched->Flags & INDEX_ENTRY_NODE);
    RemovedChild = MatchedInternal
        ? *GetSubnodeVCN(Matched)
        : 0;

    if (!MatchedInternal)
    {
        /* Leaf removal: cut the entry where it stands. */
        if (MatchedInRoot)
        {
            ULONG RootPrefix =
                FIELD_OFFSET(IndexRootEx,
                             Header);
            ULONG ValueLength =
                IndexRootAttribute->
                    Resident.DataLength;

            RootValue =
                new(PagedPool, TAG_BTREE)
                    UCHAR[ValueLength];
            if (!RootValue)
            {
                Status =
                    STATUS_INSUFFICIENT_RESOURCES;
                goto Done;
            }
            RtlCopyMemory(RootValue,
                          IndexRoot,
                          ValueLength);
            CutEntryInNodeHeader(
                &reinterpret_cast<PIndexRootEx>(
                    RootValue)->Header,
                MatchOffset,
                Matched->EntryLength);
            reinterpret_cast<PIndexRootEx>(
                RootValue)->
                    Header.AllocatedSize =
                reinterpret_cast<PIndexRootEx>(
                    RootValue)->
                        Header.TotalIndexSize;
            Status = ReplaceIndexRootValue(
                DiskVolume,
                DirectoryFile,
                IndexName,
                RootValue,
                RootPrefix +
                    reinterpret_cast<
                        PIndexRootEx>(
                            RootValue)->
                        Header.TotalIndexSize);
        }
        else
        {
            PIndexBuffer NodeBuffer =
                reinterpret_cast<PIndexBuffer>(
                    NodeImage);

            CutEntryInNodeHeader(
                &NodeBuffer->IndexHeader,
                MatchOffset,
                Matched->EntryLength);
            if (MatchOffset ==
                    NodeBuffer->IndexHeader.IndexOffset &&
                (Matched->Flags & INDEX_ENTRY_END))
            {
                ReleaseKey = &Key;
            }
            Status = WriteIndexNode(
                DiskVolume,
                DirectoryFile,
                IndexName,
                VisitedVcns[VisitedCount - 1],
                AllocationUnit,
                IndexRecordSize,
                NodeImage);
        }
        if (!NT_SUCCESS(Status))
            goto Done;
        EntryCommitted = TRUE;
        goto TouchDirectory;
    }

    /*
     * Internal removal: pull the collation maximum out of the entry's
     * subtree as the replacement key. A keyless subtree is released
     * whole and the entry simply disappears.
     */
    Status = RemoveMaxFromSubtree(
        DiskVolume,
        DirectoryFile,
        IndexName,
        IndexRecordSize,
        AllocationUnit,
        RemovedChild,
        0,
        &MaxKey,
        &MaxKeyLength,
        &MaxFound,
        &LeafEmptied,
        &Deferred);
    if (!NT_SUCCESS(Status))
        goto Done;
    if (MaxFound)
    {
        if (LeafEmptied)
        {
            MakeEntrySearchKey(Key.CollationRule,
                               MaxKey,
                               &MaxName,
                               &MaxSearchKey);
            MaxSearchKey.CaseSensitive = Key.CaseSensitive;
            ReleaseKey = &MaxSearchKey;
        }
        Status = BuildPromotedEntry(
            reinterpret_cast<PUCHAR>(MaxKey),
            MaxKeyLength,
            FALSE,
            RemovedChild,
            &Replacement,
            &ReplacementLength);
        if (!NT_SUCCESS(Status))
            goto Done;
    }
    else
    {
        /*
         * The subtree holds no key: the entry simply disappears below,
         * and the chain's bits are released once that write is durable
         * (see ReleaseChain before EntryCommitted).
         */
        ReleaseChain = TRUE;
        ReleaseKey = &Key;
    }

    if (MatchedInRoot)
    {
        ULONG ListBytes;
        ULONG ScratchCapacity =
            IndexRecordSize + 4096;

        /*
         * The subtree writes above may have moved record attributes;
         * refresh the root and relocate the matched entry.
         */
        IndexRootAttribute =
            DirectoryFile->GetAttribute(
                TypeIndexRoot,
                const_cast<PWSTR>(IndexName));
        if (!IndexRootAttribute ||
            IndexRootAttribute->IsNonResident)
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            goto Done;
        }
        IndexRoot =
            reinterpret_cast<PIndexRootEx>(
                GetResidentDataPointer(
                    IndexRootAttribute));
        Status = FindChildLinkOffset(
            &IndexRoot->Header,
            RemovedChild,
            &MatchOffset,
            FALSE);
        if (!NT_SUCCESS(Status))
            goto Done;
        Matched =
            reinterpret_cast<PIndexEntry>(
                reinterpret_cast<PUCHAR>(
                    &IndexRoot->Header) +
                MatchOffset);

        Scratch =
            new(PagedPool, TAG_BTREE)
                UCHAR[ScratchCapacity];
        if (!Scratch)
        {
            Status =
                STATUS_INSUFFICIENT_RESOURCES;
            goto Done;
        }
        {
            ULONG ListStart =
                IndexRoot->Header.IndexOffset;
            ULONG ListEnd =
                IndexRoot->Header.TotalIndexSize;
            ULONG Prefix =
                MatchOffset - ListStart;
            ULONG Tail =
                ListEnd - MatchOffset -
                Matched->EntryLength;

            if (ListEnd - ListStart -
                    Matched->EntryLength +
                    ReplacementLength >
                ScratchCapacity)
            {
                Status =
                    STATUS_FILE_CORRUPT_ERROR;
                goto Done;
            }
            RtlCopyMemory(
                Scratch,
                reinterpret_cast<PUCHAR>(
                    &IndexRoot->Header) +
                    ListStart,
                Prefix);
            if (Replacement)
            {
                RtlCopyMemory(
                    Scratch + Prefix,
                    Replacement,
                    ReplacementLength);
            }
            RtlCopyMemory(
                Scratch + Prefix +
                    ReplacementLength,
                reinterpret_cast<PUCHAR>(
                    &IndexRoot->Header) +
                    MatchOffset +
                    Matched->EntryLength,
                Tail);
            ListBytes =
                Prefix + ReplacementLength +
                Tail;
        }

        Status = BuildRootValueFromList(
            IndexRoot,
            Scratch,
            ListBytes,
            TRUE,
            &RootValue,
            &ReplacementLength /* reused */);
        if (!NT_SUCCESS(Status))
            goto Done;
        Status = RootGrowthKeepsListRoom(DirectoryFile, IndexName, ReplacementLength)
            ? ReplaceIndexRootValue(
                  DiskVolume,
                  DirectoryFile,
                  IndexName,
                  RootValue,
                  ReplacementLength)
            : STATUS_BUFFER_TOO_SMALL;
        if (Status == STATUS_BUFFER_TOO_SMALL)
        {
            Status = PushDownRoot(
                DiskVolume,
                DirectoryFile,
                IndexName,
                IndexRoot,
                Scratch,
                ListBytes,
                TRUE,
                IndexRecordSize,
                AllocationUnit);
            CompoundWrite = TRUE;
        }
        if (!NT_SUCCESS(Status))
            goto Done;
    }
    else
    {
        PIndexBuffer NodeBuffer =
            reinterpret_cast<PIndexBuffer>(
                NodeImage);

        CutEntryInNodeHeader(
            &NodeBuffer->IndexHeader,
            MatchOffset,
            Matched->EntryLength);
        if (Replacement)
        {
            Status = SplitAndPromote(
                DiskVolume,
                DirectoryFile,
                IndexName,
                FALSE,
                IndexRecordSize,
                AllocationUnit,
                VisitedVcns,
                VisitedCount,
                NodeImage,
                MatchOffset,
                Replacement,
                ReplacementLength);
            CompoundWrite = TRUE;
        }
        else
        {
            Status = WriteIndexNode(
                DiskVolume,
                DirectoryFile,
                IndexName,
                VisitedVcns[VisitedCount - 1],
                AllocationUnit,
                IndexRecordSize,
                NodeImage);
        }
        if (!NT_SUCCESS(Status))
            goto Done;
    }

    /*
     * The ancestor now carries the replacement key. Write the node that
     * gave it up, ordered after the ancestor (a compound write spread the
     * ancestor over new blocks: full flush), so a cut can duplicate the
     * key but never lose it.
     */
    if (Deferred.Valid)
    {
        if (CompoundWrite)
        {
            Status = DiskVolume->FlushVolume();
        }
        else if (MatchedInRoot)
        {
            IndexRootAttribute =
                DirectoryFile->GetAttribute(
                    TypeIndexRoot,
                    const_cast<PWSTR>(IndexName));
            Status = OrderNodeAfterRecord(
                DiskVolume,
                DirectoryFile,
                IndexName,
                Deferred.Vcn,
                AllocationUnit,
                IndexRecordSize,
                IndexRootAttribute
                    ? DirectoryFile->GetAttributeOwner(
                        IndexRootAttribute)
                    : NULL);
        }
        else
        {
            Status = OrderNodeAfterIndexNodes(
                DiskVolume,
                DirectoryFile,
                IndexName,
                Deferred.Vcn,
                &VisitedVcns[VisitedCount - 1],
                1,
                AllocationUnit,
                IndexRecordSize);
        }
        if (NT_SUCCESS(Status))
        {
            Status = WriteIndexNode(
                DiskVolume,
                DirectoryFile,
                IndexName,
                Deferred.Vcn,
                AllocationUnit,
                IndexRecordSize,
                Deferred.Image);
        }
        if (!NT_SUCCESS(Status))
        {
            /*
             * The ancestor already carries the key: a duplicate at worst,
             * never a loss. The node still links its chain, so that stays
             * allocated.
             */
            NoteIndexDamage(
                DiskVolume,
                "deferred node write",
                Deferred.Vcn,
                Status);
            Deferred.ChainValid = FALSE;
            Status = STATUS_SUCCESS;
        }
    }

    /*
     * A keyless END-only chain was taken out of the tree above, either by
     * the entry removal itself (ReleaseChain) or by the node that donated
     * its key (Deferred.ChainValid). The write that dropped the pointer to
     * it is in memory; make it durable before the chain's bits are cleared
     * (CommitIndexPointerHolder), since a cleared bit lets the next split
     * reuse a block at once. A failure leaves blocks whose bits stayed set,
     * a leak the next check repairs (NoteIndexDamage); the removal itself
     * has happened and stays successful.
     */
    if (ReleaseChain || Deferred.ChainValid)
    {
        if (Deferred.ChainValid)
        {
            Status = CommitIndexPointerHolder(
                DiskVolume,
                DirectoryFile,
                IndexName,
                AllocationUnit,
                IndexRecordSize,
                NULL,
                Deferred.Vcn);
        }
        else if (CompoundWrite)
        {
            Status = DiskVolume->FlushVolume();
        }
        else if (MatchedInRoot)
        {
            IndexRootAttribute =
                DirectoryFile->GetAttribute(
                    TypeIndexRoot,
                    const_cast<PWSTR>(IndexName));
            Status = CommitIndexPointerHolder(
                DiskVolume,
                DirectoryFile,
                IndexName,
                AllocationUnit,
                IndexRecordSize,
                IndexRootAttribute
                    ? DirectoryFile->GetAttributeOwner(
                        IndexRootAttribute)
                    : NULL,
                0);
        }
        else
        {
            Status = CommitIndexPointerHolder(
                DiskVolume,
                DirectoryFile,
                IndexName,
                AllocationUnit,
                IndexRecordSize,
                NULL,
                VisitedVcns[VisitedCount - 1]);
        }
        if (NT_SUCCESS(Status))
        {
            Status = ReleaseEndOnlyChain(
                DiskVolume,
                DirectoryFile,
                IndexName,
                IndexRecordSize,
                AllocationUnit,
                Deferred.ChainValid
                    ? Deferred.ChainVcn
                    : RemovedChild);
        }
        if (!NT_SUCCESS(Status))
        {
            NoteIndexDamage(
                DiskVolume,
                "chain release",
                Deferred.ChainValid
                    ? Deferred.ChainVcn
                    : RemovedChild,
                Status);
            Status = STATUS_SUCCESS;
        }
    }
    EntryCommitted = TRUE;

TouchDirectory:
    if (EntryCommitted)
    {
        ReleaseStatus = STATUS_SUCCESS;
        if (ReleaseKey)
        {
            ReleaseStatus = ReleaseEmptyIndexNode(
                DirectoryFile,
                IndexName,
                IndexedAttributeType,
                ReleaseKey,
                IndexRecordSize,
                AllocationUnit);
            if (!NT_SUCCESS(ReleaseStatus))
            {
                DPRINT1(
                    "Index entry was removed but "
                    "the emptied index buffer was "
                    "not released: 0x%lx.\n",
                    ReleaseStatus);
            }
        }
        TimestampStatus =
            Key.CollationRule == ATTRDEF_COLLATION_FILENAME
            ? DirectoryFile->TouchDirectory()
            : STATUS_SUCCESS;
        if (!NT_SUCCESS(TimestampStatus))
        {
            DPRINT1(
                "Directory entry was removed but "
                "timestamp update failed: 0x%lx.\n",
                TimestampStatus);
        }
        /*
         * The entry is gone from the index either way; the caller must see
         * success, or it aborts a removal that has already happened. A
         * failed release is reported through the dirty flag instead.
         */
        if (!NT_SUCCESS(ReleaseStatus))
        {
            NoteIndexDamage(
                DiskVolume,
                "empty node release",
                0,
                ReleaseStatus);
        }
        Status = STATUS_SUCCESS;
    }

Done:
    if (MaxKey)
        NtfsFreePool(MaxKey);
    if (Replacement)
        NtfsFreePool(Replacement);
    delete[] Scratch;
    delete[] RootValue;
    delete[] Deferred.Image;
    delete[] NodeImage;
    return Status;
}
