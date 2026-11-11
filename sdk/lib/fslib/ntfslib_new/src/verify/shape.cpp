/*
 * PROJECT:     LiberNT NTFS library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Record and index shape invariants, read back from the disk
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "ntfslib_new.h"
#include "ntfslib_new_internal.h"

/*
 * Debug-only consistency checks of what a mutation left on the disk. They
 * read the records and index blocks back through the volume, not through
 * the cached objects, so they see what a remount would see.
 *
 * Record shape, for a base record:
 *   - an attribute list, when present, parses, names only records that are
 *     in use and belong to this base, and each entry matches an attribute
 *     there; every attribute of the base and of those records is listed;
 *   - with NTFS_VERIFY_SCAN_EXTENSIONS, no in-use extension record names
 *     this base unless the list names it (no list means no extension);
 *   - the mapping pairs of every non-resident attribute cover exactly its
 *     allocation: segments from VCN 0, contiguous, each one's runs as long
 *     as its VCN range.
 * Index shape, for one index of a base record:
 *   - the root parses; a child pointer needs an allocation;
 *   - every referenced block lies inside the allocation, is marked in use
 *     in the index bitmap, is a valid INDX block for that VCN, and is
 *     referenced once;
 *   - every block marked in use is referenced (no unreachable child).
 */

#define SHAPE_MAX_RECORDS   256
#define SHAPE_MAX_DEPTH     32
#define SHAPE_LIST_LIMIT    (16 * 1024 * 1024)
#define SHAPE_HOOK_SCAN_RECORDS 8192
#define SHAPE_HOOK_INDEX_BYTES  (4 * 1024 * 1024)

namespace
{

struct ShapeRun
{
    ULONGLONG Vcn;
    ULONGLONG Lcn;
    ULONGLONG Length;
    BOOLEAN Sparse;
};

struct ShapeSegment
{
    PAttribute Attr;
    ULONGLONG Record;
};

class ShapeVerifier
{
public:
    ShapeVerifier(_In_ PVolume Volume, _Out_ PNtfsShapeFault Out)
        : V(Volume), Fault(Out)
    {
        RtlZeroMemory(Fault, sizeof(*Fault));
    }

    ~ShapeVerifier()
    {
        for (ULONG Index = 0; Index < Count; Index++)
            delete[] Buffers[Index];
        delete[] ListData;
    }

    NTSTATUS VerifyRecord(_In_ ULONGLONG Base, _In_ ULONG Flags);
    NTSTATUS VerifyIndex(_In_ ULONGLONG Base, _In_ PCWSTR IndexName, _In_ ULONG Flags);
    NTSTATUS VerifyVolume(_In_ ULONG Flags, _Out_ PNtfsShapeVolumeStats Stats);

    /* Indexes with more allocation than this are not walked (0: no limit);
     * IndexSkipped tells that the last VerifyIndex skipped its walk. */
    ULONGLONG IndexByteLimit = 0;
    BOOLEAN IndexSkipped = FALSE;

private:
    PVolume V;
    PNtfsShapeFault Fault;
    ULONG RecordSize = 0;
    ULONG ClusterSize = 0;
    ULONGLONG RecordCount = 0;
    ULONGLONG BaseNumber = 0;
    ULONGLONG BaseReference = 0;
    PUCHAR Buffers[SHAPE_MAX_RECORDS] = {};
    ULONGLONG Numbers[SHAPE_MAX_RECORDS] = {};
    ULONG Count = 0;
    PUCHAR ListData = NULL;
    ULONG ListLength = 0;

    NTSTATUS Fail(_In_ NtfsShapeInvariant Invariant, _In_ ULONGLONG Record, _In_ ULONGLONG Detail)
    {
        Fault->Invariant = Invariant;
        Fault->Record = Record;
        Fault->Detail = Detail;
        return STATUS_FILE_CORRUPT_ERROR;
    }

    NTSTATUS Setup();
    NTSTATUS ReadRecord(_In_ ULONGLONG Number, _Out_ PUCHAR Buffer);
    PAttribute NextAttribute(_In_ PUCHAR Record, _Inout_ PULONG Offset, _Out_ PBOOLEAN Malformed);
    NTSTATUS LoadFile(_In_ ULONGLONG Base);
    NTSTATUS LoadList(_In_ PAttribute List);
    NTSTATUS CheckList();
    NTSTATUS ScanExtensions();
    NTSTATUS CheckMappings();
    NTSTATUS DecodeRuns(_In_ PAttribute Attr, _Inout_ ShapeRun** Runs, _Inout_ PULONG RunCount,
                        _Inout_ PULONG RunCapacity, _Out_ PULONGLONG Clusters);
    NTSTATUS ReadStream(_In_ ShapeRun* Runs, _In_ ULONG RunCount, _In_ ULONGLONG Offset,
                        _In_ ULONG Length, _Out_ PUCHAR Buffer);
    BOOLEAN IsListed(_In_ ULONGLONG Record, _In_ PAttribute Attr);
    ULONG CollectSegments(_In_ ULONG Type, _In_ PCWSTR Name, _In_ UCHAR NameLength,
                          _Out_ ShapeSegment* Segments, _In_ ULONG Capacity);
    NTSTATUS CheckNode(_In_ PIndexNodeHeader Node, _In_ ULONG Available, _In_ ULONGLONG Record,
                       _In_ ULONGLONG Vcn, _Inout_ ULONGLONG* Stack, _Inout_ PULONG Depth,
                       _In_ ULONG Level);

    static BOOLEAN SameName(_In_ PCWSTR Left, _In_ UCHAR LeftLength, _In_ PCWSTR Right, _In_ UCHAR RightLength)
    {
        if (LeftLength != RightLength)
            return FALSE;
        return RtlCompareMemory(Left, Right, LeftLength * sizeof(WCHAR)) == LeftLength * sizeof(WCHAR);
    }

    static PCWSTR AttrName(_In_ PAttribute Attr)
    {
        return reinterpret_cast<PCWSTR>(reinterpret_cast<PUCHAR>(Attr) + Attr->NameOffset);
    }
};

NTSTATUS
ShapeVerifier::Setup()
{
    NtfsVolumeInformation Information;
    NTSTATUS Status = V->QueryInformation(&Information);

    if (!NT_SUCCESS(Status))
        return Status;
    RecordSize = Information.BytesPerFileRecordSegment;
    ClusterSize = Information.BytesPerCluster;
    RecordCount = RecordSize ? Information.MftValidDataLength / RecordSize : 0;
    if (!RecordSize || !ClusterSize || RecordSize % V->BytesPerSector)
        return STATUS_FILE_CORRUPT_ERROR;
    return STATUS_SUCCESS;
}

/* The record as on disk, fixups applied; STATUS_NOT_FOUND when not in use. */
NTSTATUS
ShapeVerifier::ReadRecord(_In_ ULONGLONG Number, _Out_ PUCHAR Buffer)
{
    PFileRecordHeader Header = reinterpret_cast<PFileRecordHeader>(Buffer);
    ULONGLONG DiskOffset;
    NTSTATUS Status;

    Status = V->MFT->GetFileRecordDiskOffset(Number, &DiskOffset);
    if (NT_SUCCESS(Status))
        Status = V->ReadVolume(DiskOffset, RecordSize, Buffer);
    if (!NT_SUCCESS(Status))
        return Status;
    if (RtlCompareMemory(Header->Header.TypeID, "FILE", 4) != 4)
        return STATUS_NOT_FOUND;
    Status = NtfsApplyFixup(&Header->Header, RecordSize, V->BytesPerSector);
    if (!NT_SUCCESS(Status))
        return Status;
    if (!(Header->Flags & FR_IN_USE))
        return STATUS_NOT_FOUND;
    if (Header->AttributeOffset < sizeof(FileRecordHeader) ||
        Header->ActualSize > Header->AllocatedSize ||
        Header->AllocatedSize > RecordSize ||
        Header->AttributeOffset >= Header->ActualSize)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    return STATUS_SUCCESS;
}

PAttribute
ShapeVerifier::NextAttribute(_In_ PUCHAR Record, _Inout_ PULONG Offset, _Out_ PBOOLEAN Malformed)
{
    PFileRecordHeader Header = reinterpret_cast<PFileRecordHeader>(Record);
    PAttribute Attr;

    *Malformed = FALSE;
    if (*Offset == 0)
        *Offset = Header->AttributeOffset;
    if (*Offset + 4 > Header->ActualSize)
    {
        *Malformed = TRUE;
        return NULL;
    }
    Attr = reinterpret_cast<PAttribute>(Record + *Offset);
    if (Attr->AttributeType == TypeAttributeEndMarker)
        return NULL;
    if (*Offset + 0x18 > Header->ActualSize || Attr->Length < 0x18 || (Attr->Length & 7) ||
        Attr->Length > Header->ActualSize - *Offset ||
        (ULONG)Attr->NameOffset + Attr->NameLength * sizeof(WCHAR) > Attr->Length ||
        (Attr->IsNonResident
             ? (Attr->Length < 0x40 || Attr->NonResident.DataRunsOffset >= Attr->Length)
             : ((ULONG)Attr->Resident.DataOffset + Attr->Resident.DataLength > Attr->Length)))
    {
        *Malformed = TRUE;
        return NULL;
    }
    *Offset += Attr->Length;
    return Attr;
}

NTSTATUS
ShapeVerifier::DecodeRuns(_In_ PAttribute Attr,
                          _Inout_ ShapeRun** Runs,
                          _Inout_ PULONG RunCount,
                          _Inout_ PULONG RunCapacity,
                          _Out_ PULONGLONG Clusters)
{
    PUCHAR Pairs = reinterpret_cast<PUCHAR>(Attr) + Attr->NonResident.DataRunsOffset;
    PUCHAR End = reinterpret_cast<PUCHAR>(Attr) + Attr->Length;
    ULONGLONG Vcn = Attr->NonResident.FirstVCN;
    LONGLONG Lcn = 0;

    *Clusters = 0;
    while (Pairs < End && *Pairs)
    {
        ULONG LengthBytes = *Pairs & 0x0F;
        ULONG OffsetBytes = *Pairs >> 4;
        ULONGLONG Length = 0;
        LONGLONG Delta = 0;

        if (!LengthBytes || LengthBytes > 8 || OffsetBytes > 8 ||
            Pairs + 1 + LengthBytes + OffsetBytes > End)
        {
            return STATUS_FILE_CORRUPT_ERROR;
        }
        for (ULONG Index = 0; Index < LengthBytes; Index++)
            Length |= (ULONGLONG)Pairs[1 + Index] << (8 * Index);
        if ((LONGLONG)Length <= 0)
            return STATUS_FILE_CORRUPT_ERROR;
        for (ULONG Index = 0; Index < OffsetBytes; Index++)
            Delta |= (LONGLONG)Pairs[1 + LengthBytes + Index] << (8 * Index);
        if (OffsetBytes && OffsetBytes < 8 && (Pairs[LengthBytes + OffsetBytes] & 0x80))
            Delta |= -((LONGLONG)1 << (8 * OffsetBytes));
        Lcn += Delta;

        if (Runs)
        {
            if (*RunCount == *RunCapacity)
            {
                ULONG Capacity = *RunCapacity ? *RunCapacity * 2 : 16;
                ShapeRun* Grown = new(PagedPool, TAG_NTFS) ShapeRun[Capacity];

                if (!Grown)
                    return STATUS_INSUFFICIENT_RESOURCES;
                if (*Runs)
                    RtlCopyMemory(Grown, *Runs, *RunCount * sizeof(ShapeRun));
                delete[] *Runs;
                *Runs = Grown;
                *RunCapacity = Capacity;
            }
            (*Runs)[*RunCount].Vcn = Vcn;
            (*Runs)[*RunCount].Lcn = (ULONGLONG)Lcn;
            (*Runs)[*RunCount].Length = Length;
            (*Runs)[*RunCount].Sparse = OffsetBytes == 0;
            (*RunCount)++;
        }
        if (OffsetBytes && (Lcn < 0 || (ULONGLONG)Lcn + Length > V->ClustersInVolume))
            return STATUS_FILE_CORRUPT_ERROR;
        Vcn += Length;
        *Clusters += Length;
        Pairs += 1 + LengthBytes + OffsetBytes;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
ShapeVerifier::ReadStream(_In_ ShapeRun* Runs,
                          _In_ ULONG RunCount,
                          _In_ ULONGLONG Offset,
                          _In_ ULONG Length,
                          _Out_ PUCHAR Buffer)
{
    while (Length)
    {
        ULONGLONG Vcn = Offset / ClusterSize;
        ULONG Within = (ULONG)(Offset % ClusterSize);
        ULONG Chunk = min(Length, ClusterSize - Within);
        ShapeRun* Run = NULL;
        NTSTATUS Status;

        for (ULONG Index = 0; Index < RunCount; Index++)
        {
            if (Vcn >= Runs[Index].Vcn && Vcn - Runs[Index].Vcn < Runs[Index].Length)
            {
                Run = &Runs[Index];
                break;
            }
        }
        if (!Run)
            return STATUS_FILE_CORRUPT_ERROR;
        if (Run->Sparse)
        {
            RtlZeroMemory(Buffer, Chunk);
        }
        else
        {
            Status = V->ReadVolume((Run->Lcn + (Vcn - Run->Vcn)) * ClusterSize + Within, Chunk, Buffer);
            if (!NT_SUCCESS(Status))
                return Status;
        }
        Buffer += Chunk;
        Offset += Chunk;
        Length -= Chunk;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
ShapeVerifier::LoadList(_In_ PAttribute List)
{
    ShapeRun* Runs = NULL;
    ULONG RunCount = 0, RunCapacity = 0;
    ULONGLONG Clusters;
    NTSTATUS Status;

    if (!List->IsNonResident)
    {
        ListLength = List->Resident.DataLength;
        ListData = new(PagedPool, TAG_NTFS) UCHAR[ListLength ? ListLength : 1];
        if (!ListData)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlCopyMemory(ListData, reinterpret_cast<PUCHAR>(List) + List->Resident.DataOffset, ListLength);
        return STATUS_SUCCESS;
    }
    if (List->NonResident.FirstVCN != 0 || List->NonResident.DataSize > SHAPE_LIST_LIMIT ||
        List->NonResident.DataSize > List->NonResident.AllocatedSize)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    Status = DecodeRuns(List, &Runs, &RunCount, &RunCapacity, &Clusters);
    if (NT_SUCCESS(Status) && Clusters * ClusterSize < List->NonResident.AllocatedSize)
        Status = STATUS_FILE_CORRUPT_ERROR;
    if (NT_SUCCESS(Status))
    {
        ListLength = (ULONG)List->NonResident.DataSize;
        ListData = new(PagedPool, TAG_NTFS) UCHAR[ListLength ? ListLength : 1];
        Status = ListData ? ReadStream(Runs, RunCount, 0, ListLength, ListData)
                          : STATUS_INSUFFICIENT_RESOURCES;
    }
    delete[] Runs;
    return Status;
}

/* Reads the base record, its attribute list and every record the list names. */
NTSTATUS
ShapeVerifier::LoadFile(_In_ ULONGLONG Base)
{
    PFileRecordHeader Header;
    PAttribute Attr, List = NULL;
    ULONG Offset = 0;
    BOOLEAN Malformed;
    NTSTATUS Status;

    Status = Setup();
    if (!NT_SUCCESS(Status))
        return Status;
    BaseNumber = Base;
    Buffers[0] = new(PagedPool, TAG_NTFS) UCHAR[RecordSize];
    if (!Buffers[0])
        return STATUS_INSUFFICIENT_RESOURCES;
    Numbers[0] = Base;
    Count = 1;
    Status = ReadRecord(Base, Buffers[0]);
    if (!NT_SUCCESS(Status))
        return Fail(NtfsShapeRecordUnreadable, Base, (ULONG)Status);
    Header = reinterpret_cast<PFileRecordHeader>(Buffers[0]);
    if (Header->BaseFileRecord != 0)
        return Fail(NtfsShapeNotBaseRecord, Base, Header->BaseFileRecord);
    BaseReference = Base | ((ULONGLONG)Header->SequenceNumber << 48);

    while ((Attr = NextAttribute(Buffers[0], &Offset, &Malformed)) != NULL)
    {
        if (Attr->AttributeType == TypeAttributeList)
            List = Attr;
    }
    if (Malformed)
        return Fail(NtfsShapeRecordMalformed, Base, Offset);
    if (!List)
        return STATUS_SUCCESS;

    Status = LoadList(List);
    if (!NT_SUCCESS(Status))
        return Fail(NtfsShapeListUnreadable, Base, (ULONG)Status);

    for (ULONG Position = 0; Position < ListLength;)
    {
        PAttributeListEx Entry = reinterpret_cast<PAttributeListEx>(ListData + Position);
        ULONGLONG Named;
        BOOLEAN Known = FALSE;

        if (ListLength - Position < FIELD_OFFSET(AttributeListEx, Padding) ||
            Entry->RecordLength < FIELD_OFFSET(AttributeListEx, Padding) ||
            (Entry->RecordLength & 7) || Entry->RecordLength > ListLength - Position ||
            (ULONG)Entry->NameOffset + Entry->NameLength * sizeof(WCHAR) > Entry->RecordLength)
        {
            return Fail(NtfsShapeListEntryBad, Base, Position);
        }
        Named = Entry->BaseFileRef & 0x0000FFFFFFFFFFFFULL;
        for (ULONG Index = 0; Index < Count; Index++)
            Known |= Numbers[Index] == Named;
        if (!Known)
        {
            PFileRecordHeader Extension;

            if (Count == SHAPE_MAX_RECORDS)
                return Fail(NtfsShapeListEntryBad, Base, Position);
            Buffers[Count] = new(PagedPool, TAG_NTFS) UCHAR[RecordSize];
            if (!Buffers[Count])
                return STATUS_INSUFFICIENT_RESOURCES;
            Numbers[Count] = Named;
            Count++;
            Status = Named < RecordCount ? ReadRecord(Named, Buffers[Count - 1]) : STATUS_NOT_FOUND;
            if (!NT_SUCCESS(Status))
                return Fail(NtfsShapeListEntryMissing, Named, Entry->Type);
            Extension = reinterpret_cast<PFileRecordHeader>(Buffers[Count - 1]);
            if (Extension->BaseFileRecord != BaseReference ||
                ((ULONGLONG)Extension->SequenceNumber << 48 | Named) != Entry->BaseFileRef)
            {
                return Fail(NtfsShapeListEntryMissing, Named, Entry->Type);
            }
        }
        Position += Entry->RecordLength;
    }
    return STATUS_SUCCESS;
}

/* Whether an attribute of a loaded record has its list entry. */
BOOLEAN
ShapeVerifier::IsListed(_In_ ULONGLONG Record, _In_ PAttribute Attr)
{
    ULONGLONG FirstVcn = Attr->IsNonResident ? Attr->NonResident.FirstVCN : 0;

    for (ULONG Position = 0; Position < ListLength;)
    {
        PAttributeListEx Entry = reinterpret_cast<PAttributeListEx>(ListData + Position);

        if ((Entry->BaseFileRef & 0x0000FFFFFFFFFFFFULL) == Record &&
            Entry->Type == Attr->AttributeType && Entry->AttributeId == Attr->AttributeID &&
            Entry->FirstVCN == FirstVcn &&
            SameName(reinterpret_cast<PCWSTR>(ListData + Position + Entry->NameOffset), Entry->NameLength,
                     AttrName(Attr), Attr->NameLength))
        {
            return TRUE;
        }
        Position += Entry->RecordLength;
    }
    return FALSE;
}

/* The list and the attributes of the records it names agree both ways. */
NTSTATUS
ShapeVerifier::CheckList()
{
    /* Without a list, all attributes are in the base record. */
    if (!ListData)
        return STATUS_SUCCESS;

    for (ULONG Position = 0; Position < ListLength;)
    {
        PAttributeListEx Entry = reinterpret_cast<PAttributeListEx>(ListData + Position);
        ULONGLONG Named = Entry->BaseFileRef & 0x0000FFFFFFFFFFFFULL;
        BOOLEAN Found = FALSE;

        for (ULONG Index = 0; Index < Count && !Found; Index++)
        {
            ULONG Offset = 0;
            BOOLEAN Malformed;
            PAttribute Attr;

            if (Numbers[Index] != Named)
                continue;
            while (!Found && (Attr = NextAttribute(Buffers[Index], &Offset, &Malformed)) != NULL)
            {
                Found = Attr->AttributeType == Entry->Type && Attr->AttributeID == Entry->AttributeId &&
                        (Attr->IsNonResident ? Attr->NonResident.FirstVCN : 0) == Entry->FirstVCN &&
                        SameName(AttrName(Attr), Attr->NameLength,
                                 reinterpret_cast<PCWSTR>(ListData + Position + Entry->NameOffset),
                                 Entry->NameLength);
            }
            if (Malformed)
                return Fail(NtfsShapeRecordMalformed, Named, Offset);
        }
        if (!Found)
            return Fail(NtfsShapeListEntryMissing, Named, Entry->Type);
        Position += Entry->RecordLength;
    }

    for (ULONG Index = 0; Index < Count; Index++)
    {
        ULONG Offset = 0;
        BOOLEAN Malformed;
        PAttribute Attr;

        while ((Attr = NextAttribute(Buffers[Index], &Offset, &Malformed)) != NULL)
        {
            if (Attr->AttributeType != TypeAttributeList && !IsListed(Numbers[Index], Attr))
                return Fail(NtfsShapeAttributeNotListed, Numbers[Index], Attr->AttributeType);
        }
        if (Malformed)
            return Fail(NtfsShapeRecordMalformed, Numbers[Index], Offset);
    }
    return STATUS_SUCCESS;
}

/* Every in-use extension record of the base is named by its list. */
NTSTATUS
ShapeVerifier::ScanExtensions()
{
    PUCHAR Record = new(PagedPool, TAG_NTFS) UCHAR[RecordSize];
    PFileRecordHeader Header = reinterpret_cast<PFileRecordHeader>(Record);
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Record)
        return STATUS_INSUFFICIENT_RESOURCES;
    for (ULONGLONG Number = 0; Number < RecordCount && NT_SUCCESS(Status); Number++)
    {
        BOOLEAN Named = FALSE;

        if (Number == BaseNumber || !NT_SUCCESS(ReadRecord(Number, Record)) ||
            (Header->BaseFileRecord & 0x0000FFFFFFFFFFFFULL) != BaseNumber)
        {
            continue;
        }
        for (ULONG Index = 1; Index < Count; Index++)
            Named |= Numbers[Index] == Number;
        if (!ListData)
            Status = Fail(NtfsShapeExtensionWithoutList, Number, BaseNumber);
        else if (!Named)
            Status = Fail(NtfsShapeExtensionNotListed, Number, BaseNumber);
    }
    delete[] Record;
    return Status;
}

ULONG
ShapeVerifier::CollectSegments(_In_ ULONG Type,
                               _In_ PCWSTR Name,
                               _In_ UCHAR NameLength,
                               _Out_ ShapeSegment* Segments,
                               _In_ ULONG Capacity)
{
    ULONG Found = 0;

    for (ULONG Index = 0; Index < Count; Index++)
    {
        ULONG Offset = 0;
        BOOLEAN Malformed;
        PAttribute Attr;

        while ((Attr = NextAttribute(Buffers[Index], &Offset, &Malformed)) != NULL)
        {
            if (Attr->IsNonResident && Attr->AttributeType == Type &&
                SameName(AttrName(Attr), Attr->NameLength, Name, NameLength))
            {
                if (Found < Capacity)
                {
                    Segments[Found].Attr = Attr;
                    Segments[Found].Record = Numbers[Index];
                }
                Found++;
            }
        }
    }
    /* Order by first VCN (few segments: insertion sort). */
    for (ULONG Index = 1; Index < min(Found, Capacity); Index++)
    {
        ShapeSegment Key = Segments[Index];
        ULONG Slot = Index;

        while (Slot && Segments[Slot - 1].Attr->NonResident.FirstVCN > Key.Attr->NonResident.FirstVCN)
        {
            Segments[Slot] = Segments[Slot - 1];
            Slot--;
        }
        Segments[Slot] = Key;
    }
    return Found;
}

/* The mapping pairs of each non-resident attribute cover its allocation. */
NTSTATUS
ShapeVerifier::CheckMappings()
{
    ShapeSegment Segments[SHAPE_MAX_RECORDS];

    for (ULONG Index = 0; Index < Count; Index++)
    {
        ULONG Offset = 0;
        BOOLEAN Malformed;
        PAttribute Attr;

        while ((Attr = NextAttribute(Buffers[Index], &Offset, &Malformed)) != NULL)
        {
            ULONGLONG NextVcn = 0, Allocated, Clusters;
            ULONG Found;
            NTSTATUS Status;

            /* Each attribute once: from its VCN 0 segment. */
            if (!Attr->IsNonResident || Attr->NonResident.FirstVCN != 0)
                continue;
            Found = CollectSegments(Attr->AttributeType, AttrName(Attr), Attr->NameLength,
                                    Segments, RTL_NUMBER_OF(Segments));
            if (Found > RTL_NUMBER_OF(Segments))
                return Fail(NtfsShapeMappingGap, Numbers[Index], Attr->AttributeType);
            Allocated = Attr->NonResident.AllocatedSize;
            if (Allocated % ClusterSize || Attr->NonResident.DataSize > Allocated)
                return Fail(NtfsShapeMappingGap, Numbers[Index], Attr->AttributeType);
            for (ULONG Segment = 0; Segment < Found; Segment++)
            {
                PAttribute Part = Segments[Segment].Attr;

                if (Part->NonResident.FirstVCN != NextVcn)
                    return Fail(NtfsShapeMappingGap, Segments[Segment].Record, Part->AttributeType);
                Status = DecodeRuns(Part, NULL, NULL, NULL, &Clusters);
                if (!NT_SUCCESS(Status) ||
                    Part->NonResident.LastVCN + 1 != Part->NonResident.FirstVCN + Clusters)
                {
                    return Fail(NtfsShapeMappingGap, Segments[Segment].Record, Part->AttributeType);
                }
                NextVcn = Part->NonResident.LastVCN + 1;
            }
            if (NextVcn * ClusterSize != Allocated)
                return Fail(NtfsShapeMappingGap, Numbers[Index], Attr->AttributeType);
        }
        if (Malformed)
            return Fail(NtfsShapeRecordMalformed, Numbers[Index], Offset);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
ShapeVerifier::VerifyRecord(_In_ ULONGLONG Base, _In_ ULONG Flags)
{
    NTSTATUS Status = LoadFile(Base);

    if (NT_SUCCESS(Status))
        Status = CheckList();
    if (NT_SUCCESS(Status) && (Flags & NTFS_VERIFY_SCAN_EXTENSIONS))
        Status = ScanExtensions();
    if (NT_SUCCESS(Status))
        Status = CheckMappings();
    return Status;
}

/*
 * Every file of the volume: the record shape of each base record, the shape
 * of each of its indexes, and for each extension record, that its base's
 * list names it (one MFT pass, so no per-file scan).
 */
NTSTATUS
ShapeVerifier::VerifyVolume(_In_ ULONG Flags, _Out_ PNtfsShapeVolumeStats Stats)
{
    PUCHAR Record;
    PFileRecordHeader Header;
    NTSTATUS Status;

    RtlZeroMemory(Stats, sizeof(*Stats));
    Status = Setup();
    if (!NT_SUCCESS(Status))
        return Status;
    Record = new(PagedPool, TAG_NTFS) UCHAR[RecordSize];
    if (!Record)
        return STATUS_INSUFFICIENT_RESOURCES;
    Header = reinterpret_cast<PFileRecordHeader>(Record);

    for (ULONGLONG Number = 0; Number < RecordCount && NT_SUCCESS(Status); Number++)
    {
        ShapeVerifier File(V, Fault);

        if (!NT_SUCCESS(ReadRecord(Number, Record)))
            continue;
        if (Header->BaseFileRecord)
        {
            ULONGLONG Base = Header->BaseFileRecord & 0x0000FFFFFFFFFFFFULL;
            BOOLEAN Named = FALSE;

            Status = File.LoadFile(Base);
            if (!NT_SUCCESS(Status))
            {
                if (Fault->Invariant == NtfsShapeRecordUnreadable)
                    Status = Fail(NtfsShapeExtensionWithoutList, Number, Base);
                continue;
            }
            for (ULONG Index = 1; Index < File.Count; Index++)
                Named |= File.Numbers[Index] == Number;
            if (!Named)
                Status = Fail(File.ListData ? NtfsShapeExtensionNotListed : NtfsShapeExtensionWithoutList,
                              Number, Base);
            continue;
        }

        Stats->FilesChecked++;
        Status = File.VerifyRecord(Number, Flags & ~NTFS_VERIFY_SCAN_EXTENSIONS);
        for (ULONG Index = 0; NT_SUCCESS(Status) && Index < File.Count; Index++)
        {
            ULONG Offset = 0;
            BOOLEAN Malformed;
            PAttribute Attr;

            while (NT_SUCCESS(Status) && (Attr = File.NextAttribute(File.Buffers[Index], &Offset, &Malformed)) != NULL)
            {
                WCHAR Name[256];
                ShapeVerifier Index_(V, Fault);

                if (Attr->AttributeType != TypeIndexRoot)
                    continue;
                RtlCopyMemory(Name, AttrName(Attr), Attr->NameLength * sizeof(WCHAR));
                Name[Attr->NameLength] = 0;
                Index_.IndexByteLimit = IndexByteLimit;
                Status = Index_.VerifyIndex(Number, Name, Flags & ~NTFS_VERIFY_SCAN_EXTENSIONS);
                if (NT_SUCCESS(Status))
                {
                    if (Index_.IndexSkipped)
                        Stats->IndexesSkipped++;
                    else
                        Stats->IndexesChecked++;
                }
            }
        }
    }
    delete[] Record;
    return Status;
}

/*
 * Checks the entries of one node and pushes the children it points to.
 * Stack holds pending VCNs with their depth in the top 8 bits.
 */
NTSTATUS
ShapeVerifier::CheckNode(_In_ PIndexNodeHeader Node,
                         _In_ ULONG Available,
                         _In_ ULONGLONG Record,
                         _In_ ULONGLONG Vcn,
                         _Inout_ ULONGLONG* Stack,
                         _Inout_ PULONG Depth,
                         _In_ ULONG Level)
{
    PUCHAR Base = reinterpret_cast<PUCHAR>(Node);
    ULONG Offset = Node->IndexOffset;
    ULONG End = Node->TotalIndexSize;
    BOOLEAN Ended = FALSE;

    if (End > Node->AllocatedSize || Node->AllocatedSize > Available ||
        Offset < sizeof(IndexNodeHeader) || Offset >= End || (Offset & 7))
    {
        return Fail(Level ? NtfsShapeIndexBlockInvalid : NtfsShapeIndexRootBad, Record, Vcn);
    }
    while (!Ended)
    {
        PIndexEntry Entry = reinterpret_cast<PIndexEntry>(Base + Offset);

        if (End - Offset < FIELD_OFFSET(IndexEntry, IndexStream) ||
            Entry->EntryLength < FIELD_OFFSET(IndexEntry, IndexStream) ||
            (Entry->EntryLength & 7) || Entry->EntryLength > End - Offset ||
            ((Entry->Flags & INDEX_ENTRY_NODE) &&
             Entry->EntryLength < FIELD_OFFSET(IndexEntry, IndexStream) + sizeof(ULONGLONG)))
        {
            return Fail(Level ? NtfsShapeIndexBlockInvalid : NtfsShapeIndexRootBad, Record, Vcn);
        }
        if (Entry->Flags & INDEX_ENTRY_NODE)
        {
            if (Level >= SHAPE_MAX_DEPTH || *Depth == SHAPE_MAX_DEPTH * 64)
                return Fail(NtfsShapeIndexBlockInvalid, Record, Vcn);
            Stack[(*Depth)++] = (*GetSubnodeVCN(Entry) & 0x00FFFFFFFFFFFFFFULL) |
                                ((ULONGLONG)(Level + 1) << 56);
        }
        Ended = !!(Entry->Flags & INDEX_ENTRY_END);
        Offset += Entry->EntryLength;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
ShapeVerifier::VerifyIndex(_In_ ULONGLONG Base, _In_ PCWSTR IndexName, _In_ ULONG Flags)
{
    ShapeSegment Segments[SHAPE_MAX_RECORDS];
    UCHAR NameLength = 0;
    PAttribute Root = NULL, BitmapAttr = NULL;
    ULONGLONG RootRecord = Base;
    PIndexRootEx RootValue;
    ShapeRun* Runs = NULL;
    ULONG RunCount = 0, RunCapacity = 0, Found;
    PUCHAR Bitmap = NULL, Visited = NULL, Block = NULL;
    ULONGLONG BitmapBits = 0, Blocks = 0, DataSize = 0;
    ULONGLONG* Stack = NULL;
    ULONG Depth = 0, BlockSize, VcnSize;
    NTSTATUS Status;

    while (IndexName[NameLength] && NameLength < 255)
        NameLength++;
    Status = LoadFile(Base);
    if (!NT_SUCCESS(Status))
        return Status;

    for (ULONG Index = 0; Index < Count; Index++)
    {
        ULONG Offset = 0;
        BOOLEAN Malformed;
        PAttribute Attr;

        while ((Attr = NextAttribute(Buffers[Index], &Offset, &Malformed)) != NULL)
        {
            if (!SameName(AttrName(Attr), Attr->NameLength, IndexName, NameLength))
                continue;
            if (Attr->AttributeType == TypeIndexRoot)
            {
                Root = Attr;
                RootRecord = Numbers[Index];
            }
            else if (Attr->AttributeType == TypeBitmap)
            {
                BitmapAttr = Attr;
            }
        }
        if (Malformed)
            return Fail(NtfsShapeRecordMalformed, Numbers[Index], Offset);
    }
    if (!Root || Root->IsNonResident ||
        Root->Resident.DataLength < sizeof(IndexRootEx))
    {
        return Fail(NtfsShapeIndexRootBad, Base, 0);
    }
    RootValue = reinterpret_cast<PIndexRootEx>(reinterpret_cast<PUCHAR>(Root) + Root->Resident.DataOffset);
    BlockSize = RootValue->BytesPerIndexRec;
    if (BlockSize < 512 || (BlockSize & (BlockSize - 1)) || BlockSize > 0x10000)
        return Fail(NtfsShapeIndexRootBad, RootRecord, BlockSize);
    VcnSize = BlockSize >= ClusterSize ? ClusterSize : 512;

    Stack = new(PagedPool, TAG_NTFS) ULONGLONG[SHAPE_MAX_DEPTH * 64];
    if (!Stack)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = CheckNode(&RootValue->Header,
                       Root->Resident.DataLength - FIELD_OFFSET(IndexRootEx, Header),
                       RootRecord, 0, Stack, &Depth, 0);
    if (!NT_SUCCESS(Status) || !Depth)
        goto Done;

    /* A child needs the allocation and its bitmap. */
    Found = CollectSegments(TypeIndexAllocation, IndexName, NameLength, Segments, RTL_NUMBER_OF(Segments));
    if (!Found || Found > RTL_NUMBER_OF(Segments) || !BitmapAttr)
    {
        Status = Fail(NtfsShapeIndexRootBad, RootRecord, Found);
        goto Done;
    }
    for (ULONG Segment = 0; Segment < Found; Segment++)
    {
        ULONGLONG Clusters;

        Status = DecodeRuns(Segments[Segment].Attr, &Runs, &RunCount, &RunCapacity, &Clusters);
        if (!NT_SUCCESS(Status))
        {
            Status = Fail(NtfsShapeMappingGap, Segments[Segment].Record, TypeIndexAllocation);
            goto Done;
        }
    }
    DataSize = Segments[0].Attr->NonResident.DataSize;
    Blocks = DataSize / BlockSize;
    if ((Flags & NTFS_VERIFY_HOOK) ? DataSize > SHAPE_HOOK_INDEX_BYTES
                                   : IndexByteLimit && DataSize > IndexByteLimit)
    {
        IndexSkipped = TRUE;
        Status = STATUS_SUCCESS;
        goto Done;
    }

    if (BitmapAttr->IsNonResident)
    {
        ShapeRun* BitmapRuns = NULL;
        ULONG BitmapRunCount = 0, BitmapRunCapacity = 0;
        ULONGLONG Clusters;
        ULONGLONG Length = BitmapAttr->NonResident.DataSize;

        Status = BitmapAttr->NonResident.FirstVCN || Length > SHAPE_LIST_LIMIT
                     ? STATUS_FILE_CORRUPT_ERROR
                     : DecodeRuns(BitmapAttr, &BitmapRuns, &BitmapRunCount, &BitmapRunCapacity, &Clusters);
        if (NT_SUCCESS(Status))
        {
            Bitmap = new(PagedPool, TAG_NTFS) UCHAR[(ULONG)Length + 1];
            Status = Bitmap ? ReadStream(BitmapRuns, BitmapRunCount, 0, (ULONG)Length, Bitmap)
                            : STATUS_INSUFFICIENT_RESOURCES;
        }
        delete[] BitmapRuns;
        BitmapBits = Length * 8;
    }
    else
    {
        BitmapBits = (ULONGLONG)BitmapAttr->Resident.DataLength * 8;
        Bitmap = new(PagedPool, TAG_NTFS) UCHAR[BitmapAttr->Resident.DataLength + 1];
        if (Bitmap)
        {
            RtlCopyMemory(Bitmap,
                          reinterpret_cast<PUCHAR>(BitmapAttr) + BitmapAttr->Resident.DataOffset,
                          BitmapAttr->Resident.DataLength);
        }
        Status = Bitmap ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!NT_SUCCESS(Status))
    {
        if (Status != STATUS_INSUFFICIENT_RESOURCES)
            Status = Fail(NtfsShapeIndexRootBad, RootRecord, TypeBitmap);
        goto Done;
    }

    Visited = new(PagedPool, TAG_NTFS) UCHAR[(ULONG)(Blocks / 8) + 1];
    Block = new(PagedPool, TAG_NTFS) UCHAR[BlockSize];
    if (!Visited || !Block)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }
    RtlZeroMemory(Visited, (ULONG)(Blocks / 8) + 1);

    while (Depth)
    {
        ULONGLONG Entry = Stack[--Depth];
        ULONG Level = (ULONG)(Entry >> 56);
        ULONGLONG Vcn = Entry & 0x00FFFFFFFFFFFFFFULL;
        ULONGLONG Number;
        PIndexBuffer Buffer = reinterpret_cast<PIndexBuffer>(Block);

        if (Vcn > ~0ULL / VcnSize || (Vcn * VcnSize) % BlockSize ||
            (Number = Vcn * VcnSize / BlockSize) >= Blocks)
        {
            Status = Fail(NtfsShapeIndexBlockBeyondAllocation, RootRecord, Vcn);
            goto Done;
        }
        if (Visited[Number >> 3] & (1 << (Number & 7)))
        {
            Status = Fail(NtfsShapeIndexBlockShared, RootRecord, Vcn);
            goto Done;
        }
        Visited[Number >> 3] |= (UCHAR)(1 << (Number & 7));
        if (Number >= BitmapBits || !(Bitmap[Number >> 3] & (1 << (Number & 7))))
        {
            Status = Fail(NtfsShapeIndexBlockFree, RootRecord, Vcn);
            goto Done;
        }
        Status = ReadStream(Runs, RunCount, Number * BlockSize, BlockSize, Block);
        if (!NT_SUCCESS(Status) ||
            RtlCompareMemory(Buffer->RecordHeader.TypeID, "INDX", 4) != 4 ||
            !NT_SUCCESS(NtfsApplyFixup(&Buffer->RecordHeader, BlockSize, V->BytesPerSector)) ||
            Buffer->VCN != Vcn)
        {
            Status = Fail(NtfsShapeIndexBlockInvalid, RootRecord, Vcn);
            goto Done;
        }
        Status = CheckNode(&Buffer->IndexHeader, BlockSize - FIELD_OFFSET(IndexBuffer, IndexHeader),
                           RootRecord, Vcn, Stack, &Depth, Level);
        if (!NT_SUCCESS(Status))
            goto Done;
    }

    for (ULONGLONG Number = 0; Number < Blocks && Number < BitmapBits; Number++)
    {
        if ((Bitmap[Number >> 3] & (1 << (Number & 7))) && !(Visited[Number >> 3] & (1 << (Number & 7))))
        {
            Status = Fail(NtfsShapeIndexBlockUnreachable, RootRecord, Number * BlockSize / VcnSize);
            goto Done;
        }
    }
    Status = STATUS_SUCCESS;

Done:
    delete[] Stack;
    delete[] Runs;
    delete[] Bitmap;
    delete[] Visited;
    delete[] Block;
    return Status;
}

}

extern "C"
NTSTATUS
NtfsVerifyRecordShape(
    _In_ PNtfsVolume DiskVolume,
    _In_ ULONGLONG FileRecord,
    _In_ ULONG Flags,
    _Out_ PNtfsShapeFault Fault)
{
    if (!DiskVolume || !Fault)
        return STATUS_INVALID_PARAMETER;
    ShapeVerifier Verifier(reinterpret_cast<PVolume>(DiskVolume), Fault);
    return Verifier.VerifyRecord(FileRecord & 0x0000FFFFFFFFFFFFULL, Flags);
}

extern "C"
NTSTATUS
NtfsVerifyVolumeShape(
    _In_ PNtfsVolume DiskVolume,
    _In_ ULONG Flags,
    _Out_ PULONGLONG FilesChecked,
    _Out_ PNtfsShapeFault Fault)
{
    NtfsShapeVolumeStats Stats;
    NTSTATUS Status;

    if (!DiskVolume || !FilesChecked || !Fault)
        return STATUS_INVALID_PARAMETER;
    Status = NtfsVerifyVolumeShapeBounded(DiskVolume, Flags, 0, &Stats, Fault);
    *FilesChecked = Stats.FilesChecked;
    return Status;
}

extern "C"
NTSTATUS
NtfsVerifyVolumeShapeBounded(
    _In_ PNtfsVolume DiskVolume,
    _In_ ULONG Flags,
    _In_ ULONGLONG IndexByteLimit,
    _Out_ PNtfsShapeVolumeStats Stats,
    _Out_ PNtfsShapeFault Fault)
{
    if (!DiskVolume || !Stats || !Fault)
        return STATUS_INVALID_PARAMETER;
    ShapeVerifier Verifier(reinterpret_cast<PVolume>(DiskVolume), Fault);
    Verifier.IndexByteLimit = IndexByteLimit;
    return Verifier.VerifyVolume(Flags & ~NTFS_VERIFY_HOOK, Stats);
}

extern "C"
NTSTATUS
NtfsVerifyIndexShape(
    _In_ PNtfsVolume DiskVolume,
    _In_ ULONGLONG FileRecord,
    _In_ PCWSTR IndexName,
    _In_ ULONG Flags,
    _Out_ PNtfsShapeFault Fault)
{
    if (!DiskVolume || !IndexName || !Fault)
        return STATUS_INVALID_PARAMETER;
    ShapeVerifier Verifier(reinterpret_cast<PVolume>(DiskVolume), Fault);
    return Verifier.VerifyIndex(FileRecord & 0x0000FFFFFFFFFFFFULL, IndexName, Flags);
}

/*
 * The checked-build hook, after a mutation returned success. Bounded so a
 * checked build stays usable: the extension scan only on small MFTs, the
 * index walk only up to SHAPE_HOOK_INDEX_BYTES of allocation. A violation is
 * printed and asserted.
 */
void
NtfsVerifyShapeAfterMutation(
    _In_ PVolume DiskVolume,
    _In_ ULONGLONG FileRecord,
    _In_opt_ PCWSTR IndexName)
{
    NtfsShapeFault Fault;
    NtfsVolumeInformation Information;
    ULONG Flags = NTFS_VERIFY_HOOK;
    NTSTATUS Status;

    if (!DiskVolume || !DiskVolume->MFT || DiskVolume->ShapeVerifySuppressed ||
        FileRecord == MAXULONG || FileRecord == (ULONGLONG)-1)
    {
        return;
    }
    if (NT_SUCCESS(DiskVolume->QueryInformation(&Information)) &&
        Information.BytesPerFileRecordSegment &&
        Information.MftValidDataLength / Information.BytesPerFileRecordSegment <= SHAPE_HOOK_SCAN_RECORDS)
    {
        Flags |= NTFS_VERIFY_SCAN_EXTENSIONS;
    }

    Status = NtfsVerifyRecordShape(reinterpret_cast<PNtfsVolume>(DiskVolume), FileRecord, Flags, &Fault);
    if (Status == STATUS_FILE_CORRUPT_ERROR && Fault.Invariant == NtfsShapeRecordUnreadable &&
        Fault.Detail == (ULONG)STATUS_NOT_FOUND)
    {
        return; /* no longer in use: the mutation deleted it */
    }
    if (Status == STATUS_SUCCESS && IndexName)
        Status = NtfsVerifyIndexShape(reinterpret_cast<PNtfsVolume>(DiskVolume), FileRecord, IndexName, Flags, &Fault);
    if (Status != STATUS_FILE_CORRUPT_ERROR)
        return;

#ifdef NTFSLIB_PORTABLE
    fprintf(stderr,
            "ntfslib: shape invariant %u broken: record 0x%llx, detail 0x%llx (after a change to record 0x%llx)\n",
            (unsigned)Fault.Invariant, (unsigned long long)Fault.Record,
            (unsigned long long)Fault.Detail, (unsigned long long)FileRecord);
#else
    DbgPrint("ntfslib: shape invariant %lu broken: record 0x%I64x, detail 0x%I64x (after a change to record 0x%I64x)\n",
             (ULONG)Fault.Invariant, Fault.Record, Fault.Detail, FileRecord);
#endif
    ASSERT(!"ntfslib shape invariant broken");
}

void
FileRecord::VerifyShapeAfterMutation(_In_ NTSTATUS Status)
{
#ifdef NTFSLIB_VERIFY_SHAPE
    if (NT_SUCCESS(Status) && Header && RecordNumber != MAXULONG)
    {
        NtfsVerifyShapeAfterMutation(DiskVolume,
                                     Header->BaseFileRecord
                                         ? Header->BaseFileRecord & 0x0000FFFFFFFFFFFFULL
                                         : RecordNumber,
                                     NULL);
    }
#else
    UNREFERENCED_PARAMETER(Status);
#endif
}
