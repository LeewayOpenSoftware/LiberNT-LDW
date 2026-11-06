/*
 * PROJECT:     LiberNT NTFS library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Volume consistency check
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "ntfslib_new.h"
#include "ntfslib_new_internal.h"

#define CHECK_RECORD_CHUNK      64
/* Unreadable attribute lists rebuilt in one check, and the extension
 * records one rebuilt list may name. */
#define CHECK_LIST_REBUILDS     32
#define CHECK_LIST_EXTENSIONS   64
#define CHECK_BITMAP_CHUNK      0x10000
#define CHECK_FIRST_USER_RECORD NTFS_FIRST_USER_FILE_RECORD

namespace
{

struct CrossLink
{
    ULONGLONG Owner;
    ULONGLONG Vcn;
    ULONGLONG Lcn;
    ULONGLONG Length;
    ULONG Type;
    USHORT NameLength;
    WCHAR Name[256];
};

struct CorruptRecord
{
    ULONGLONG Number;
    ULONG Faults;
    BOOLEAN Readable;
};

struct DanglingEntry
{
    ULONGLONG Directory;
    ULONGLONG Target;
    USHORT NameLength;
    BOOLEAN Pending;    /* cut from an index block that is not written yet */
    BOOLEAN Removed;    /* gone from the index on disk */
    WCHAR Name[255];
};

struct LostFile
{
    ULONGLONG Number;
    ULONGLONG Parent;
    USHORT NameLength;
    WCHAR Name[NTFS_MAX_FILE_NAME_LENGTH + 1];
};

struct NameTally
{
    ULONG ExpectedHash;
    ULONG FoundHash;
    USHORT Expected;
    USHORT Found;
};

class VolumeCheck
{
public:
    VolumeCheck(_In_ PVolume TargetVolume,
                _In_opt_ PNtfsCheckCallback Callback,
                _In_opt_ void* Context,
                _Out_ PNtfsCheckResult Result)
        : V(TargetVolume), Report(Callback), ReportContext(Context), Out(Result)
    {
        V->ShapeVerifySuppressed++;
    }

    ~VolumeCheck()
    {
        V->ShapeVerifySuppressed--;
        delete[] UsedMap;
        delete[] CrossMap;
        delete[] MftBitmap;
        delete[] DirectoryMap;
        delete[] InUseMap;
        delete[] Sequence;
        delete[] Names;
        delete[] Records;
        delete[] Dangling;
        delete[] Corrupt;
        delete[] Orphans;
        delete[] Lost;
        delete[] CrossLinks;
        delete[] NameKeys;
        delete[] NameKeyUsed;
        delete[] ExtensionOwner;
        delete[] AttrListMap;
        delete[] ExtendedMap;
        delete[] LiveBeyondMap;
    }

    NTSTATUS Run(_In_ BOOLEAN Repair);

private:
    PVolume V;
    PNtfsCheckCallback Report;
    void* ReportContext;
    PNtfsCheckResult Out;

    ULONG ClusterSize = 0;
    ULONG RecordSize = 0;
    ULONGLONG ClusterCount = 0;
    ULONGLONG RecordCount = 0;
    ULONGLONG StructuralErrors = 0;
    /* The first file records that raised a structural problem, to name the
     * one that keeps the index and bitmap repairs off. */
    ULONGLONG Blockers[16];
    ULONG BlockerCount = 0;
    BOOLEAN RepairIndexes = FALSE;
    BOOLEAN RepairRecords = FALSE;
    BOOLEAN MftBitmapChanged = FALSE;
    ULONG MftBitmapFixes = 0;
    ULONG RecordFaults = 0;
    CorruptRecord* Corrupt = NULL;
    ULONG CorruptCount = 0;
    ULONG CorruptCapacity = 0;
    ULONGLONG* Orphans = NULL;
    CrossLink* CrossLinks = NULL;
    ULONG CrossLinkCount = 0;
    ULONG CrossLinkCapacity = 0;
    ULONG OrphanCount = 0;
    ULONG OrphanCapacity = 0;
    LostFile* Lost = NULL;
    ULONG LostCount = 0;
    ULONG LostCapacity = 0;
    ULONGLONG* NameKeys = NULL;
    ULONG NameKeyCount = 0;
    ULONG NameKeyCapacity = 0;
    BOOLEAN NameKeysIncomplete = FALSE;
    PULONG ExtensionOwner = NULL;
    PUCHAR AttrListMap = NULL;
    /* Bases that some record, valid or faulted, names as its base. */
    PUCHAR ExtendedMap = NULL;
    /* One bit per name key: an index entry already counted for it. */
    PUCHAR NameKeyUsed = NULL;

    /* MFT bitmap bits past the end of the $MFT data (CheckRecordsBeyondMft). */
    ULONGLONG BitmapBits = 0;
    PUCHAR LiveBeyondMap = NULL;
    ULONG BeyondRanges = 0;
    ULONG DanglingFailures = 0;

    DanglingEntry* Dangling = NULL;
    ULONG DanglingCount = 0;
    ULONG PendingCuts = 0;
    ULONG DanglingCapacity = 0;

    PUCHAR UsedMap = NULL;
    PUCHAR CrossMap = NULL;
    PUCHAR MftBitmap = NULL;
    ULONG MftBitmapLength = 0;
    PUCHAR DirectoryMap = NULL;
    PUCHAR InUseMap = NULL;
    PUSHORT Sequence = NULL;
    NameTally* Names = NULL;
    PUCHAR Records = NULL;

    static BOOLEAN TestBit(_In_ const UCHAR* Map, _In_ ULONGLONG Bit)
    {
        return (Map[Bit >> 3] & (1 << (Bit & 7))) != 0;
    }

    static void SetBit(_Inout_ PUCHAR Map, _In_ ULONGLONG Bit)
    {
        Map[Bit >> 3] |= (UCHAR)(1 << (Bit & 7));
    }

    static PUCHAR AllocateMap(_In_ ULONGLONG Bits)
    {
        ULONGLONG Bytes = (Bits + 7) / 8;
        PUCHAR Map;

        if (Bytes == 0 || Bytes > MAXULONG)
            return NULL;
        Map = new(PagedPool, TAG_NTFS) UCHAR[(ULONG)Bytes];
        if (Map)
            RtlZeroMemory(Map, (ULONG)Bytes);
        return Map;
    }

    static ULONG HashName(_In_ const FileNameEx* Name)
    {
        const UCHAR* Bytes = reinterpret_cast<const UCHAR*>(Name);
        ULONG Hash = 2166136261u;
        ULONG Length = FIELD_OFFSET(FileNameEx, Name) + Name->NameLength * sizeof(WCHAR);

        for (ULONG Index = 0; Index < sizeof(Name->ParentFileReference); Index++)
            Hash = (Hash ^ Bytes[Index]) * 16777619u;
        for (ULONG Index = FIELD_OFFSET(FileNameEx, NameLength); Index < Length; Index++)
            Hash = (Hash ^ Bytes[Index]) * 16777619u;
        return Hash;
    }

    void Event(_In_ NtfsCheckEvent Kind, _In_ ULONGLONG Arg1, _In_ ULONGLONG Arg2)
    {
        if (Kind != NtfsCheckLostClustersFreed && Kind != NtfsCheckUsedClustersClaimed &&
            Kind != NtfsCheckIndexEntryFixed && Kind != NtfsCheckIndexEntryRemoved &&
            Kind != NtfsCheckRecordFreed && Kind != NtfsCheckRecordBitmapFixed &&
            Kind != NtfsCheckNameRelinked && Kind != NtfsCheckCrossLinkResolved &&
            Kind != NtfsCheckLostAndFound && Kind != NtfsCheckFileRecovered &&
            Kind != NtfsCheckAttributeListFixed && Kind != NtfsCheckDataAttributeInserted &&
            Kind != NtfsCheckLostNameRemoved && Kind != NtfsCheckRepairsBlocked &&
            Kind != NtfsCheckNameRelinkFailed && Kind != NtfsCheckNameFoundUncounted &&
            Kind != NtfsCheckAttributeListRemoved && Kind != NtfsCheckIndexReset &&
            Kind != NtfsCheckIndexEntryNotRemoved && Kind != NtfsCheckMftBitmapNotWritten &&
            Kind != NtfsCheckRecordKept &&
            Kind != NtfsCheckAttributeListRebuilt)
        {
            Out->Errors++;
            Out->FoundErrors++;
        }
        if (Kind == NtfsCheckRecordCorrupt || Kind == NtfsCheckBadRunList ||
            Kind == NtfsCheckClusterOutOfRange || Kind == NtfsCheckCrossLinked)
        {
            StructuralErrors++;
            /* Every structural event names its file record in Arg1. */
            if (BlockerCount < RTL_NUMBER_OF(Blockers) &&
                (BlockerCount == 0 || Blockers[BlockerCount - 1] != Arg1))
            {
                Blockers[BlockerCount++] = Arg1;
            }
        }
        if (Kind == NtfsCheckRecordCorrupt || Kind == NtfsCheckBadRunList ||
            Kind == NtfsCheckClusterOutOfRange)
        {
            RecordFaults++;
        }
        if (Report)
            Report(ReportContext, Kind, Arg1, Arg2);
    }

    NTSTATUS LoadMftBitmap();
    NTSTATUS ReadRecords(_In_ ULONGLONG First, _In_ ULONG Count);
    void CheckRecord(_In_ ULONGLONG Number, _Inout_ PUCHAR Record);
    NTSTATUS CheckRecordsBeyondMft();
    void ReportBeyondRange(_In_ ULONGLONG First, _In_ ULONGLONG Count);
    BOOLEAN IsLiveBeyondMft(_In_ ULONGLONG Number);
    void CheckAttributes(_In_ ULONGLONG Number, _In_ ULONGLONG Owner, _In_ PUCHAR Record);
    void CheckRunList(_In_ ULONGLONG Number, _In_ ULONGLONG Owner, _In_ PAttribute Attr);
    void CheckNonResidentSizes(_In_ ULONGLONG Number, _In_ BOOLEAN WholeMapping, _In_ PAttribute Attr);
    struct ClusterRange
    {
        ULONGLONG Lcn;
        ULONGLONG Length;
    };

    struct RangeList
    {
        ClusterRange* Ranges;
        ULONG Count;
        ULONG Capacity;
    };

    static BOOLEAN AddRange(_Inout_ RangeList* List, _In_ ULONGLONG Lcn);
    NTSTATUS CheckBitmap(_In_ BOOLEAN Repair);
    NTSTATUS RepairRange(_In_ BOOLEAN Claim, _In_ ULONGLONG Lcn, _In_ ULONGLONG Count);
    BOOLEAN RecordHasName(_In_ ULONGLONG Ordinal, _In_ const FileNameEx* Key, _In_ ULONG KeyLength);
    NTSTATUS WriteStreamRange(_In_ PDataRun Runs, _In_ ULONGLONG Offset, _In_ ULONG Length, _In_ PUCHAR Data);
    BOOLEAN RememberDangling(_In_ ULONGLONG Directory, _In_ ULONGLONG Target, _In_ const FileNameEx* Key);
    void SettlePendingCuts(_In_ BOOLEAN Written);
    BOOLEAN IsStillReferenced(_In_ ULONGLONG Number);
    void RememberCorrupt(_In_ ULONGLONG Number, _In_ ULONG Faults, _In_ BOOLEAN Readable);
    void RememberOrphan(_In_ ULONGLONG Number);
    NTSTATUS WriteMftBitmap();
    void CommitMftBitmap(_In_ BOOLEAN IndexesRepaired);
    void PlanRecordFrees();
    void RepairRecordsPass();
    void RelinkOrphans();
    void RememberLost(_In_ ULONGLONG Number, _In_ const FileNameEx* Name);
    void RememberNameKey(_In_ ULONGLONG Owner, _In_ ULONG Hash);
    void SortNameKeys();
    BOOLEAN HasNameKey(_In_ ULONGLONG Owner, _In_ ULONG Hash);
    enum NameMatch
    {
        NameAbsent,
        NameFresh,
        NameRepeated
    };
    NameMatch ConsumeNameKey(_In_ ULONGLONG Owner, _In_ ULONG Hash);
    static BOOLEAN KeepListEntry(_In_ void* Context, _In_ const AttributeListEx* Entry);
    BOOLEAN HasExtensions(_In_ ULONGLONG Base);
    void CheckAttributeLists();
    void RebuildAttributeLists(_In_reads_(Count) const ULONGLONG* Bases, _In_ ULONG Count);
    struct WalkFrame
    {
        ULONG Block;
        PIndexEntry Low;
        PIndexEntry High;
    };
    struct IndexWalk
    {
        ULONGLONG Directory;
        PUCHAR Bitmap;
        ULONGLONG BitmapBits;
        PUCHAR Valid;
        PUCHAR Linked;
        WalkFrame* Stack;
        /* Per block: the offset of a tail of keys above the node's range,
         * which a split cut short leaves in the left node; 0 if none. For
         * an internal node, the child its END keeps; ~0 for a leaf. */
        PULONG Trim;
        PULONGLONG TrimChild;
        /* What a lookup from the root walks, to tell a cut node's names are
         * held elsewhere: the allocation and the root's header. */
        PUCHAR Allocation;
        PIndexNodeHeader RootHeader;
        ULONG Depth;
        ULONG Blocks;
        ULONG BlockSize;
        ULONG VcnSize;
        ULONG InUse;
        ULONG Problems;
        BOOLEAN Unwalkable;
        BOOLEAN CaseSensitive;
    };
    static BOOLEAN KeyName(_In_ PIndexEntry Entry, _Out_ PUNICODE_STRING Name);
    BOOLEAN KeyInRange(_In_ IndexWalk* Walk, _In_ PUNICODE_STRING Name, _In_opt_ PIndexEntry Low, _In_opt_ PIndexEntry High);
    BOOLEAN KeyCompare(_In_ IndexWalk* Walk, _In_ PUNICODE_STRING Name, _In_opt_ PIndexEntry Bound, _Out_ LONG* Result);
    BOOLEAN KeyAbove(_In_ IndexWalk* Walk, _In_ PUNICODE_STRING Name, _In_opt_ PIndexEntry High);
    ULONG FindSplitTail(_In_ IndexWalk* Walk,
                        _In_ PIndexNodeHeader Node,
                        _In_opt_ PIndexEntry Low,
                        _In_opt_ PIndexEntry High);
    ULONG AcceptTail(_In_ IndexWalk* Walk, _In_ PIndexNodeHeader Node, _In_opt_ PIndexEntry Low, _In_ ULONG Start);
    BOOLEAN KeyReachable(_In_ IndexWalk* Walk, _In_ PUNICODE_STRING Name);
    void VerifyWholeCuts(_Inout_ IndexWalk* Walk);
    void LinkChild(_Inout_ IndexWalk* Walk, _In_ ULONGLONG Vcn, _In_opt_ PIndexEntry Low, _In_opt_ PIndexEntry High);
    void WalkIndexNode(_Inout_ IndexWalk* Walk,
                       _In_ PIndexNodeHeader Node,
                       _In_ ULONG Available,
                       _In_ ULONG NodeBlock,
                       _In_ ULONGLONG NodeVcn,
                       _In_opt_ PIndexEntry Low,
                       _In_opt_ PIndexEntry High);
    void RecoverLostFiles();
    void RememberCrossLink(_In_ ULONGLONG Owner, _In_ PAttribute Attr, _In_ ULONGLONG Vcn, _In_ ULONGLONG Lcn, _In_ ULONGLONG Length);
    void ResolveCrossLinks();
    void RemoveDangling();
    ULONG CheckIndexEntries(_In_ ULONGLONG Directory,
                            _In_ PIndexNodeHeader Node,
                            _In_ ULONG Available,
                            _In_ ULONGLONG Vcn,
                            _In_ BOOLEAN AllowFix,
                            _In_ BOOLEAN AllowCut);
    void CheckDirectory(_In_ ULONGLONG Directory);
};

NTSTATUS
VolumeCheck::LoadMftBitmap()
{
    PFileRecord MftFile;
    PAttribute BitmapAttr;
    NTSTATUS Status;

    Status = V->MFT->GetFileAttributeFromFileRecordNumber(TypeBitmap, NULL, _MFT, &MftFile, &BitmapAttr);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = MftFile->ReadAttributeAlloc(BitmapAttr, &MftBitmap, &MftBitmapLength);
    delete MftFile;
    if (NT_SUCCESS(Status) && (ULONGLONG)MftBitmapLength * 8 < RecordCount)
        Status = STATUS_FILE_CORRUPT_ERROR;
    return Status;
}

NTSTATUS
VolumeCheck::ReadRecords(_In_ ULONGLONG First, _In_ ULONG Count)
{
    ULONG Index = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    while (NT_SUCCESS(Status) && Index < Count)
    {
        ULONGLONG Start, Next;
        ULONG Run = 1;

        Status = V->MFT->GetFileRecordDiskOffset(First + Index, &Start);
        while (NT_SUCCESS(Status) && Index + Run < Count &&
               NT_SUCCESS(V->MFT->GetFileRecordDiskOffset(First + Index + Run, &Next)) &&
               Next == Start + (ULONGLONG)Run * RecordSize)
        {
            Run++;
        }
        if (NT_SUCCESS(Status))
            Status = V->ReadVolume(Start, Run * RecordSize, Records + Index * RecordSize);
        Index += Run;
    }
    return Status;
}

void
VolumeCheck::CheckRunList(_In_ ULONGLONG Number, _In_ ULONGLONG Owner, _In_ PAttribute Attr)
{
    const UCHAR* Run;
    const UCHAR* End;
    ULONGLONG Vcn = Attr->NonResident.FirstVCN;
    ULONGLONG Lcn = 0;

    if (Attr->NonResident.DataRunsOffset >= Attr->Length)
    {
        Event(NtfsCheckBadRunList, Number, Attr->AttributeType);
        return;
    }

    Run = reinterpret_cast<const UCHAR*>(Attr) + Attr->NonResident.DataRunsOffset;
    End = reinterpret_cast<const UCHAR*>(Attr) + Attr->Length;
    while (Run < End && *Run)
    {
        ULONG LengthBytes = *Run & 0x0F;
        ULONG OffsetBytes = *Run >> 4;
        ULONGLONG Length = 0;
        LONGLONG Delta = 0;

        Run++;
        if (LengthBytes == 0 || LengthBytes > 8 || OffsetBytes > 8 ||
            (ULONG)(End - Run) < LengthBytes + OffsetBytes)
        {
            Event(NtfsCheckBadRunList, Number, Attr->AttributeType);
            return;
        }
        /*
         * A run length is a signed value: a top bit in its last byte makes
         * it negative, and readers reject the whole mapping. Decoding it
         * unsigned would accept, say, 161 clusters written as the single
         * byte 0xA1, which every reader takes as -95.
         */
        if (Run[LengthBytes - 1] & 0x80)
        {
            Event(NtfsCheckBadRunList, Number, Attr->AttributeType);
            return;
        }
        for (ULONG Index = 0; Index < LengthBytes; Index++)
            Length |= (ULONGLONG)Run[Index] << (Index * 8);
        Run += LengthBytes;
        if (Length == 0)
        {
            Event(NtfsCheckBadRunList, Number, Attr->AttributeType);
            return;
        }
        if (OffsetBytes)
        {
            for (ULONG Index = 0; Index < OffsetBytes; Index++)
                Delta |= (LONGLONG)((ULONGLONG)Run[Index] << (Index * 8));
            if (Run[OffsetBytes - 1] & 0x80)
                Delta |= (LONGLONG)(~0ULL << (OffsetBytes * 8 - 1) << 1);
            Run += OffsetBytes;
            Lcn += (ULONGLONG)Delta;
            if (Lcn >= ClusterCount || Length > ClusterCount - Lcn)
            {
                Event(NtfsCheckClusterOutOfRange, Number, Lcn);
                return;
            }
            ULONGLONG FirstShared = 0;
            BOOLEAN Shared = FALSE;

            for (ULONGLONG Cluster = Lcn; Cluster < Lcn + Length; Cluster++)
            {
                if (TestBit(UsedMap, Cluster))
                {
                    if (!Shared)
                        FirstShared = Cluster;
                    Shared = TRUE;
                    if (CrossMap)
                        SetBit(CrossMap, Cluster);
                    continue;
                }
                SetBit(UsedMap, Cluster);
                Out->UsedClusters++;
            }
            if (Shared)
            {
                Event(NtfsCheckCrossLinked, Number, FirstShared);
                RememberCrossLink(Owner, Attr, Vcn, Lcn, Length);
            }
        }
        Vcn += Length;
    }

    if (Vcn != Attr->NonResident.LastVCN + 1)
        Event(NtfsCheckBadRunList, Number, Attr->AttributeType);
}

/*
 * The sizes of a non-resident attribute are only kept in its first segment.
 * Data size never exceeds allocated size, initialized size never exceeds data
 * size, and the allocation is whole clusters. When the segment is the whole
 * attribute (a base record without an attribute list), its mapping must also
 * cover exactly the allocated size. A data size past the mapping makes the
 * tail of the stream unreadable. Reported only; nothing is changed.
 */
void
VolumeCheck::CheckNonResidentSizes(_In_ ULONGLONG Number, _In_ BOOLEAN WholeMapping, _In_ PAttribute Attr)
{
    ULONGLONG Allocated = Attr->NonResident.AllocatedSize;

    if (Attr->NonResident.FirstVCN != 0)
        return;
    if (Attr->NonResident.DataSize > Allocated ||
        Attr->NonResident.InitalizedDataSize > Attr->NonResident.DataSize ||
        (ClusterSize != 0 && Allocated % ClusterSize != 0))
    {
        Event(NtfsCheckAttributeSizeInvalid, Number, Attr->AttributeType);
        return;
    }
    /* LastVCN + 1 wraps to 0 for an empty mapping, matching an allocation of 0. */
    if (WholeMapping && ClusterSize != 0 &&
        (Attr->NonResident.LastVCN + 1) * (ULONGLONG)ClusterSize != Allocated)
    {
        Event(NtfsCheckAttributeMappingMismatch, Number, Attr->AttributeType);
    }
}

void
VolumeCheck::CheckAttributes(_In_ ULONGLONG Number, _In_ ULONGLONG Owner, _In_ PUCHAR Record)
{
    PFileRecordHeader Header = reinterpret_cast<PFileRecordHeader>(Record);
    ULONG Offset = Header->AttributeOffset;
    BOOLEAN HasList = FALSE;

    while (Offset + sizeof(ULONG) <= Header->ActualSize)
    {
        PAttribute Attr = reinterpret_cast<PAttribute>(Record + Offset);

        if (Attr->AttributeType == TypeAttributeEndMarker)
            return;
        if (Offset + FIELD_OFFSET(Attribute, Resident) > Header->ActualSize ||
            Attr->Length < FIELD_OFFSET(Attribute, Resident) + 8 || (Attr->Length & 7) != 0 ||
            Attr->Length > Header->ActualSize - Offset ||
            (Attr->NameLength && (ULONG)Attr->NameOffset + Attr->NameLength * sizeof(WCHAR) > Attr->Length))
        {
            Event(NtfsCheckRecordCorrupt, Number, Offset);
            return;
        }

        if (Attr->AttributeType == TypeAttributeList && Owner == Number)
        {
            SetBit(AttrListMap, Number);
            HasList = TRUE;
        }

        if (Attr->IsNonResident)
        {
            if (Attr->Length < FIELD_OFFSET(Attribute, NonResident.CompressedDataSize))
            {
                Event(NtfsCheckRecordCorrupt, Number, Offset);
                return;
            }
            CheckRunList(Number, Owner, Attr);
            CheckNonResidentSizes(Number, Owner == Number && !HasList, Attr);
        }
        else
        {
            if ((ULONG)Attr->Resident.DataOffset + Attr->Resident.DataLength > Attr->Length)
            {
                Event(NtfsCheckRecordCorrupt, Number, Offset);
                return;
            }
            if (Attr->AttributeType == TypeFileName)
            {
                const FileNameEx* Name =
                    reinterpret_cast<const FileNameEx*>(reinterpret_cast<PUCHAR>(Attr) + Attr->Resident.DataOffset);

                if (Attr->Resident.DataLength < FIELD_OFFSET(FileNameEx, Name) ||
                    Attr->Resident.DataLength < FIELD_OFFSET(FileNameEx, Name) + Name->NameLength * sizeof(WCHAR))
                {
                    Event(NtfsCheckRecordCorrupt, Number, Offset);
                    return;
                }
                Names[Owner].Expected++;
                Names[Owner].ExpectedHash += HashName(Name);
                RememberNameKey(Owner, HashName(Name));
            }
            else if (Attr->AttributeType == TypeIndexRoot && Attr->NameLength == 4 &&
                     RtlCompareMemory(reinterpret_cast<PUCHAR>(Attr) + Attr->NameOffset, L"$I30", 8) == 8)
            {
                SetBit(DirectoryMap, Owner);
            }
        }
        Offset += Attr->Length;
    }

    Event(NtfsCheckRecordCorrupt, Number, Offset);
}

void
VolumeCheck::CheckRecord(_In_ ULONGLONG Number, _Inout_ PUCHAR Record)
{
    PFileRecordHeader Header = reinterpret_cast<PFileRecordHeader>(Record);
    BOOLEAN Allocated = TestBit(MftBitmap, Number);
    BOOLEAN InUse;
    ULONGLONG Owner;
    ULONG Faults = RecordFaults;

    if (RtlCompareMemory(Header->Header.TypeID, "FILE", 4) != 4 ||
        !NT_SUCCESS(NtfsApplyFixup(&Header->Header, RecordSize, V->BytesPerSector)))
    {
        /* A FILE record whose fixup fails still names its base: the header
         * lies in the first sector, below the bytes the fixup covers. So does
         * a torn record marked BAAD. A record without either signature holds
         * no attribute anyone can read. */
        if (RtlCompareMemory(Header->Header.TypeID, "FILE", 4) == 4 ||
            RtlCompareMemory(Header->Header.TypeID, "BAAD", 4) == 4)
        {
            Owner = Header->BaseFileRecord & 0x0000FFFFFFFFFFFFULL;
            if (Owner != 0 && Owner < RecordCount)
                SetBit(ExtendedMap, Owner);
        }
        if (Allocated && Number >= CHECK_FIRST_USER_RECORD)
        {
            Event(NtfsCheckRecordCorrupt, Number, 0);
            RememberCorrupt(Number, RecordFaults - Faults, FALSE);
        }
        return;
    }

    InUse = (Header->Flags & FR_IN_USE) != 0;
    if (InUse != Allocated && (InUse || Number >= CHECK_FIRST_USER_RECORD))
    {
        Event(NtfsCheckRecordBitmap, Number, InUse);
        if (RepairRecords)
        {
            if (InUse)
                SetBit(MftBitmap, Number);
            else
                MftBitmap[Number >> 3] &= (UCHAR)~(1 << (Number & 7));
            MftBitmapChanged = TRUE;
            MftBitmapFixes++;
        }
    }
    if (!InUse)
        return;

    Owner = Header->BaseFileRecord & 0x0000FFFFFFFFFFFFULL;
    if (Owner != 0 && Owner < RecordCount)
        SetBit(ExtendedMap, Owner);

    if (Header->AllocatedSize != RecordSize || Header->ActualSize > RecordSize ||
        Header->AttributeOffset < sizeof(NTFSRecordHeader) || (Header->AttributeOffset & 7) != 0 ||
        Header->AttributeOffset >= Header->ActualSize ||
        (Header->Header.UpdateSequenceOffset >= FIELD_OFFSET(FileRecordHeader, MFTRecordNumber) + sizeof(UINT32) &&
         Header->MFTRecordNumber != (UINT32)Number))
    {
        Event(NtfsCheckRecordCorrupt, Number, 0);
        if (Number >= CHECK_FIRST_USER_RECORD)
            RememberCorrupt(Number, RecordFaults - Faults, TRUE);
        return;
    }

    Owner = Header->BaseFileRecord & 0x0000FFFFFFFFFFFFULL;
    if (Owner == 0)
    {
        Owner = Number;
        SetBit(InUseMap, Number);
        Sequence[Number] = Header->SequenceNumber;
        Out->FileRecords++;
    }
    else if (Owner >= RecordCount)
    {
        Event(NtfsCheckRecordCorrupt, Number, 0);
        return;
    }

    CheckAttributes(Number, Owner, Record);
    if (RecordFaults == Faults && Owner != Number)
    {
        ExtensionOwner[Number] = (ULONG)Owner + 1;
        Sequence[Number] = Header->SequenceNumber;
    }
    /*
     * A file whose record is intact but whose attributes are damaged (a
     * bad header or run list, clusters outside the volume) still has its
     * names, and possibly most of its data. Deleting it would lose all of
     * that. Keep it, and report it: its faults stay structural, so this
     * check makes no index or bitmap repair and the volume stays dirty
     * until it is repaired by hand. Only records that cannot be parsed at
     * all (above) are freed.
     */
    if (RecordFaults != Faults && Owner == Number && Number >= CHECK_FIRST_USER_RECORD)
        Event(NtfsCheckRecordKept, Number, RecordFaults - Faults);
}

void
VolumeCheck::ReportBeyondRange(_In_ ULONGLONG First, _In_ ULONGLONG Count)
{
    if (!Count)
        return;
    Event(NtfsCheckRecordBeyondMft, First, Count);
    BeyondRanges++;
}

/*
 * The record scan stops at the end of the $MFT data, but the MFT bitmap can
 * name records past it. Nothing exists there for a reader: the data size is
 * the end of the file, so such a record reads as zeros. A crash that writes
 * the bitmap but not the $MFT record that grew the data leaves exactly this,
 * and so far the check never looked. Records that are readable and in use
 * there are kept as they are (a later repair could extend the $MFT data to
 * cover them), and index entries that name them are not treated as dangling.
 */
NTSTATUS
VolumeCheck::CheckRecordsBeyondMft()
{
    PUCHAR Record;
    PFileRecordHeader Header;
    ULONGLONG RangeStart = 0, RangeCount = 0;

    BitmapBits = (ULONGLONG)MftBitmapLength * 8;
    if (BitmapBits <= RecordCount)
        return STATUS_SUCCESS;

    LiveBeyondMap = AllocateMap(BitmapBits - RecordCount);
    Record = new(PagedPool, TAG_NTFS) UCHAR[RecordSize];
    if (!LiveBeyondMap || !Record)
    {
        delete[] Record;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Header = reinterpret_cast<PFileRecordHeader>(Record);

    for (ULONGLONG Number = RecordCount; Number < BitmapBits; Number++)
    {
        ULONGLONG DiskOffset;
        BOOLEAN Live;

        if (!TestBit(MftBitmap, Number))
            continue;

        Live = NT_SUCCESS(V->MFT->GetFileRecordDiskOffset(Number, &DiskOffset)) &&
               NT_SUCCESS(V->ReadVolume(DiskOffset, RecordSize, Record)) &&
               RtlCompareMemory(Header->Header.TypeID, "FILE", 4) == 4 &&
               NT_SUCCESS(NtfsApplyFixup(&Header->Header, RecordSize, V->BytesPerSector)) &&
               (Header->Flags & FR_IN_USE) != 0 &&
               (Header->Header.UpdateSequenceOffset < FIELD_OFFSET(FileRecordHeader, MFTRecordNumber) + sizeof(UINT32) ||
                Header->MFTRecordNumber == (UINT32)Number);
        if (Live)
        {
            ReportBeyondRange(RangeStart, RangeCount);
            RangeCount = 0;
            SetBit(LiveBeyondMap, Number - RecordCount);
            Event(NtfsCheckRecordBeyondMftInUse, Number, 0);
            continue;
        }

        if (RangeCount && RangeStart + RangeCount == Number)
        {
            RangeCount++;
        }
        else
        {
            ReportBeyondRange(RangeStart, RangeCount);
            RangeStart = Number;
            RangeCount = 1;
        }
    }
    ReportBeyondRange(RangeStart, RangeCount);

    delete[] Record;
    return STATUS_SUCCESS;
}

BOOLEAN
VolumeCheck::IsLiveBeyondMft(_In_ ULONGLONG Number)
{
    return LiveBeyondMap && Number >= RecordCount && Number < BitmapBits &&
           TestBit(LiveBeyondMap, Number - RecordCount);
}

void
VolumeCheck::RememberCorrupt(_In_ ULONGLONG Number, _In_ ULONG Faults, _In_ BOOLEAN Readable)
{
    if (!RepairRecords)
        return;
    if (CorruptCount == CorruptCapacity)
    {
        ULONG Capacity = CorruptCapacity ? CorruptCapacity * 2 : 16;
        CorruptRecord* Grown = new(PagedPool, TAG_NTFS) CorruptRecord[Capacity];

        if (!Grown)
            return;
        if (Corrupt)
            RtlCopyMemory(Grown, Corrupt, CorruptCount * sizeof(CorruptRecord));
        delete[] Corrupt;
        Corrupt = Grown;
        CorruptCapacity = Capacity;
    }
    Corrupt[CorruptCount].Number = Number;
    Corrupt[CorruptCount].Faults = Faults;
    Corrupt[CorruptCount].Readable = Readable;
    CorruptCount++;
}

void
VolumeCheck::RememberCrossLink(_In_ ULONGLONG Owner, _In_ PAttribute Attr, _In_ ULONGLONG Vcn, _In_ ULONGLONG Lcn, _In_ ULONGLONG Length)
{
    if (!RepairRecords || Owner < CHECK_FIRST_USER_RECORD || Length == 0 || Length > MAXULONG)
        return;
    if (CrossLinkCount == CrossLinkCapacity)
    {
        ULONG Capacity = CrossLinkCapacity ? CrossLinkCapacity * 2 : 8;
        CrossLink* Grown = new(PagedPool, TAG_NTFS) CrossLink[Capacity];

        if (!Grown)
            return;
        if (CrossLinks)
            RtlCopyMemory(Grown, CrossLinks, CrossLinkCount * sizeof(CrossLink));
        delete[] CrossLinks;
        CrossLinks = Grown;
        CrossLinkCapacity = Capacity;
    }

    CrossLinks[CrossLinkCount].Owner = Owner;
    CrossLinks[CrossLinkCount].Vcn = Vcn;
    CrossLinks[CrossLinkCount].Lcn = Lcn;
    CrossLinks[CrossLinkCount].Length = Length;
    CrossLinks[CrossLinkCount].Type = Attr->AttributeType;
    CrossLinks[CrossLinkCount].NameLength = Attr->NameLength;
    RtlCopyMemory(CrossLinks[CrossLinkCount].Name,
                  reinterpret_cast<PUCHAR>(Attr) + Attr->NameOffset,
                  Attr->NameLength * sizeof(WCHAR));
    CrossLinks[CrossLinkCount].Name[Attr->NameLength] = 0;
    CrossLinkCount++;
}

void
VolumeCheck::ResolveCrossLinks()
{
    for (ULONG Index = 0; Index < CrossLinkCount; Index++)
    {
        CrossLink* Link = &CrossLinks[Index];
        PDataRun Target = NULL;
        PFileRecord File;
        ULONGLONG Lcn;
        NTSTATUS Status;

        Status = V->AllocateClusters(0, (ULONG)Link->Length, 1, &Target);
        if (!NT_SUCCESS(Status) || !Target || Target->Length != Link->Length || Target->NextRun)
        {
            if (Target)
            {
                V->ReleaseClusters(Target);
                FreeDataRun(Target);
            }
            continue;
        }
        Lcn = Target->LCN;
        V->ReleaseClusters(Target);
        FreeDataRun(Target);

        if (!NT_SUCCESS(V->MFT->GetFileRecord((ULONG)Link->Owner, &File)))
            continue;
        Status = File->MoveClusters((AttributeType)Link->Type,
                                    Link->NameLength ? Link->Name : NULL,
                                    Link->Vcn,
                                    Lcn,
                                    (ULONG)Link->Length);
        delete File;

        /* The move released the old run, but its shared clusters still belong to the other owner. */
        for (ULONGLONG Cluster = Link->Lcn; NT_SUCCESS(Status) && Cluster < Link->Lcn + Link->Length;)
        {
            DataRun SharedRun = {};

            if (!TestBit(CrossMap, Cluster))
            {
                Cluster++;
                continue;
            }
            SharedRun.LCN = Cluster;
            while (Cluster < Link->Lcn + Link->Length && TestBit(CrossMap, Cluster))
            {
                SharedRun.Length++;
                Cluster++;
            }
            Status = V->ClaimClusters(&SharedRun);
        }

        if (NT_SUCCESS(Status))
        {
            Out->Errors--;
            StructuralErrors--;
            Out->ResolvedCrossLinks++;
            Event(NtfsCheckCrossLinkResolved, Link->Owner, Lcn);
        }
    }
    if (CrossLinkCount)
        V->FlushVolume();
}

void
VolumeCheck::RememberOrphan(_In_ ULONGLONG Number)
{
    if (OrphanCount == OrphanCapacity)
    {
        ULONG Capacity = OrphanCapacity ? OrphanCapacity * 2 : 16;
        ULONGLONG* Grown = new(PagedPool, TAG_NTFS) ULONGLONG[Capacity];

        if (!Grown)
            return;
        if (Orphans)
            RtlCopyMemory(Grown, Orphans, OrphanCount * sizeof(ULONGLONG));
        delete[] Orphans;
        Orphans = Grown;
        OrphanCapacity = Capacity;
    }
    Orphans[OrphanCount++] = Number;
}

NTSTATUS
VolumeCheck::WriteMftBitmap()
{
    PFileRecord MftFile;
    PAttribute BitmapAttr;
    PDataRun Runs;
    NTSTATUS Status;

    Status = V->MFT->GetFileAttributeFromFileRecordNumber(TypeBitmap, NULL, _MFT, &MftFile, &BitmapAttr);
    if (!NT_SUCCESS(Status))
        return Status;

    if (BitmapAttr->IsNonResident)
    {
        Runs = MftFile->FindNonResidentData(BitmapAttr);
        Status = Runs ? WriteStreamRange(Runs, 0, MftBitmapLength, MftBitmap) : STATUS_INSUFFICIENT_RESOURCES;
        if (Runs)
            FreeDataRun(Runs);
    }
    else
    {
        Status = MftFile->ReplaceResidentData(BitmapAttr, MftBitmap, MftBitmapLength);
        if (NT_SUCCESS(Status))
            Status = V->MFT->WriteFileRecordToMFT(MftFile);
    }
    delete MftFile;

    if (NT_SUCCESS(Status))
        Status = V->FlushVolume();
    return Status;
}

/*
 * The corrupt records are already out of InUseMap, DirectoryMap and the
 * name tallies, so the index pass treats every entry that names one as
 * dangling whether or not the record has been freed on disk yet. Their
 * faults therefore no longer block the index repair; the records
 * themselves are freed by RepairRecordsPass once those entries are gone.
 */
void
VolumeCheck::PlanRecordFrees()
{
    for (ULONG Index = 0; Index < CorruptCount; Index++)
        StructuralErrors -= Corrupt[Index].Faults;
}

/* TRUE when an index entry naming this record could not be removed. */
BOOLEAN
VolumeCheck::IsStillReferenced(_In_ ULONGLONG Number)
{
    for (ULONG Index = 0; Index < DanglingCount; Index++)
    {
        if (!Dangling[Index].Removed &&
            (Dangling[Index].Target & 0x0000FFFFFFFFFFFFULL) == Number)
        {
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * Runs after the index repair: a record is freed only when no index
 * entry still names it, so a later allocation can never hand a record
 * that a directory still points at to another file. A record that is
 * still referenced stays marked in use (it is still reported corrupt).
 */
void
VolumeCheck::RepairRecordsPass()
{
    PUCHAR Record = new(PagedPool, TAG_NTFS) UCHAR[RecordSize];
    PFileRecordHeader Header = reinterpret_cast<PFileRecordHeader>(Record);

    if (!Record)
        return;

    for (ULONG Index = 0; Index < CorruptCount; Index++)
    {
        ULONGLONG Number = Corrupt[Index].Number;
        ULONGLONG DiskOffset;
        BOOLEAN Freed = TRUE;

        if (!RepairIndexes || IsStillReferenced(Number))
        {
            StructuralErrors += Corrupt[Index].Faults;
            continue;
        }
        if (Corrupt[Index].Readable)
        {
            Freed = NT_SUCCESS(V->MFT->GetFileRecordDiskOffset(Number, &DiskOffset)) &&
                    NT_SUCCESS(V->ReadVolume(DiskOffset, RecordSize, Record)) &&
                    RtlCompareMemory(Header->Header.TypeID, "FILE", 4) == 4 &&
                    NT_SUCCESS(NtfsApplyFixup(&Header->Header, RecordSize, V->BytesPerSector));
            if (Freed)
            {
                Header->Flags &= ~FR_IN_USE;
                Freed = NT_SUCCESS(NtfsCommitFixup(&Header->Header, RecordSize, V->BytesPerSector)) &&
                        NT_SUCCESS(V->WriteVolume(DiskOffset, RecordSize, Record));
            }
        }
        if (!Freed)
        {
            StructuralErrors += Corrupt[Index].Faults;
            continue;
        }

        MftBitmap[Number >> 3] &= (UCHAR)~(1 << (Number & 7));
        MftBitmapChanged = TRUE;
        Out->FreedRecords++;
        Out->Errors -= Corrupt[Index].Faults;
        Event(NtfsCheckRecordFreed, Number, 0);
    }
    delete[] Record;
    V->FlushVolume();

    /* The MFT bitmap itself is written by CommitMftBitmap, next. */
}

/*
 * Index entries first, MFT bitmap second: if the check stops in between,
 * an index entry can only point at a record that is still marked in use,
 * never at a free record that a later allocation could hand to another
 * file. Bits past the end of the $MFT data are cleared only when every
 * dangling index entry was removed, for the same reason.
 */
void
VolumeCheck::CommitMftBitmap(_In_ BOOLEAN IndexesRepaired)
{
    ULONG BeyondFixes = 0;

    if (IndexesRepaired && BeyondRanges && DanglingFailures == 0)
    {
        for (ULONGLONG Number = RecordCount; Number < BitmapBits; Number++)
        {
            if (TestBit(MftBitmap, Number) && !IsLiveBeyondMft(Number))
            {
                MftBitmap[Number >> 3] &= (UCHAR)~(1 << (Number & 7));
                BeyondFixes++;
            }
        }
        if (BeyondFixes)
            MftBitmapChanged = TRUE;
    }

    if (!MftBitmapChanged)
        return;
    {
        NTSTATUS WriteStatus = WriteMftBitmap();

        if (!NT_SUCCESS(WriteStatus))
        {
            Event(NtfsCheckMftBitmapNotWritten, (ULONG)WriteStatus, MftBitmapLength);
            return;
        }
    }

    Out->Errors -= MftBitmapFixes;
    Out->FixedRecordBitmapBits += MftBitmapFixes + BeyondFixes;
    if (BeyondFixes)
        Out->Errors -= BeyondRanges;
    if (MftBitmapFixes + BeyondFixes)
        Event(NtfsCheckRecordBitmapFixed, MftBitmapFixes + BeyondFixes, 0);
}

void
VolumeCheck::RelinkOrphans()
{
    PUCHAR Record = new(PagedPool, TAG_NTFS) UCHAR[RecordSize];
    PFileRecordHeader Header = reinterpret_cast<PFileRecordHeader>(Record);
    WCHAR NameBuffer[256];

    if (!Record)
        return;

    for (ULONG Index = 0; Index < OrphanCount; Index++)
    {
        ULONGLONG Number = Orphans[Index];
        ULONGLONG DiskOffset;
        ULONG Offset, Relinked = 0, NameCount = 0, DeadNames = 0, DeadParentCount = 0;
        ULONGLONG DeadParents[8];
        PFileNameEx DeadName = NULL;
        BOOLEAN Failed = FALSE, OtherFailures = FALSE;

        if (!NT_SUCCESS(V->MFT->GetFileRecordDiskOffset(Number, &DiskOffset)) ||
            !NT_SUCCESS(V->ReadVolume(DiskOffset, RecordSize, Record)) ||
            RtlCompareMemory(Header->Header.TypeID, "FILE", 4) != 4 ||
            !NT_SUCCESS(NtfsApplyFixup(&Header->Header, RecordSize, V->BytesPerSector)) ||
            Header->ActualSize > RecordSize)
        {
            continue;
        }

        for (Offset = Header->AttributeOffset; Offset + FIELD_OFFSET(Attribute, Resident) + 8 <= Header->ActualSize;)
        {
            PAttribute Attr = reinterpret_cast<PAttribute>(Record + Offset);
            PFileNameEx Name;
            ULONGLONG Parent;
            PFileRecord ParentFile, Found;

            if (Attr->AttributeType == TypeAttributeEndMarker || Attr->Length == 0 ||
                Attr->Length > Header->ActualSize - Offset)
            {
                break;
            }
            Offset += Attr->Length;
            if (Attr->AttributeType != TypeFileName || Attr->IsNonResident)
                continue;

            Name = reinterpret_cast<PFileNameEx>(reinterpret_cast<PUCHAR>(Attr) + Attr->Resident.DataOffset);
            Parent = Name->ParentFileReference & 0x0000FFFFFFFFFFFFULL;
            NameCount++;
            if (Parent >= RecordCount || !TestBit(InUseMap, Parent) || !TestBit(DirectoryMap, Parent) ||
                Sequence[Parent] != (USHORT)(Name->ParentFileReference >> 48))
            {
                ULONG Known;

                if (!DeadName || (DeadName->NameType == NAME_TYPE_DOS &&
                                  Name->ParentFileReference == DeadName->ParentFileReference))
                {
                    DeadName = Name;
                }
                for (Known = 0; Known < DeadParentCount && DeadParents[Known] != Name->ParentFileReference; Known++)
                {
                }
                if (Known == DeadParentCount)
                {
                    if (DeadParentCount < RTL_NUMBER_OF(DeadParents))
                        DeadParents[DeadParentCount++] = Name->ParentFileReference;
                    else
                        OtherFailures = TRUE;
                }
                DeadNames++;
                Failed = TRUE;
                continue;
            }
            NTSTATUS Status = V->MFT->GetFileRecord((ULONG)Parent, &ParentFile);

            if (!NT_SUCCESS(Status))
            {
                Event(NtfsCheckNameRelinkFailed, Number, ((ULONGLONG)(ULONG)Status << 32) | (ULONG)Parent);
                Failed = TRUE;
                OtherFailures = TRUE;
                continue;
            }

            RtlCopyMemory(NameBuffer, Name->Name, Name->NameLength * sizeof(WCHAR));
            NameBuffer[Name->NameLength] = 0;
            if (NT_SUCCESS(V->MFT->GetFileRecordInDirectory(ParentFile, NameBuffer, &Found)))
            {
                BOOLEAN Same = Found->RecordNumber == (UINT32)Number;

                delete Found;
                if (Same)
                {
                    Event(NtfsCheckNameFoundUncounted, Number, Parent);
                    delete ParentFile;
                    continue;
                }
            }

            {
                Directory ParentIndex(V);

                Status = ParentIndex.AddFileToDirectory(ParentFile,
                                                        Number | ((ULONGLONG)Sequence[Number] << 48),
                                                        Name);
                if (NT_SUCCESS(Status))
                {
                    Relinked++;
                    Event(NtfsCheckNameRelinked, Number, Parent);
                }
                else
                {
                    Event(NtfsCheckNameRelinkFailed, Number, ((ULONGLONG)(ULONG)Status << 32) | (ULONG)Parent);
                    Failed = TRUE;
                    OtherFailures = TRUE;
                }
            }
            delete ParentFile;
        }

        if (DeadNames && DeadNames < NameCount && !OtherFailures)
        {
            ULONG RemovedTotal = 0, Removed;
            PFileRecord File;

            if (NT_SUCCESS(V->MFT->GetFileRecord((ULONG)Number, &File)))
            {
                for (ULONG Dead = 0; Dead < DeadParentCount; Dead++)
                {
                    if (NT_SUCCESS(V->MFT->RemoveFileNamesUnderParent(File, DeadParents[Dead], &Removed)))
                        RemovedTotal += Removed;
                }
                delete File;
            }
            if (RemovedTotal == DeadNames)
            {
                Out->Errors--;
                Out->RemovedNames += RemovedTotal;
                Out->RelinkedNames += Relinked;
                Event(NtfsCheckLostNameRemoved, Number, RemovedTotal);
            }
        }
        else if (Relinked && !Failed)
        {
            Out->Errors--;
            Out->RelinkedNames += Relinked;
        }
        else if (!Relinked && DeadName && DeadNames == NameCount)
        {
            RememberLost(Number, DeadName);
        }
    }

    delete[] Record;
    if (OrphanCount)
        V->FlushVolume();
}

void
VolumeCheck::RememberNameKey(_In_ ULONGLONG Owner, _In_ ULONG Hash)
{
    if (NameKeysIncomplete)
        return;
    if (NameKeyCount == NameKeyCapacity)
    {
        ULONG Capacity = NameKeyCapacity ? NameKeyCapacity * 2 : 1024;
        ULONGLONG* Grown = Capacity > NameKeyCapacity ? new(PagedPool, TAG_NTFS) ULONGLONG[Capacity] : NULL;

        if (!Grown)
        {
            NameKeysIncomplete = TRUE;
            return;
        }
        if (NameKeys)
            RtlCopyMemory(Grown, NameKeys, NameKeyCount * sizeof(ULONGLONG));
        delete[] NameKeys;
        NameKeys = Grown;
        NameKeyCapacity = Capacity;
    }
    NameKeys[NameKeyCount++] = (Owner << 32) | Hash;
}

void
VolumeCheck::SortNameKeys()
{
    ULONG Count = NameKeyCount;

    if (Count < 2)
        return;
    for (ULONG Start = Count / 2; Start-- > 0;)
    {
        for (ULONG Root = Start; 2 * Root + 1 < Count;)
        {
            ULONG Child = 2 * Root + 1;

            if (Child + 1 < Count && NameKeys[Child] < NameKeys[Child + 1])
                Child++;
            if (NameKeys[Root] >= NameKeys[Child])
                break;
            ULONGLONG Swap = NameKeys[Root];
            NameKeys[Root] = NameKeys[Child];
            NameKeys[Child] = Swap;
            Root = Child;
        }
    }
    for (ULONG End = Count - 1; End > 0; End--)
    {
        ULONGLONG Swap = NameKeys[0];
        NameKeys[0] = NameKeys[End];
        NameKeys[End] = Swap;
        for (ULONG Root = 0; 2 * Root + 1 < End;)
        {
            ULONG Child = 2 * Root + 1;

            if (Child + 1 < End && NameKeys[Child] < NameKeys[Child + 1])
                Child++;
            if (NameKeys[Root] >= NameKeys[Child])
                break;
            Swap = NameKeys[Root];
            NameKeys[Root] = NameKeys[Child];
            NameKeys[Child] = Swap;
            Root = Child;
        }
    }
}

BOOLEAN
VolumeCheck::HasNameKey(_In_ ULONGLONG Owner, _In_ ULONG Hash)
{
    ULONGLONG Key = (Owner << 32) | Hash;
    ULONG Low = 0, High = NameKeyCount;

    if (NameKeysIncomplete)
        return TRUE;
    while (Low < High)
    {
        ULONG Middle = Low + (High - Low) / 2;

        if (NameKeys[Middle] < Key)
            Low = Middle + 1;
        else
            High = Middle;
    }
    return Low < NameKeyCount && NameKeys[Low] == Key;
}

/* Matches an index entry to one name of the file. Each name is counted once:
 * an entry for a name that is counted already is a second copy. */
VolumeCheck::NameMatch
VolumeCheck::ConsumeNameKey(_In_ ULONGLONG Owner, _In_ ULONG Hash)
{
    ULONGLONG Key = (Owner << 32) | Hash;
    ULONG Low = 0, High = NameKeyCount;

    if (NameKeysIncomplete || !NameKeyUsed)
        return HasNameKey(Owner, Hash) ? NameFresh : NameAbsent;
    while (Low < High)
    {
        ULONG Middle = Low + (High - Low) / 2;

        if (NameKeys[Middle] < Key)
            Low = Middle + 1;
        else
            High = Middle;
    }
    if (Low >= NameKeyCount || NameKeys[Low] != Key)
        return NameAbsent;
    for (ULONG Index = Low; Index < NameKeyCount && NameKeys[Index] == Key; Index++)
    {
        if (!TestBit(NameKeyUsed, Index))
        {
            SetBit(NameKeyUsed, Index);
            return NameFresh;
        }
    }
    return NameRepeated;
}

struct ListFilterContext
{
    VolumeCheck* Check;
    ULONGLONG Base;
};

BOOLEAN
VolumeCheck::KeepListEntry(_In_ void* Context, _In_ const AttributeListEx* Entry)
{
    ListFilterContext* Filter = static_cast<ListFilterContext*>(Context);
    VolumeCheck* Check = Filter->Check;
    ULONGLONG Record = Entry->BaseFileRef & 0x0000FFFFFFFFFFFFULL;
    USHORT SequenceNumber = (USHORT)(Entry->BaseFileRef >> 48);

    if (Record == Filter->Base)
        return TRUE;
    if (Record >= Check->RecordCount || Check->ExtensionOwner[Record] != (ULONG)Filter->Base + 1)
        return FALSE;
    return SequenceNumber == 0 || Check->Sequence[Record] == SequenceNumber;
}

/* TRUE if some record names Base as its base: an in-use one, valid or
 * faulted, or a FILE record whose fixup fails. */
BOOLEAN
VolumeCheck::HasExtensions(_In_ ULONGLONG Base)
{
    return TestBit(ExtendedMap, Base);
}

void
VolumeCheck::CheckAttributeLists()
{
    ULONGLONG Rebuild[CHECK_LIST_REBUILDS];
    ULONG RebuildCount = 0;

    for (ULONGLONG Number = 0; Number < RecordCount; Number++)
    {
        ListFilterContext Filter = { this, Number };
        ULONG Dropped, DroppedNames;
        BOOLEAN DroppedData;
        BOOLEAN Unneeded;
        PFileRecord File;
        NTSTATUS Status;

        if (!TestBit(AttrListMap, Number) || !TestBit(InUseMap, Number))
            continue;
        if (!NT_SUCCESS(V->MFT->GetFileRecord((ULONG)Number, &File)))
            continue;

        Status = File->FilterAttributeList(KeepListEntry, &Filter, FALSE, &Dropped, &DroppedNames, &DroppedData);
        if (!NT_SUCCESS(Status) && Status != STATUS_INSUFFICIENT_RESOURCES)
        {
            /* With no extension record, every attribute is in the base record
             * and the list carries nothing: removing it makes the attributes
             * readable again. A list with extensions is written again from
             * the records once they are all known. */
            Event(NtfsCheckAttributeListUnreadable, Number, (ULONG)Status);
            if (RepairRecords && !HasExtensions(Number) && NT_SUCCESS(File->RemoveAttributeList()))
            {
                Out->Errors--;
                Out->RepairedAttributeLists++;
                Event(NtfsCheckAttributeListRemoved, Number, 0);
            }
            else if (RepairRecords && HasExtensions(Number) && RebuildCount < CHECK_LIST_REBUILDS)
            {
                Rebuild[RebuildCount++] = Number;
            }
        }
        else if (NT_SUCCESS(Status) && Dropped)
        {
            Event(NtfsCheckAttributeListBad, Number, Dropped);
            if (RepairRecords &&
                NT_SUCCESS(File->FilterAttributeList(KeepListEntry, &Filter, TRUE, &Dropped, &DroppedNames, &DroppedData)))
            {
                Out->Errors--;
                Out->RepairedAttributeLists++;
                Event(NtfsCheckAttributeListFixed, Number, Dropped);
                if (DroppedData && !(File->Header->Flags & FR_IS_DIRECTORY))
                    Event(NtfsCheckDataAttributeInserted, Number, 0);
            }
        }

        /* A list that only names the base record is one Windows rejects */
        if (NT_SUCCESS(File->DropUnneededAttributeList(FALSE, &Unneeded)) && Unneeded)
        {
            Event(NtfsCheckAttributeListUnneeded, Number, 0);
            if (RepairRecords && NT_SUCCESS(File->DropUnneededAttributeList(TRUE, &Unneeded)))
            {
                Out->Errors--;
                Out->RepairedAttributeLists++;
                Event(NtfsCheckAttributeListRemoved, Number, 0);
            }
        }
        delete File;
    }
    if (RebuildCount)
        RebuildAttributeLists(Rebuild, RebuildCount);
    if (RepairRecords)
        V->FlushVolume();
}

void
VolumeCheck::RebuildAttributeLists(_In_reads_(Count) const ULONGLONG* Bases, _In_ ULONG Count)
{
    ULONGLONG* Extensions = new(PagedPool, TAG_NTFS) ULONGLONG[Count * CHECK_LIST_EXTENSIONS];
    ULONG ExtensionCount[CHECK_LIST_REBUILDS] = {};
    BOOLEAN Usable[CHECK_LIST_REBUILDS];

    if (!Extensions)
        return;
    for (ULONG Base = 0; Base < Count; Base++)
        Usable[Base] = TRUE;

    /* One pass over the MFT finds every record that names one of the bases.
     * A list is only rebuilt when all of them can be read: a torn extension
     * held attributes that a new list would drop. */
    for (ULONGLONG First = 0; First < RecordCount; First += CHECK_RECORD_CHUNK)
    {
        ULONG Chunk = (ULONG)min((ULONGLONG)CHECK_RECORD_CHUNK, RecordCount - First);

        if (!NT_SUCCESS(ReadRecords(First, Chunk)))
        {
            for (ULONG Base = 0; Base < Count; Base++)
                Usable[Base] = FALSE;
            break;
        }
        for (ULONG Index = 0; Index < Chunk; Index++)
        {
            PFileRecordHeader Header = reinterpret_cast<PFileRecordHeader>(Records + Index * RecordSize);
            BOOLEAN IsFile = RtlCompareMemory(Header->Header.TypeID, "FILE", 4) == 4;
            ULONGLONG Owner;

            if (!IsFile && RtlCompareMemory(Header->Header.TypeID, "BAAD", 4) != 4)
                continue;
            Owner = Header->BaseFileRecord & 0x0000FFFFFFFFFFFFULL;
            if (Owner == 0)
                continue;
            for (ULONG Base = 0; Base < Count; Base++)
            {
                if (Bases[Base] != Owner)
                    continue;
                if (!IsFile || !NT_SUCCESS(NtfsApplyFixup(&Header->Header, RecordSize, V->BytesPerSector)))
                {
                    Usable[Base] = FALSE;
                }
                else if ((Header->Flags & FR_IN_USE) && (USHORT)(Header->BaseFileRecord >> 48) == Sequence[Owner])
                {
                    if (ExtensionCount[Base] == CHECK_LIST_EXTENSIONS)
                        Usable[Base] = FALSE;
                    else
                        Extensions[Base * CHECK_LIST_EXTENSIONS + ExtensionCount[Base]++] = First + Index;
                }
                break;
            }
        }
    }

    for (ULONG Base = 0; Base < Count; Base++)
    {
        PFileRecord Parts[CHECK_LIST_EXTENSIONS] = {};
        PFileRecord File = NULL;
        ULONG Loaded = 0;
        NTSTATUS Status;

        if (!Usable[Base] || ExtensionCount[Base] == 0)
            continue;
        Status = V->MFT->GetFileRecord((ULONG)Bases[Base], &File);
        while (NT_SUCCESS(Status) && Loaded < ExtensionCount[Base])
        {
            Status = V->MFT->GetFileRecord((ULONG)Extensions[Base * CHECK_LIST_EXTENSIONS + Loaded], &Parts[Loaded]);
            if (NT_SUCCESS(Status))
                Loaded++;
        }
        if (NT_SUCCESS(Status))
            Status = File->RebuildAttributeList(Parts, Loaded);
        if (NT_SUCCESS(Status))
        {
            Out->Errors--;
            Out->RepairedAttributeLists++;
            Event(NtfsCheckAttributeListRebuilt, Bases[Base], Loaded);
        }
        for (ULONG Index = 0; Index < Loaded; Index++)
            delete Parts[Index];
        delete File;
    }
    delete[] Extensions;
}

void
VolumeCheck::RememberLost(_In_ ULONGLONG Number, _In_ const FileNameEx* Name)
{
    if (LostCount == LostCapacity)
    {
        ULONG Capacity = LostCapacity ? LostCapacity * 2 : 16;
        LostFile* Grown = new(PagedPool, TAG_NTFS) LostFile[Capacity];

        if (!Grown)
            return;
        if (Lost)
            RtlCopyMemory(Grown, Lost, LostCount * sizeof(LostFile));
        delete[] Lost;
        Lost = Grown;
        LostCapacity = Capacity;
    }
    Lost[LostCount].Number = Number;
    Lost[LostCount].Parent = Name->ParentFileReference;
    Lost[LostCount].NameLength = Name->NameLength;
    RtlCopyMemory(Lost[LostCount].Name, Name->Name, Name->NameLength * sizeof(WCHAR));
    Lost[LostCount].Name[Name->NameLength] = 0;
    LostCount++;
}

static const UCHAR LostAndFoundSecurity[] =
{
    0x01, 0x00, 0x04, 0x90, 0x48, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x14, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x34, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x14, 0x00, 0xFF, 0x01, 0x1F, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
    0x12, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x18, 0x00, 0xFF, 0x01, 0x1F, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
    0x20, 0x00, 0x00, 0x00, 0x20, 0x02, 0x00, 0x00,
    0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x20, 0x00, 0x00, 0x00, 0x20, 0x02, 0x00, 0x00,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x12, 0x00, 0x00, 0x00
};

static void
FormatDecimal(_Out_ PWCHAR Out, _In_ ULONG Value, _In_ ULONG Digits)
{
    while (Digits--)
    {
        Out[Digits] = (WCHAR)(L'0' + Value % 10);
        Value /= 10;
    }
}

static NTSTATUS
CreateLostDirectory(_In_ PVolume V,
                    _In_ PFileRecord Parent,
                    _In_ PWCHAR Name,
                    _In_ ULONG FileAttributes,
                    _In_ UCHAR Control,
                    _Out_ PFileRecord* Directory)
{
    UCHAR Security[sizeof(LostAndFoundSecurity)];
    NTSTATUS Status;

    Status = V->MFT->CreateFileInDirectory(Parent, Name, (ULONG)NtfsWcsLen(Name), TRUE,
                                           FileAttributes, FALSE, Directory);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlCopyMemory(Security, LostAndFoundSecurity, sizeof(Security));
    Security[3] = Control;
    Status = (*Directory)->SetSecurityDescriptor(Security, sizeof(Security));
    if (!NT_SUCCESS(Status))
    {
        delete *Directory;
        *Directory = NULL;
    }
    return Status;
}

void
VolumeCheck::RecoverLostFiles()
{
    static const WCHAR Hex[] = L"0123456789ABCDEF";
    WCHAR FoundName[] = L"found.000";
    WCHAR GroupName[] = L"dir0000.chk";
    WCHAR NameBuffer[NTFS_MAX_FILE_NAME_LENGTH + 1];
    PFileRecord Root, Found = NULL, Existing;
    ULONG FoundIndex, Singles = 0, Groups = 0;

    if (!LostCount || !NT_SUCCESS(V->MFT->GetFileRecord(_Root, &Root)))
        return;

    for (FoundIndex = 0; FoundIndex < 1000; FoundIndex++)
    {
        FormatDecimal(FoundName + 6, FoundIndex, 3);
        if (!NT_SUCCESS(V->MFT->GetFileRecordInDirectory(Root, FoundName, &Existing)))
            break;
        delete Existing;
    }
    if (FoundIndex == 1000 ||
        !NT_SUCCESS(CreateLostDirectory(V, Root, FoundName, FILE_PERM_HIDDEN | FILE_PERM_SYSTEM, 0x90, &Found)))
    {
        delete Root;
        return;
    }
    delete Root;
    Event(NtfsCheckLostAndFound, Found->RecordNumber, FoundIndex);

    for (ULONG Index = 0; Index < LostCount; Index++)
    {
        ULONG Members = 0, First;
        PFileRecord Target = NULL;

        for (First = 0; First < LostCount && Lost[First].Parent != Lost[Index].Parent; First++)
        {
        }
        if (First < Index)
            continue;
        for (ULONG Other = Index; Other < LostCount; Other++)
        {
            if (Lost[Other].Parent == Lost[Index].Parent)
                Members++;
        }

        if (Members > 1)
        {
            PFileRecord Refreshed;

            FormatDecimal(GroupName + 3, Groups++, 4);
            if (!NT_SUCCESS(V->MFT->GetFileRecord(Found->RecordNumber, &Refreshed)))
                continue;
            if (!NT_SUCCESS(CreateLostDirectory(V, Refreshed, GroupName, 0, 0x80, &Target)))
            {
                delete Refreshed;
                continue;
            }
            delete Refreshed;
        }

        for (ULONG Other = Index; Other < LostCount; Other++)
        {
            PFileRecord File, Parent;
            PWCHAR NewName = NULL;
            NTSTATUS Status;

            if (Lost[Other].Parent != Lost[Index].Parent)
                continue;
            if (Members == 1)
            {
                for (ULONG Digit = 0; Digit < 8; Digit++)
                    NameBuffer[Digit] = Hex[(Singles >> (Digit * 4)) & 15];
                NameBuffer[8] = L'-';
                RtlCopyMemory(NameBuffer + 9, Lost[Other].Name,
                              min((ULONG)Lost[Other].NameLength, (ULONG)NTFS_MAX_FILE_NAME_LENGTH - 9) * sizeof(WCHAR));
                NameBuffer[9 + min((ULONG)Lost[Other].NameLength, (ULONG)NTFS_MAX_FILE_NAME_LENGTH - 9)] = 0;
                NewName = NameBuffer;
                Singles++;
            }

            if (!NT_SUCCESS(V->MFT->GetFileRecord((ULONG)Lost[Other].Number, &File)))
                continue;
            if (!NT_SUCCESS(V->MFT->GetFileRecord(Target ? Target->RecordNumber
                                                         : Found->RecordNumber,
                                                  &Parent)))
            {
                delete File;
                continue;
            }
            Status = V->MFT->ReconnectFile(File, Lost[Other].Parent, Parent, NewName);
            delete Parent;
            delete File;
            if (NT_SUCCESS(Status))
            {
                Out->Errors--;
                Out->RecoveredFiles++;
                Event(NtfsCheckFileRecovered, Lost[Other].Number,
                      Target ? Target->RecordNumber : Found->RecordNumber);
            }
        }
        delete Target;
    }

    delete Found;
    V->FlushVolume();
}

NTSTATUS
VolumeCheck::RepairRange(_In_ BOOLEAN Claim, _In_ ULONGLONG Lcn, _In_ ULONGLONG Count)
{
    DataRun Run;
    NTSTATUS Status;

    Run.NextRun = NULL;
    Run.LCN = Lcn;
    Run.Length = Count;
    Run.IsSparse = FALSE;
    Status = Claim ? V->ClaimClusters(&Run) : V->ReleaseClusters(&Run);
    if (NT_SUCCESS(Status))
    {
        if (Claim)
        {
            Out->ClaimedClusters += Count;
            Event(NtfsCheckUsedClustersClaimed, Lcn, Count);
        }
        else
        {
            Out->FreedClusters += Count;
            Event(NtfsCheckLostClustersFreed, Lcn, Count);
        }
    }
    return Status;
}

BOOLEAN
VolumeCheck::AddRange(_Inout_ RangeList* List, _In_ ULONGLONG Lcn)
{
    if (List->Count && List->Ranges[List->Count - 1].Lcn + List->Ranges[List->Count - 1].Length == Lcn)
    {
        List->Ranges[List->Count - 1].Length++;
        return TRUE;
    }
    if (List->Count == List->Capacity)
    {
        ULONG Capacity = List->Capacity ? List->Capacity * 2 : 64;
        ClusterRange* Grown = new(PagedPool, TAG_NTFS) ClusterRange[Capacity];

        if (!Grown)
            return FALSE;
        if (List->Ranges)
            RtlCopyMemory(Grown, List->Ranges, List->Count * sizeof(ClusterRange));
        delete[] List->Ranges;
        List->Ranges = Grown;
        List->Capacity = Capacity;
    }
    List->Ranges[List->Count].Lcn = Lcn;
    List->Ranges[List->Count].Length = 1;
    List->Count++;
    return TRUE;
}

NTSTATUS
VolumeCheck::CheckBitmap(_In_ BOOLEAN Repair)
{
    PUCHAR Chunk;
    ULONGLONG Lcn = 0;
    RangeList Lost = {}, Missing = {};
    NTSTATUS Status = STATUS_SUCCESS;

    Chunk = new(PagedPool, TAG_NTFS) UCHAR[CHECK_BITMAP_CHUNK];
    if (!Chunk)
        return STATUS_INSUFFICIENT_RESOURCES;

    while (NT_SUCCESS(Status) && Lcn < ClusterCount)
    {
        ULONGLONG Returned, Size;
        ULONG Length = CHECK_BITMAP_CHUNK;

        Status = V->ReadBitmap(Lcn, &Returned, &Size, Chunk, &Length);
        if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW)
            break;
        Status = STATUS_SUCCESS;
        if (Returned != Lcn || Length == 0)
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            break;
        }

        for (ULONG Bit = 0; Bit < Length * 8 && Lcn < ClusterCount && NT_SUCCESS(Status); Bit++, Lcn++)
        {
            BOOLEAN Marked = TestBit(Chunk, Bit);
            BOOLEAN Used = TestBit(UsedMap, Lcn);

            if (Marked != Used && !AddRange(Used ? &Missing : &Lost, Lcn))
                Status = STATUS_INSUFFICIENT_RESOURCES;
            if (Marked && !Used)
                Out->LostClusters++;
        }
    }
    delete[] Chunk;

    if (NT_SUCCESS(Status))
    {
        /*
         * Claiming only ever sets bits: at worst it keeps space a damaged
         * record named, while a clear bit under live data lets the allocator
         * hand that data's clusters to another file. So the claim does not
         * wait for the structural findings. Freeing is safe only when every
         * run list was parsed, so it still does.
         */
        BOOLEAN Claim = Repair && !V->IsReadOnly;
        BOOLEAN Free = Claim && StructuralErrors == 0;

        for (ULONG Index = 0; Index < Missing.Count; Index++)
            Event(NtfsCheckUsedClustersFree, Missing.Ranges[Index].Lcn, Missing.Ranges[Index].Length);
        if (Out->LostClusters)
            Event(NtfsCheckLostClusters, Out->LostClusters, Lost.Count);

        for (ULONG Index = 0; Claim && NT_SUCCESS(Status) && Index < Missing.Count; Index++)
            Status = RepairRange(TRUE, Missing.Ranges[Index].Lcn, Missing.Ranges[Index].Length);
        if (Claim && NT_SUCCESS(Status))
            Out->Errors -= Missing.Count;
        for (ULONG Index = 0; Free && NT_SUCCESS(Status) && Index < Lost.Count; Index++)
            Status = RepairRange(FALSE, Lost.Ranges[Index].Lcn, Lost.Ranges[Index].Length);
        if (Free && NT_SUCCESS(Status) && Out->LostClusters)
            Out->Errors--;
    }

    delete[] Lost.Ranges;
    delete[] Missing.Ranges;
    return Status;
}

BOOLEAN
VolumeCheck::RecordHasName(_In_ ULONGLONG Ordinal, _In_ const FileNameEx* Key, _In_ ULONG KeyLength)
{
    PUCHAR Record = new(PagedPool, TAG_NTFS) UCHAR[RecordSize];
    PFileRecordHeader Header = reinterpret_cast<PFileRecordHeader>(Record);
    ULONGLONG DiskOffset;
    ULONG Offset, NameBytes = FIELD_OFFSET(FileNameEx, Name) + Key->NameLength * sizeof(WCHAR);
    BOOLEAN Found = FALSE;

    if (!Record)
        return FALSE;
    if (NameBytes > KeyLength ||
        !NT_SUCCESS(V->MFT->GetFileRecordDiskOffset(Ordinal, &DiskOffset)) ||
        !NT_SUCCESS(V->ReadVolume(DiskOffset, RecordSize, Record)) ||
        RtlCompareMemory(Header->Header.TypeID, "FILE", 4) != 4 ||
        !NT_SUCCESS(NtfsApplyFixup(&Header->Header, RecordSize, V->BytesPerSector)) ||
        Header->ActualSize > RecordSize)
    {
        delete[] Record;
        return FALSE;
    }

    for (Offset = Header->AttributeOffset; !Found && Offset + FIELD_OFFSET(Attribute, Resident) + 8 <= Header->ActualSize;)
    {
        PAttribute Attr = reinterpret_cast<PAttribute>(Record + Offset);

        if (Attr->AttributeType == TypeAttributeEndMarker || Attr->Length == 0 ||
            Attr->Length > Header->ActualSize - Offset)
        {
            break;
        }
        if (Attr->AttributeType == TypeFileName && !Attr->IsNonResident &&
            Attr->Resident.DataLength >= NameBytes &&
            (ULONG)Attr->Resident.DataOffset + NameBytes <= Attr->Length)
        {
            const FileNameEx* Name =
                reinterpret_cast<const FileNameEx*>(reinterpret_cast<PUCHAR>(Attr) + Attr->Resident.DataOffset);

            Found = Name->ParentFileReference == Key->ParentFileReference &&
                    Name->NameLength == Key->NameLength &&
                    Name->NameType == Key->NameType &&
                    RtlCompareMemory(Name->Name, Key->Name, Key->NameLength * sizeof(WCHAR)) ==
                        Key->NameLength * sizeof(WCHAR);
        }
        Offset += Attr->Length;
    }

    delete[] Record;
    return Found;
}

NTSTATUS
VolumeCheck::WriteStreamRange(_In_ PDataRun Runs, _In_ ULONGLONG Offset, _In_ ULONG Length, _In_ PUCHAR Data)
{
    ULONGLONG RunStart = 0;
    NTSTATUS Status;

    for (PDataRun Run = Runs; Run && Length; Run = Run->NextRun)
    {
        ULONGLONG RunBytes = Run->Length * ClusterSize;

        if (Offset < RunStart + RunBytes)
        {
            ULONGLONG Within = Offset - RunStart;
            ULONG Chunk = (ULONG)min((ULONGLONG)Length, RunBytes - Within);

            if (Run->IsSparse)
                return STATUS_FILE_CORRUPT_ERROR;
            Status = V->WriteVolume(Run->LCN * ClusterSize + Within, Chunk, Data);
            if (!NT_SUCCESS(Status))
                return Status;
            Offset += Chunk;
            Data += Chunk;
            Length -= Chunk;
        }
        RunStart += RunBytes;
    }
    return Length ? STATUS_FILE_CORRUPT_ERROR : STATUS_SUCCESS;
}

ULONG
VolumeCheck::CheckIndexEntries(_In_ ULONGLONG Directory,
                               _In_ PIndexNodeHeader Node,
                               _In_ ULONG Available,
                               _In_ ULONGLONG Vcn,
                               _In_ BOOLEAN AllowFix,
                               _In_ BOOLEAN AllowCut)
{
    PUCHAR Base = reinterpret_cast<PUCHAR>(Node);
    ULONG Offset = Node->IndexOffset;
    ULONG End = Node->TotalIndexSize;
    ULONG Fixed = 0;
    BOOLEAN Dangles;

    if (End > Available || Offset >= End || (Offset & 7) != 0)
    {
        Event(NtfsCheckIndexCorrupt, Directory, Vcn);
        return 0;
    }

    while (Offset + FIELD_OFFSET(IndexEntry, IndexStream) <= End)
    {
        PIndexEntry Entry = reinterpret_cast<PIndexEntry>(Base + Offset);
        const FileNameEx* Key = reinterpret_cast<const FileNameEx*>(Entry->IndexStream);
        ULONGLONG Target, Ordinal;

        if (Entry->EntryLength < FIELD_OFFSET(IndexEntry, IndexStream) || (Entry->EntryLength & 7) != 0 ||
            Entry->EntryLength > End - Offset ||
            Entry->StreamLength > Entry->EntryLength - FIELD_OFFSET(IndexEntry, IndexStream))
        {
            Event(NtfsCheckIndexCorrupt, Directory, Vcn);
            return Fixed;
        }
        if (Entry->Flags & INDEX_ENTRY_END)
            return Fixed;
        if (Entry->StreamLength < FIELD_OFFSET(FileNameEx, Name) ||
            Entry->StreamLength < FIELD_OFFSET(FileNameEx, Name) + Key->NameLength * sizeof(WCHAR))
        {
            Event(NtfsCheckIndexCorrupt, Directory, Vcn);
            return Fixed;
        }

        Out->IndexEntries++;
        Dangles = FALSE;
        Target = Entry->Data.Directory.IndexedFile;
        Ordinal = Target & 0x0000FFFFFFFFFFFFULL;
        if (Ordinal < RecordCount && TestBit(InUseMap, Ordinal) &&
            Sequence[Ordinal] == (USHORT)(Target >> 48) &&
            (Key->ParentFileReference & 0x0000FFFFFFFFFFFFULL) == Directory)
        {
            NameMatch Match = ConsumeNameKey(Ordinal, HashName(Key));

            if (Match == NameRepeated)
            {
                /* This name is counted already: a second copy, which a cut
                 * can leave in a node and an ancestor. */
                Event(NtfsCheckIndexDuplicateEntry, Directory, Target);
                if (RepairIndexes)
                    RememberDangling(Directory, Target, Key);
            }
            else if (Match == NameFresh)
            {
                Names[Ordinal].Found++;
                Names[Ordinal].FoundHash += HashName(Key);
            }
            else
            {
                Event(NtfsCheckIndexNameMissing, Directory, Target);
                Dangles = TRUE;
            }
        }
        else if (Ordinal < RecordCount && TestBit(InUseMap, Ordinal) &&
                 (Key->ParentFileReference & 0x0000FFFFFFFFFFFFULL) == Directory &&
                 RecordHasName(Ordinal, Key, Entry->StreamLength))
        {
            Names[Ordinal].Found++;
            Names[Ordinal].FoundHash += HashName(Key);
            Event(NtfsCheckIndexStaleEntry, Directory, Target);
            if (AllowFix && RepairIndexes)
            {
                Entry->Data.Directory.IndexedFile = Ordinal | ((ULONGLONG)Sequence[Ordinal] << 48);
                Fixed++;
            }
        }
        else if (IsLiveBeyondMft(Ordinal))
        {
            /* The file exists but lies past the $MFT data; keep its name. */
            Event(NtfsCheckIndexBadEntry, Directory, Target);
        }
        else
        {
            Event(NtfsCheckIndexBadEntry, Directory, Target);
            Dangles = TRUE;
        }

        /*
         * A dangling leaf entry in an index block is cut where it stands:
         * the block is written back by the caller. That needs no lookup by
         * name, which a damaged or duplicate-name index can defeat. Entries
         * in the root and in internal nodes (they carry a child pointer)
         * go through RemoveDangling and the B+ tree code instead.
         */
        if (Dangles && RepairIndexes)
        {
            ULONG Length = Entry->EntryLength;

            if (RememberDangling(Directory, Target, Key) &&
                AllowCut && !(Entry->Flags & INDEX_ENTRY_NODE))
            {
                Dangling[DanglingCount - 1].Pending = TRUE;
                RtlMoveMemory(Base + Offset, Base + Offset + Length, End - Offset - Length);
                RtlZeroMemory(Base + End - Length, Length);
                End -= Length;
                Node->TotalIndexSize = End;
                PendingCuts++;
                continue;
            }
        }
        Offset += Entry->EntryLength;
    }

    Event(NtfsCheckIndexCorrupt, Directory, Vcn);
    return Fixed;
}

BOOLEAN
VolumeCheck::RememberDangling(_In_ ULONGLONG Directory, _In_ ULONGLONG Target, _In_ const FileNameEx* Key)
{
    if (DanglingCount == DanglingCapacity)
    {
        ULONG Capacity = DanglingCapacity ? DanglingCapacity * 2 : 16;
        DanglingEntry* Grown = new(PagedPool, TAG_NTFS) DanglingEntry[Capacity];

        if (!Grown)
        {
            DanglingFailures++;
            return FALSE;
        }
        if (Dangling)
            RtlCopyMemory(Grown, Dangling, DanglingCount * sizeof(DanglingEntry));
        delete[] Dangling;
        Dangling = Grown;
        DanglingCapacity = Capacity;
    }

    Dangling[DanglingCount].Directory = Directory;
    Dangling[DanglingCount].Target = Target;
    Dangling[DanglingCount].NameLength = Key->NameLength;
    Dangling[DanglingCount].Pending = FALSE;
    Dangling[DanglingCount].Removed = FALSE;
    RtlCopyMemory(Dangling[DanglingCount].Name, Key->Name, Key->NameLength * sizeof(WCHAR));
    DanglingCount++;
    return TRUE;
}

/*
 * Called once the index block holding the entries cut by CheckIndexEntries
 * was written (or failed to be). A failed write leaves them on disk, so
 * RemoveDangling tries them again by name.
 */
void
VolumeCheck::SettlePendingCuts(_In_ BOOLEAN Written)
{
    for (ULONG Index = DanglingCount; PendingCuts && Index-- > 0;)
    {
        DanglingEntry* Entry = &Dangling[Index];

        if (!Entry->Pending)
            continue;
        Entry->Pending = FALSE;
        PendingCuts--;
        if (!Written)
            continue;
        Entry->Removed = TRUE;
        Out->Errors--;
        Out->RemovedIndexEntries++;
        Event(NtfsCheckIndexEntryRemoved, Entry->Directory, Entry->Target);
    }
    PendingCuts = 0;
}

void
VolumeCheck::RemoveDangling()
{
    ULONG Attempted = 0;

    for (ULONG Index = 0; Index < DanglingCount; Index++)
    {
        DanglingEntry* Entry = &Dangling[Index];
        UNICODE_STRING Name;
        PFileRecord File;
        NTSTATUS Status;

        if (Entry->Removed)
            continue;
        Attempted++;
        Status = V->MFT->GetFileRecord((ULONG)Entry->Directory, &File);
        if (!NT_SUCCESS(Status))
        {
            DanglingFailures++;
            Event(NtfsCheckIndexEntryNotRemoved, Entry->Directory, Entry->Target);
            continue;
        }

        Name.Buffer = Entry->Name;
        Name.Length = Name.MaximumLength = (USHORT)(Entry->NameLength * sizeof(WCHAR));
        {
            Directory Index(V);

            Status = Index.RemoveFileFromDirectory(File, Entry->Target, &Name);
        }
        delete File;

        if (NT_SUCCESS(Status))
        {
            Entry->Removed = TRUE;
            Out->Errors--;
            Out->RemovedIndexEntries++;
            Event(NtfsCheckIndexEntryRemoved, Entry->Directory, Entry->Target);
        }
        else
        {
            DanglingFailures++;
            Event(NtfsCheckIndexEntryNotRemoved, Entry->Directory, Entry->Target);
        }
    }
    if (Attempted)
        V->FlushVolume();
}

BOOLEAN
VolumeCheck::KeyName(_In_ PIndexEntry Entry, _Out_ PUNICODE_STRING Name)
{
    const FileNameEx* Key = reinterpret_cast<const FileNameEx*>(Entry->IndexStream);

    if (Entry->StreamLength < FIELD_OFFSET(FileNameEx, Name) ||
        Entry->StreamLength > Entry->EntryLength - FIELD_OFFSET(IndexEntry, IndexStream) ||
        Entry->StreamLength < FIELD_OFFSET(FileNameEx, Name) + Key->NameLength * sizeof(WCHAR))
    {
        return FALSE;
    }
    Name->Buffer = const_cast<PWSTR>(Key->Name);
    Name->Length = Name->MaximumLength = (USHORT)(Key->NameLength * sizeof(WCHAR));
    return TRUE;
}

BOOLEAN
VolumeCheck::KeyInRange(_In_ IndexWalk* Walk, _In_ PUNICODE_STRING Name, _In_opt_ PIndexEntry Low, _In_opt_ PIndexEntry High)
{
    UNICODE_STRING Bound;
    LONG Result;

    /* Bounds are inclusive: a cut can leave the same key in a node and in
     * one of its ancestors for a while, and that is not a broken tree. */
    if (Low && KeyName(Low, &Bound) &&
        NT_SUCCESS(V->CollateFileNames(Name, &Bound, Walk->CaseSensitive, &Result)) && Result < 0)
    {
        return FALSE;
    }
    if (High && KeyName(High, &Bound) &&
        NT_SUCCESS(V->CollateFileNames(Name, &Bound, Walk->CaseSensitive, &Result)) && Result > 0)
    {
        return FALSE;
    }
    return TRUE;
}

BOOLEAN
VolumeCheck::KeyCompare(_In_ IndexWalk* Walk, _In_ PUNICODE_STRING Name, _In_opt_ PIndexEntry Bound, _Out_ LONG* Result)
{
    UNICODE_STRING BoundName;

    return Bound && KeyName(Bound, &BoundName) &&
           NT_SUCCESS(V->CollateFileNames(Name, &BoundName, Walk->CaseSensitive, Result));
}

BOOLEAN
VolumeCheck::KeyAbove(_In_ IndexWalk* Walk, _In_ PUNICODE_STRING Name, _In_opt_ PIndexEntry High)
{
    LONG Result;

    return KeyCompare(Walk, Name, High, &Result) && Result > 0;
}

/* A tail that starts at the first entry leaves nothing in range: the split
 * took the first entry as its median and the left node was not rewritten.
 * Such a tail is only a candidate, and only if the node's keys are above
 * Low like any key of its range; VerifyWholeCuts then asks that every one
 * of them is held elsewhere. */
ULONG
VolumeCheck::AcceptTail(_In_ IndexWalk* Walk, _In_ PIndexNodeHeader Node, _In_opt_ PIndexEntry Low, _In_ ULONG Start)
{
    PIndexEntry First = reinterpret_cast<PIndexEntry>(reinterpret_cast<PUCHAR>(Node) + Node->IndexOffset);
    UNICODE_STRING Name;
    LONG Result;

    if (Start > Node->IndexOffset)
        return Start;
    /* The first entry was validated by the caller. */
    if (!Low)
        return Start;
    return KeyName(First, &Name) && KeyCompare(Walk, &Name, Low, &Result) && Result > 0 ? Start : 0;
}

/* Finds where a node that a split cut short starts holding its right
 * sibling's entries: the first key above High, or the copy of High just
 * before it (the separator the split moved up). A copy of High that is the
 * last entry is the separator too, when the split took the last entry as
 * its median. 0 if there is none. When the tail is the whole node, the
 * result is the first entry's offset. */
ULONG
VolumeCheck::FindSplitTail(_In_ IndexWalk* Walk,
                           _In_ PIndexNodeHeader Node,
                           _In_opt_ PIndexEntry Low,
                           _In_opt_ PIndexEntry High)
{
    PUCHAR Base = reinterpret_cast<PUCHAR>(Node);
    ULONG Offset = Node->IndexOffset;
    ULONG End = Node->TotalIndexSize;
    ULONG PreviousOffset = 0;
    BOOLEAN PreviousEqual = FALSE;

    if (!High)
        return 0;
    while (Offset + FIELD_OFFSET(IndexEntry, IndexStream) <= End)
    {
        PIndexEntry Entry = reinterpret_cast<PIndexEntry>(Base + Offset);
        UNICODE_STRING Name;
        LONG Result;

        if (Entry->EntryLength < FIELD_OFFSET(IndexEntry, IndexStream) || (Entry->EntryLength & 7) != 0 ||
            Entry->EntryLength > End - Offset)
        {
            return 0;
        }
        if (Entry->Flags & INDEX_ENTRY_END)
            return PreviousEqual ? AcceptTail(Walk, Node, Low, PreviousOffset) : 0;
        if (!KeyName(Entry, &Name) || !KeyCompare(Walk, &Name, High, &Result))
            return 0;
        if (Result > 0)
        {
            ULONG Start = PreviousEqual ? PreviousOffset : Offset;

            return AcceptTail(Walk, Node, Low, Start);
        }
        PreviousOffset = Offset;
        PreviousEqual = Result == 0;
        Offset += Entry->EntryLength;
    }
    return 0;
}

void
VolumeCheck::LinkChild(_Inout_ IndexWalk* Walk, _In_ ULONGLONG Vcn, _In_opt_ PIndexEntry Low, _In_opt_ PIndexEntry High)
{
    ULONGLONG Bytes = Vcn * Walk->VcnSize;
    ULONG Block;

    if (Vcn > MAXULONG || (Bytes % Walk->BlockSize) != 0 || Bytes / Walk->BlockSize >= Walk->Blocks)
    {
        /* The node lies outside the allocation. */
        Walk->Unwalkable = TRUE;
        Walk->Problems++;
        Event(NtfsCheckIndexCorrupt, Walk->Directory, Vcn);
        return;
    }
    Block = (ULONG)(Bytes / Walk->BlockSize);
    if (TestBit(Walk->Linked, Block))
    {
        /* Linked twice. */
        Walk->Unwalkable = TRUE;
        Walk->Problems++;
        Event(NtfsCheckIndexCorrupt, Walk->Directory, Vcn);
        return;
    }
    SetBit(Walk->Linked, Block);
    if (TestBit(Walk->Valid, Block))
    {
        Walk->Stack[Walk->Depth].Block = Block;
        Walk->Stack[Walk->Depth].Low = Low;
        Walk->Stack[Walk->Depth].High = High;
        Walk->Depth++;
        return;
    }
    /* A linked block that is free, or in use but not a valid INDX block
     * (that one was reported already). */
    Walk->Unwalkable = TRUE;
    if (!(Block < Walk->BitmapBits && TestBit(Walk->Bitmap, Block)))
    {
        Walk->Problems++;
        Event(NtfsCheckIndexCorrupt, Walk->Directory, Vcn);
    }
}

void
VolumeCheck::WalkIndexNode(_Inout_ IndexWalk* Walk,
                           _In_ PIndexNodeHeader Node,
                           _In_ ULONG Available,
                           _In_ ULONG NodeBlock,
                           _In_ ULONGLONG NodeVcn,
                           _In_opt_ PIndexEntry Low,
                           _In_opt_ PIndexEntry High)
{
    PUCHAR Base = reinterpret_cast<PUCHAR>(Node);
    ULONG Offset = Node->IndexOffset;
    ULONG End = Node->TotalIndexSize;
    PIndexEntry Previous = Low;
    ULONG TrimFrom = 0;
    BOOLEAN Branch = FALSE;

    if (End > Available || Offset >= End || (Offset & 7) != 0)
    {
        Walk->Unwalkable = TRUE;
        return;
    }
    if (NodeBlock != MAXULONG)
        TrimFrom = FindSplitTail(Walk, Node, Low, High);

    while (Offset + FIELD_OFFSET(IndexEntry, IndexStream) <= End)
    {
        PIndexEntry Entry = reinterpret_cast<PIndexEntry>(Base + Offset);
        UNICODE_STRING Name;

        if (Entry->EntryLength < FIELD_OFFSET(IndexEntry, IndexStream) || (Entry->EntryLength & 7) != 0 ||
            Entry->EntryLength > End - Offset)
        {
            Walk->Unwalkable = TRUE;
            return;
        }
        if (TrimFrom && Offset == TrimFrom)
        {
            /* A split whose parent and right node landed before the rewritten
             * left node: from here on this node holds the right node's
             * entries. Cut them; an internal node keeps the child of the
             * first cut entry as its END child, as the rewrite would. */
            BOOLEAN Internal = (Entry->Flags & INDEX_ENTRY_NODE) != 0;
            ULONG Tail = Offset;

            while (Tail + FIELD_OFFSET(IndexEntry, IndexStream) <= End)
            {
                PIndexEntry Cut = reinterpret_cast<PIndexEntry>(Base + Tail);

                if (Cut->EntryLength < FIELD_OFFSET(IndexEntry, IndexStream) || (Cut->EntryLength & 7) != 0 ||
                    Cut->EntryLength > End - Tail || ((Cut->Flags & INDEX_ENTRY_NODE) != 0) != Internal ||
                    (Internal && Cut->EntryLength < FIELD_OFFSET(IndexEntry, IndexStream) + sizeof(ULONGLONG)) ||
                    (Tail != Offset && !(Cut->Flags & INDEX_ENTRY_END) &&
                     !(KeyName(Cut, &Name) && KeyAbove(Walk, &Name, High))))
                {
                    break;
                }
                if (Cut->Flags & INDEX_ENTRY_END)
                {
                    Tail = 0;
                    break;
                }
                Tail += Cut->EntryLength;
            }
            if (Tail != 0 || (TrimFrom != Node->IndexOffset && Internal != Branch))
            {
                Walk->Unwalkable = TRUE;
                Walk->Problems++;
                Event(NtfsCheckIndexCorrupt, Walk->Directory, NodeVcn);
                return;
            }
            Walk->Trim[NodeBlock] = TrimFrom;
            Walk->TrimChild[NodeBlock] = Internal ? *GetSubnodeVCN(Entry) : ~0ULL;
            if (Internal)
                LinkChild(Walk, *GetSubnodeVCN(Entry), Previous, High);
            return;
        }
        if (!(Entry->Flags & INDEX_ENTRY_END))
        {
            if (!KeyName(Entry, &Name))
            {
                Walk->Unwalkable = TRUE;
                return;
            }
            if (!KeyInRange(Walk, &Name, Low, High))
            {
                /* The parent links a node of another key range, for example
                 * one whose slot was freed and reused. */
                Walk->Unwalkable = TRUE;
                Walk->Problems++;
                Event(NtfsCheckIndexCorrupt, Walk->Directory, NodeVcn);
                return;
            }
        }
        if (Entry->Flags & INDEX_ENTRY_NODE)
        {
            Branch = TRUE;
            if (Entry->EntryLength < FIELD_OFFSET(IndexEntry, IndexStream) + sizeof(ULONGLONG))
            {
                Walk->Unwalkable = TRUE;
                return;
            }
            LinkChild(Walk, *GetSubnodeVCN(Entry), Previous, (Entry->Flags & INDEX_ENTRY_END) ? High : Entry);
        }
        if (Entry->Flags & INDEX_ENTRY_END)
            return;
        Previous = Entry;
        Offset += Entry->EntryLength;
    }
    Walk->Unwalkable = TRUE;
}

/* Whether a lookup of the name from the root finds it, in the tree as the
 * trims will leave it: a trimmed node is read up to its cut and goes on at
 * the child its END keeps. */
BOOLEAN
VolumeCheck::KeyReachable(_In_ IndexWalk* Walk, _In_ PUNICODE_STRING Name)
{
    PIndexNodeHeader Node = Walk->RootHeader;
    ULONG Block = MAXULONG;

    for (ULONG Level = 0; Node && Level <= Walk->Blocks; Level++)
    {
        PUCHAR Base = reinterpret_cast<PUCHAR>(Node);
        ULONG Offset = Node->IndexOffset;
        ULONG End = Node->TotalIndexSize;
        ULONGLONG Child = ~0ULL;
        ULONGLONG Bytes;

        if (Block != MAXULONG && Walk->Trim[Block])
        {
            End = Walk->Trim[Block];
            Child = Walk->TrimChild[Block];
        }
        while (Offset + FIELD_OFFSET(IndexEntry, IndexStream) <= End)
        {
            PIndexEntry Entry = reinterpret_cast<PIndexEntry>(Base + Offset);
            LONG Result;

            if (Entry->EntryLength < FIELD_OFFSET(IndexEntry, IndexStream) || (Entry->EntryLength & 7) != 0 ||
                Entry->EntryLength > End - Offset)
            {
                return FALSE;
            }
            if (!(Entry->Flags & INDEX_ENTRY_END))
            {
                if (!KeyCompare(Walk, Name, Entry, &Result))
                    return FALSE;
                if (Result == 0)
                    return TRUE;
            }
            else
            {
                Result = -1;
            }
            if (Result < 0)
            {
                Child = ~0ULL;
                if ((Entry->Flags & INDEX_ENTRY_NODE) &&
                    Entry->EntryLength >= FIELD_OFFSET(IndexEntry, IndexStream) + sizeof(ULONGLONG))
                {
                    Child = *GetSubnodeVCN(Entry);
                }
                break;
            }
            Offset += Entry->EntryLength;
        }
        if (Child == ~0ULL)
            return FALSE;
        Bytes = Child * Walk->VcnSize;
        if (Child > MAXULONG || (Bytes % Walk->BlockSize) != 0 || Bytes / Walk->BlockSize >= Walk->Blocks)
            return FALSE;
        Block = (ULONG)(Bytes / Walk->BlockSize);
        if (!TestBit(Walk->Valid, Block) || !TestBit(Walk->Linked, Block))
            return FALSE;
        Node = &reinterpret_cast<PIndexBuffer>(Walk->Allocation + (ULONGLONG)Block * Walk->BlockSize)->IndexHeader;
    }
    return FALSE;
}

/* A node whose every entry sits above its range is cut whole, as the split
 * that left it would have rewritten it. That is safe only if each name it
 * holds is found by a lookup somewhere else, in the right sibling or the
 * parent; if one is not, the node is not a split cut short and the tree is
 * left unwalkable, to be reset as before. */
void
VolumeCheck::VerifyWholeCuts(_Inout_ IndexWalk* Walk)
{
    if (Walk->Unwalkable || !Walk->Allocation || !Walk->RootHeader)
        return;
    for (ULONG Block = 0; Block < Walk->Blocks; Block++)
    {
        PIndexNodeHeader Node =
            &reinterpret_cast<PIndexBuffer>(Walk->Allocation + (ULONGLONG)Block * Walk->BlockSize)->IndexHeader;
        PUCHAR Base = reinterpret_cast<PUCHAR>(Node);
        ULONG Offset;

        if (!TestBit(Walk->Linked, Block) || !Walk->Trim[Block] || Walk->Trim[Block] != Node->IndexOffset)
            continue;
        for (Offset = Walk->Trim[Block]; Offset + FIELD_OFFSET(IndexEntry, IndexStream) <= Node->TotalIndexSize;)
        {
            PIndexEntry Entry = reinterpret_cast<PIndexEntry>(Base + Offset);
            UNICODE_STRING Name;

            if (Entry->Flags & INDEX_ENTRY_END)
                break;
            if (!KeyName(Entry, &Name) || !KeyReachable(Walk, &Name))
            {
                Walk->Trim[Block] = 0;
                Walk->Unwalkable = TRUE;
                Walk->Problems++;
                Event(NtfsCheckIndexCorrupt,
                      Walk->Directory,
                      (ULONGLONG)Block * Walk->BlockSize / Walk->VcnSize);
                return;
            }
            Offset += Entry->EntryLength;
        }
    }
}

void
VolumeCheck::CheckDirectory(_In_ ULONGLONG Directory)
{
    PFileRecord File;
    PAttribute RootAttr, AllocationAttr, BitmapAttr;
    PIndexRootEx Root;
    PUCHAR Allocation = NULL, Bitmap = NULL;
    ULONG AllocationLength = 0, BitmapLength = 0;
    PDataRun Runs = NULL;
    ULONG Fixed;
    BOOLEAN RootInBase, Written;
    IndexWalk Walk;
    NTSTATUS Status;

    RtlZeroMemory(&Walk, sizeof(Walk));
    Walk.Directory = Directory;
    Out->Directories++;
    Status = V->MFT->GetFileRecord((ULONG)Directory, &File);
    if (!NT_SUCCESS(Status))
    {
        Event(NtfsCheckIndexCorrupt, Directory, 0);
        return;
    }

    RootAttr = File->GetAttribute(TypeIndexRoot, const_cast<PWSTR>(L"$I30"));
    if (!RootAttr || RootAttr->IsNonResident ||
        RootAttr->Resident.DataLength < sizeof(IndexRootEx))
    {
        Event(NtfsCheckIndexCorrupt, Directory, 0);
        delete File;
        return;
    }
    Root = reinterpret_cast<PIndexRootEx>(reinterpret_cast<PUCHAR>(RootAttr) + RootAttr->Resident.DataOffset);
    RootInBase = reinterpret_cast<PUCHAR>(RootAttr) >= File->Data &&
                 reinterpret_cast<PUCHAR>(RootAttr) + RootAttr->Length <= File->Data + RecordSize;

    /* Check every block in use, then walk the tree from the root: only the
     * blocks the walk reaches hold names a lookup can find. */
    AllocationAttr = File->GetAttribute(TypeIndexAllocation, const_cast<PWSTR>(L"$I30"));
    BitmapAttr = File->GetAttribute(TypeBitmap, const_cast<PWSTR>(L"$I30"));
    if (AllocationAttr)
    {
        Walk.BlockSize = Root->BytesPerIndexRec;
        if (!BitmapAttr || Walk.BlockSize < sizeof(IndexBuffer) || (Walk.BlockSize % V->BytesPerSector) != 0 ||
            !NT_SUCCESS(File->ReadAttributeAlloc(AllocationAttr, &Allocation, &AllocationLength)) ||
            !NT_SUCCESS(File->ReadAttributeAlloc(BitmapAttr, &Bitmap, &BitmapLength)))
        {
            /* The blocks cannot be read (an attribute list that could not be
             * rebuilt hides their extents): no name in them can be found, so
             * the tree is emptied and refilled like any other. */
            Event(NtfsCheckIndexCorrupt, Directory, 0);
            Walk.Problems++;
            Walk.Unwalkable = TRUE;
        }
        else
        {
            Walk.Blocks = AllocationLength / Walk.BlockSize;
            Walk.VcnSize = Walk.BlockSize >= ClusterSize ? ClusterSize : 512;
            Walk.Bitmap = Bitmap;
            Walk.BitmapBits = (ULONGLONG)BitmapLength * 8;
            Walk.Valid = AllocateMap((ULONGLONG)Walk.Blocks + 1);
            Walk.Linked = AllocateMap((ULONGLONG)Walk.Blocks + 1);
            Walk.Stack = new(PagedPool, TAG_NTFS) WalkFrame[Walk.Blocks + 1];
            Walk.Trim = new(PagedPool, TAG_NTFS) ULONG[Walk.Blocks + 1];
            if (Walk.Trim)
                RtlZeroMemory(Walk.Trim, (Walk.Blocks + 1) * sizeof(ULONG));
            Walk.TrimChild = new(PagedPool, TAG_NTFS) ULONGLONG[Walk.Blocks + 1];
            Walk.CaseSensitive = File->IsCaseSensitiveDirectory();
            Walk.Allocation = Allocation;
            Walk.RootHeader = &Root->Header;
            if (!Walk.Valid || !Walk.Linked || !Walk.Stack || !Walk.Trim || !Walk.TrimChild)
            {
                Event(NtfsCheckIndexCorrupt, Directory, 0);
                delete[] Walk.Valid;
                Walk.Valid = NULL;
            }
        }
    }
    else if (Root->Header.Flags & 1)
    {
        Event(NtfsCheckIndexCorrupt, Directory, 0);
        Walk.Problems++;
        Walk.Unwalkable = TRUE;
    }

    if (Walk.Valid)
    {
        for (ULONG Block = 0; Block < Walk.Blocks && Block < Walk.BitmapBits; Block++)
        {
            PIndexBuffer Buffer = reinterpret_cast<PIndexBuffer>(Allocation + Block * Walk.BlockSize);
            ULONGLONG Vcn = (ULONGLONG)Block * Walk.BlockSize / Walk.VcnSize;

            if (!TestBit(Bitmap, Block))
                continue;
            Walk.InUse++;
            if (RtlCompareMemory(Buffer->RecordHeader.TypeID, "INDX", 4) != 4 ||
                !NT_SUCCESS(NtfsApplyFixup(&Buffer->RecordHeader, Walk.BlockSize, V->BytesPerSector)) ||
                Buffer->VCN != Vcn)
            {
                Walk.Problems++;
                Event(NtfsCheckIndexCorrupt, Directory, Vcn);
                continue;
            }
            SetBit(Walk.Valid, Block);
        }
        /* A bit past the end of the allocation names a block that is not there. */
        for (ULONGLONG Block = Walk.Blocks; Block < Walk.BitmapBits; Block++)
        {
            if (TestBit(Bitmap, Block))
            {
                Walk.Problems++;
                Walk.Unwalkable = TRUE;
                Event(NtfsCheckIndexCorrupt, Directory, Block * Walk.BlockSize / Walk.VcnSize);
                break;
            }
        }

        WalkIndexNode(&Walk,
                      &Root->Header,
                      RootAttr->Resident.DataLength - FIELD_OFFSET(IndexRootEx, Header),
                      MAXULONG,
                      0,
                      NULL,
                      NULL);
        while (Walk.Depth)
        {
            WalkFrame Frame = Walk.Stack[--Walk.Depth];
            PIndexBuffer Buffer = reinterpret_cast<PIndexBuffer>(Allocation + Frame.Block * Walk.BlockSize);

            WalkIndexNode(&Walk,
                          &Buffer->IndexHeader,
                          Walk.BlockSize - FIELD_OFFSET(IndexBuffer, IndexHeader),
                          Frame.Block,
                          (ULONGLONG)Frame.Block * Walk.BlockSize / Walk.VcnSize,
                          Frame.Low,
                          Frame.High);
        }
        VerifyWholeCuts(&Walk);
    }

    /* A tree that cannot be walked is emptied; every name in it is then
     * missing and is added back from the files' own $FILE_NAME. */
    if (Walk.Unwalkable && RepairIndexes && RootInBase)
    {
        class Directory Index(V);

        Status = Index.ResetIndex(File, L"$I30");
        if (NT_SUCCESS(Status))
        {
            Out->Errors -= Walk.Problems;
            Out->ResetIndexes++;
            Event(NtfsCheckIndexReset, Directory, Walk.InUse);
            goto Done;
        }
    }

    Fixed = CheckIndexEntries(Directory,
                              &Root->Header,
                              RootAttr->Resident.DataLength - FIELD_OFFSET(IndexRootEx, Header),
                              0,
                              RootInBase,
                              FALSE);
    if (Fixed && NT_SUCCESS(V->MFT->WriteFileRecordToMFT(File)))
    {
        Out->Errors -= Fixed;
        Out->FixedIndexEntries += Fixed;
        Event(NtfsCheckIndexEntryFixed, Directory, Fixed);
    }

    if (Walk.Valid)
    {
        PUCHAR Scratch = NULL;
        ULONG Released = 0;
        BOOLEAN Trimmed;

        if (RepairIndexes)
        {
            Runs = File->FindNonResidentData(AllocationAttr);
            Scratch = new(PagedPool, TAG_NTFS) UCHAR[Walk.BlockSize];
        }

        for (ULONG Block = 0; Block < Walk.Blocks && Block < Walk.BitmapBits; Block++)
        {
            PIndexBuffer Buffer = reinterpret_cast<PIndexBuffer>(Allocation + Block * Walk.BlockSize);
            ULONGLONG Vcn = (ULONGLONG)Block * Walk.BlockSize / Walk.VcnSize;

            if (!TestBit(Bitmap, Block))
                continue;
            if (!TestBit(Walk.Valid, Block))
            {
                /* In use but not a valid block. If nothing links it, it was
                 * never written: its corruption was counted, release it.
                 * A linked one made the tree unwalkable. */
                if (!TestBit(Walk.Linked, Block) && RepairIndexes && !Walk.Unwalkable)
                {
                    Bitmap[Block >> 3] &= (UCHAR)~(1 << (Block & 7));
                    Released++;
                }
                continue;
            }
            if (!TestBit(Walk.Linked, Block))
            {
                /* No lookup reaches it, so its names are not counted; they
                 * are relinked into the tree and the block is released. */
                Event(NtfsCheckIndexBlockUnreferenced, Directory, Vcn);
                if (RepairIndexes && !Walk.Unwalkable)
                {
                    Bitmap[Block >> 3] &= (UCHAR)~(1 << (Block & 7));
                    Released++;
                }
                continue;
            }
            Trimmed = FALSE;
            if (Walk.Trim[Block])
            {
                /* End the node before the entries its right sibling holds;
                 * they are not counted here. An internal node's END keeps
                 * the child of the first entry cut. */
                PIndexEntry Tail = reinterpret_cast<PIndexEntry>(
                    reinterpret_cast<PUCHAR>(&Buffer->IndexHeader) + Walk.Trim[Block]);
                ULONG TailLength = FIELD_OFFSET(IndexEntry, IndexStream);

                if (Walk.TrimChild[Block] != ~0ULL)
                    TailLength += sizeof(ULONGLONG);
                RtlZeroMemory(Tail, TailLength);
                Tail->EntryLength = (UINT16)TailLength;
                Tail->Flags = INDEX_ENTRY_END;
                if (Walk.TrimChild[Block] != ~0ULL)
                {
                    Tail->Flags |= INDEX_ENTRY_NODE;
                    *GetSubnodeVCN(Tail) = Walk.TrimChild[Block];
                }
                Buffer->IndexHeader.TotalIndexSize = Walk.Trim[Block] + TailLength;
                Event(NtfsCheckIndexTailTrimmed, Directory, Vcn);
                Trimmed = TRUE;
            }
            Fixed = CheckIndexEntries(Directory,
                                      &Buffer->IndexHeader,
                                      Walk.BlockSize - FIELD_OFFSET(IndexBuffer, IndexHeader),
                                      Vcn,
                                      Runs && Scratch,
                                      Runs && Scratch);
            if ((!Fixed && !Trimmed && !PendingCuts) || !Runs || !Scratch)
                continue;

            RtlCopyMemory(Scratch, Buffer, Walk.BlockSize);
            Written = NT_SUCCESS(NtfsCommitFixup(reinterpret_cast<PNTFSRecordHeader>(Scratch),
                                                 Walk.BlockSize,
                                                 V->BytesPerSector)) &&
                      NT_SUCCESS(WriteStreamRange(Runs, (ULONGLONG)Block * Walk.BlockSize, Walk.BlockSize, Scratch));
            SettlePendingCuts(Written);
            if (Written && (Fixed || Trimmed))
            {
                Out->Errors -= Fixed + (Trimmed ? 1 : 0);
                Out->FixedIndexEntries += Fixed + (Trimmed ? 1 : 0);
                if (Fixed)
                    Event(NtfsCheckIndexEntryFixed, Directory, Fixed);
            }
        }
        delete[] Scratch;

        if (Released)
        {
            ULONG Length = BitmapLength;
            LARGE_INTEGER Offset = {};

            if (NT_SUCCESS(File->WriteFileData(TypeBitmap, const_cast<PWSTR>(L"$I30"), Bitmap, &Length, &Offset)) &&
                Length == BitmapLength)
            {
                Out->Errors -= Released;
                Out->ReleasedIndexBlocks += Released;
            }
        }
    }

Done:
    if (Runs)
        FreeDataRun(Runs);
    if (RepairIndexes)
        V->FlushVolume();
    delete[] Walk.Valid;
    delete[] Walk.Linked;
    delete[] Walk.Trim;
    delete[] Walk.TrimChild;
    delete[] Walk.Stack;
    delete[] Allocation;
    delete[] Bitmap;
    delete File;
}

NTSTATUS
VolumeCheck::Run(_In_ BOOLEAN Repair)
{
    NtfsVolumeInformation Information;
    NTSTATUS Status;

    RtlZeroMemory(Out, sizeof(*Out));
    Status = V->QueryInformation(&Information);
    if (!NT_SUCCESS(Status))
        return Status;

    ClusterSize = Information.BytesPerCluster;
    RecordSize = Information.BytesPerFileRecordSegment;
    ClusterCount = V->ClustersInVolume;
    if (!ClusterSize || !RecordSize || RecordSize % V->BytesPerSector)
        return STATUS_FILE_CORRUPT_ERROR;
    RecordCount = Information.MftValidDataLength / RecordSize;
    if (RecordCount <= CHECK_FIRST_USER_RECORD || RecordCount > MAXULONG)
        return STATUS_FILE_CORRUPT_ERROR;

    UsedMap = AllocateMap(ClusterCount);
    DirectoryMap = AllocateMap(RecordCount);
    InUseMap = AllocateMap(RecordCount);
    Sequence = new(PagedPool, TAG_NTFS) USHORT[(ULONG)RecordCount];
    Names = new(PagedPool, TAG_NTFS) NameTally[(ULONG)RecordCount];
    Records = new(PagedPool, TAG_NTFS) UCHAR[CHECK_RECORD_CHUNK * RecordSize];
    ExtensionOwner = new(PagedPool, TAG_NTFS) ULONG[(ULONG)RecordCount];
    AttrListMap = AllocateMap(RecordCount);
    ExtendedMap = AllocateMap(RecordCount);
    if (!UsedMap || !DirectoryMap || !InUseMap || !Sequence || !Names || !Records || !ExtensionOwner || !AttrListMap ||
        !ExtendedMap)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(ExtensionOwner, (ULONG)RecordCount * sizeof(ULONG));
    RtlZeroMemory(Sequence, (ULONG)RecordCount * sizeof(USHORT));
    RtlZeroMemory(Names, (ULONG)RecordCount * sizeof(NameTally));

    Status = LoadMftBitmap();
    if (!NT_SUCCESS(Status))
        return Status;

    RepairRecords = Repair && !V->IsReadOnly;
    if (RepairRecords)
    {
        CrossMap = AllocateMap(ClusterCount);
        if (!CrossMap)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    for (ULONGLONG First = 0; First < RecordCount; First += CHECK_RECORD_CHUNK)
    {
        ULONG Count = (ULONG)min((ULONGLONG)CHECK_RECORD_CHUNK, RecordCount - First);

        Status = ReadRecords(First, Count);
        if (!NT_SUCCESS(Status))
            return Status;
        for (ULONG Index = 0; Index < Count; Index++)
            CheckRecord(First + Index, Records + Index * RecordSize);
    }

    Status = CheckRecordsBeyondMft();
    if (!NT_SUCCESS(Status))
        return Status;

    if (RepairRecords)
        PlanRecordFrees();

    SortNameKeys();
    if (NameKeyCount)
        NameKeyUsed = AllocateMap(NameKeyCount);
    CheckAttributeLists();

    RepairIndexes = Repair && StructuralErrors == 0 && !V->IsReadOnly;
    if (Repair && !V->IsReadOnly && StructuralErrors != 0)
    {
        ULONGLONG Blocker = ~0ULL;

        /* A corrupt record the record pass will free no longer holds
         * anything back (PlanRecordFrees took its faults out). */
        for (ULONG Index = 0; Index < BlockerCount && Blocker == ~0ULL; Index++)
        {
            BOOLEAN Planned = FALSE;

            for (ULONG Plan = 0; RepairRecords && Plan < CorruptCount; Plan++)
                Planned |= Corrupt[Plan].Number == Blockers[Index];
            if (Blockers[Index] < RecordCount && TestBit(MftBitmap, Blockers[Index]) && !Planned)
                Blocker = Blockers[Index];
        }
        Event(NtfsCheckRepairsBlocked, StructuralErrors, Blocker);
    }
    for (ULONGLONG Number = 0; Number < RecordCount; Number++)
    {
        if (TestBit(DirectoryMap, Number) && TestBit(InUseMap, Number))
            CheckDirectory(Number);
    }

    for (ULONGLONG Number = 0; Number < RecordCount; Number++)
    {
        if (TestBit(InUseMap, Number) &&
            (Names[Number].Expected != Names[Number].Found ||
             Names[Number].ExpectedHash != Names[Number].FoundHash))
        {
            Event(NtfsCheckNameNotIndexed, Number, Names[Number].Expected - Names[Number].Found);
            if (RepairIndexes && Names[Number].Expected > Names[Number].Found)
                RememberOrphan(Number);
        }
    }

    /*
     * Crash-safe order: index entries first (cut in place above, the rest
     * here), then the corrupt records they named, then the MFT bitmap,
     * then the volume bitmap. A stop at any point leaves at worst records
     * or clusters marked in use that nothing references, never an index
     * entry naming a free record or a record whose clusters are free.
     */
    if (RepairIndexes)
        RemoveDangling();
    if (RepairRecords)
    {
        RepairRecordsPass();
        CommitMftBitmap(RepairIndexes);
    }
    Status = CheckBitmap(Repair);
    if (NT_SUCCESS(Status) && RepairIndexes)
    {
        RelinkOrphans();
        RecoverLostFiles();
    }
    else if (RepairIndexes && Out->ResetIndexes)
    {
        /* A directory that was emptied gets its names back even when the
         * bitmap check failed; it is never left empty. */
        RelinkOrphans();
    }
    if (NT_SUCCESS(Status) && RepairRecords && CrossLinkCount && StructuralErrors == CrossLinkCount)
        ResolveCrossLinks();
    return Status;
}

}

/*
 * The shape invariants (src/verify) on what the check and its repairs left:
 * a broken one counts as a problem the check could not fix, so the volume is
 * not reported clean and its dirty flag is not cleared. It costs one more MFT
 * pass and one more walk of each index; an index with more than
 * NTFS_CHECK_SHAPE_INDEX_BYTES of allocation is not walked, and the skipped
 * ones are reported. When the verifier cannot run at all, that is reported
 * and the check's own verdict stands.
 */
static void
VerifyCheckedShape(
    _In_ PNtfsVolume DiskVolume,
    _In_opt_ PNtfsCheckCallback Callback,
    _In_opt_ void* Context,
    _Inout_ PNtfsCheckResult Result)
{
    NtfsShapeVolumeStats Stats;
    NtfsShapeFault Fault;
    NTSTATUS Status;

    Status = NtfsVerifyVolumeShapeBounded(DiskVolume, 0, NTFS_CHECK_SHAPE_INDEX_BYTES, &Stats, &Fault);
    Result->ShapeFiles = Stats.FilesChecked;
    Result->ShapeSkippedIndexes = Stats.IndexesSkipped;
    if (Stats.IndexesSkipped && Callback)
        Callback(Context, NtfsCheckShapeIndexesSkipped, Stats.IndexesSkipped, NTFS_CHECK_SHAPE_INDEX_BYTES);
    if (Status == STATUS_FILE_CORRUPT_ERROR && Fault.Invariant != NtfsShapeOk)
    {
        if (Callback)
        {
            Callback(Context, NtfsCheckShapeFault,
                     ((UINT64)Fault.Invariant << 48) | (Fault.Record & 0x0000FFFFFFFFFFFFULL), Fault.Detail);
        }
        /* Found and left: the restart rule (Errors < FoundErrors) is unchanged. */
        Result->Errors++;
        Result->FoundErrors++;
    }
    else if (!NT_SUCCESS(Status) && Callback)
    {
        Callback(Context, NtfsCheckShapeNotRun, (ULONG)Status, 0);
    }
}

extern "C"
NTSTATUS
NtfsVolumeCheck(
    _In_ PNtfsVolume DiskVolume,
    _In_ BOOLEAN Repair,
    _In_opt_ PNtfsCheckCallback Callback,
    _In_opt_ void* Context,
    _Out_ PNtfsCheckResult Result)
{
    NtfsCheckResult Pass;
    NTSTATUS Status;

    if (!DiskVolume || !Result)
        return STATUS_INVALID_PARAMETER;

    {
        VolumeCheck Check(reinterpret_cast<PVolume>(DiskVolume), Callback, Context, Result);

        Status = Check.Run(Repair);
    }
    /* Verify whenever anything was repaired, so Errors is always measured on
     * what the repairs left and not just counted down as they were written. */
    if (!NT_SUCCESS(Status) || !Repair ||
        !(Result->RemovedIndexEntries || Result->RelinkedNames || Result->FreedRecords ||
          Result->ResolvedCrossLinks || Result->RecoveredFiles || Result->RepairedAttributeLists ||
          Result->RemovedNames || Result->FixedIndexEntries || Result->ClaimedClusters ||
          Result->FreedClusters || Result->FixedRecordBitmapBits || Result->ResetIndexes ||
          Result->ReleasedIndexBlocks))
    {
        if (NT_SUCCESS(Status))
            VerifyCheckedShape(DiskVolume, Callback, Context, Result);
        return Status;
    }

    {
        VolumeCheck Check(reinterpret_cast<PVolume>(DiskVolume), Callback, Context, &Pass);

        Status = Check.Run(Repair);
    }
    if (NT_SUCCESS(Status))
    {
        Pass.FreedClusters += Result->FreedClusters;
        Pass.ClaimedClusters += Result->ClaimedClusters;
        Pass.FixedIndexEntries += Result->FixedIndexEntries;
        Pass.RemovedIndexEntries += Result->RemovedIndexEntries;
        Pass.RelinkedNames += Result->RelinkedNames;
        Pass.FreedRecords += Result->FreedRecords;
        Pass.ResolvedCrossLinks += Result->ResolvedCrossLinks;
        Pass.RecoveredFiles += Result->RecoveredFiles;
        Pass.RepairedAttributeLists += Result->RepairedAttributeLists;
        Pass.RemovedNames += Result->RemovedNames;
        Pass.FixedRecordBitmapBits += Result->FixedRecordBitmapBits;
        Pass.ResetIndexes += Result->ResetIndexes;
        Pass.ReleasedIndexBlocks += Result->ReleasedIndexBlocks;
        /* The verify pass only sees what the repairs left behind. */
        Pass.FoundErrors = Result->FoundErrors;
        *Result = Pass;
    }
    if (NT_SUCCESS(Status))
        VerifyCheckedShape(DiskVolume, Callback, Context, Result);
    return Status;
}
