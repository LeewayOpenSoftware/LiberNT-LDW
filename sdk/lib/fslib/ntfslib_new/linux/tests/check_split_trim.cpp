/*
 * PROJECT:     LiberNT NTFS library tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The checker's index walk trims a node that a split cut short
 *              left holding its right sibling's entries, keeps the right
 *              END child, and loses no name, also when the split took the
 *              last entry as its median
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <new>

#include <ntfslib_new.h>

/* The walk and the volume's upcase table are private; open the library's
 * classes for this test only. The standard headers are already included. */
#define private public
#include <ntfslib_new_internal.h>
#include "../../src/check/check.cpp"
#undef private

namespace
{

const ULONG BlockSize = 4096;
const ULONG Blocks = 8;

struct Key
{
    const char* Name;
    ULONGLONG Child;     /* ~0: no subnode */
};

int Failures = 0;

void
Expect(bool Condition, const char* Test, const char* What)
{
    if (!Condition)
    {
        std::fprintf(stderr, "check_split_trim: %s: %s\n", Test, What);
        Failures++;
    }
}

/* Writes a node: its keys, then an END entry linking EndChild (~0: none). */
void
BuildNode(PIndexNodeHeader Node, ULONG Available, const std::vector<Key>& Keys, ULONGLONG EndChild)
{
    PUCHAR Base = reinterpret_cast<PUCHAR>(Node);
    ULONG Offset = sizeof(IndexNodeHeader);

    std::memset(Node, 0, Available);
    Node->IndexOffset = Offset;
    for (const Key& K : Keys)
    {
        PIndexEntry Entry = reinterpret_cast<PIndexEntry>(Base + Offset);
        PFileNameEx Name = reinterpret_cast<PFileNameEx>(Entry->IndexStream);
        ULONG Length = (ULONG)std::strlen(K.Name);
        ULONG Stream = FIELD_OFFSET(FileNameEx, Name) + Length * sizeof(WCHAR);
        ULONG EntryLength = (FIELD_OFFSET(IndexEntry, IndexStream) + Stream + 7) & ~7u;

        if (K.Child != ~0ULL)
            EntryLength += sizeof(ULONGLONG);
        Name->ParentFileReference = 5;
        Name->NameLength = (UINT8)Length;
        Name->NameType = 1;
        for (ULONG Index = 0; Index < Length; Index++)
            Name->Name[Index] = (WCHAR)K.Name[Index];
        Entry->Data.Directory.IndexedFile = 100 + Offset;
        Entry->EntryLength = (UINT16)EntryLength;
        Entry->StreamLength = (UINT16)Stream;
        if (K.Child != ~0ULL)
        {
            Entry->Flags = INDEX_ENTRY_NODE;
            *GetSubnodeVCN(Entry) = K.Child;
        }
        Offset += EntryLength;
    }
    {
        PIndexEntry End = reinterpret_cast<PIndexEntry>(Base + Offset);
        ULONG EndLength = FIELD_OFFSET(IndexEntry, IndexStream);

        End->Flags = INDEX_ENTRY_END;
        if (EndChild != ~0ULL)
        {
            EndLength += sizeof(ULONGLONG);
            End->EntryLength = (UINT16)EndLength;
            End->Flags |= INDEX_ENTRY_NODE;
            *GetSubnodeVCN(End) = EndChild;
        }
        End->EntryLength = (UINT16)EndLength;
        Offset += EndLength;
    }
    Node->TotalIndexSize = Offset;
    Node->AllocatedSize = Available;
}

ULONG
OffsetOf(PIndexNodeHeader Node, const char* Wanted)
{
    PUCHAR Base = reinterpret_cast<PUCHAR>(Node);

    for (ULONG Offset = Node->IndexOffset; Offset < Node->TotalIndexSize;)
    {
        PIndexEntry Entry = reinterpret_cast<PIndexEntry>(Base + Offset);
        PFileNameEx Name = reinterpret_cast<PFileNameEx>(Entry->IndexStream);

        if (Entry->Flags & INDEX_ENTRY_END)
            break;
        if (Name->NameLength == std::strlen(Wanted))
        {
            ULONG Index = 0;

            while (Index < Name->NameLength && Name->Name[Index] == (WCHAR)Wanted[Index])
                Index++;
            if (Index == Name->NameLength)
                return Offset;
        }
        Offset += Entry->EntryLength;
    }
    return 0;
}

/* Collects the keys a lookup reaches once the trims are applied, as the
 * counting does: entries of linked valid blocks, each up to its trim. */
void
CollectKeys(PIndexNodeHeader Node, ULONG Trim, std::multiset<std::string>& Names)
{
    PUCHAR Base = reinterpret_cast<PUCHAR>(Node);
    ULONG End = Trim ? Trim : Node->TotalIndexSize;

    for (ULONG Offset = Node->IndexOffset; Offset < End;)
    {
        PIndexEntry Entry = reinterpret_cast<PIndexEntry>(Base + Offset);
        PFileNameEx Name = reinterpret_cast<PFileNameEx>(Entry->IndexStream);
        std::string Text;

        if (Entry->Flags & INDEX_ENTRY_END)
            break;
        for (ULONG Index = 0; Index < Name->NameLength; Index++)
            Text.push_back((char)Name->Name[Index]);
        Names.insert(Text);
        Offset += Entry->EntryLength;
    }
}

struct Fixture
{
    alignas(Volume) unsigned char VolumeStorage[sizeof(Volume)];
    Volume* V;
    std::vector<WCHAR> Upcase;
    NtfsCheckResult Result;
    VolumeCheck* Check;
    std::vector<UCHAR> Allocation;
    UCHAR RootBuffer[1024];
    PIndexNodeHeader Root;
    UCHAR Bitmap[8];
    VolumeCheck::IndexWalk Walk;

    Fixture()
        : Upcase(65536), Allocation(Blocks * BlockSize)
    {
        /* Only the upcase table of the volume is used by the walk. */
        std::memset(VolumeStorage, 0, sizeof(VolumeStorage));
        V = reinterpret_cast<Volume*>(VolumeStorage);
        for (ULONG Index = 0; Index < Upcase.size(); Index++)
            Upcase[Index] = (WCHAR)((Index >= 'a' && Index <= 'z') ? Index - 32 : Index);
        V->UpcaseTable = Upcase.data();
        V->UpcaseTableLength = (ULONG)Upcase.size();
        Check = new VolumeCheck(V, NULL, NULL, &Result);
        std::memset(&Result, 0, sizeof(Result));
        Root = reinterpret_cast<PIndexNodeHeader>(RootBuffer);
        std::memset(Bitmap, 0xFF, sizeof(Bitmap));

        std::memset(&Walk, 0, sizeof(Walk));
        Walk.Directory = 5;
        Walk.Bitmap = Bitmap;
        Walk.BitmapBits = Blocks;
        Walk.Valid = VolumeCheck::AllocateMap(Blocks + 1);
        Walk.Linked = VolumeCheck::AllocateMap(Blocks + 1);
        Walk.Stack = new VolumeCheck::WalkFrame[Blocks + 1];
        Walk.Trim = new ULONG[Blocks + 1]();
        Walk.TrimChild = new ULONGLONG[Blocks + 1]();
        Walk.Blocks = Blocks;
        Walk.Allocation = Allocation.data();
        Walk.RootHeader = Root;
        Walk.BlockSize = BlockSize;
        Walk.VcnSize = BlockSize;
        for (ULONG Block = 0; Block < Blocks; Block++)
            VolumeCheck::SetBit(Walk.Valid, Block);
    }

    ~Fixture()
    {
        delete Check;
        delete[] Walk.Valid;
        delete[] Walk.Linked;
        delete[] Walk.Stack;
        delete[] Walk.Trim;
        delete[] Walk.TrimChild;
    }

    PIndexNodeHeader Node(ULONG Block)
    {
        return &reinterpret_cast<PIndexBuffer>(Allocation.data() + Block * BlockSize)->IndexHeader;
    }

    void BuildBlock(ULONG Block, const std::vector<Key>& Keys, ULONGLONG EndChild)
    {
        BuildNode(Node(Block), BlockSize - FIELD_OFFSET(IndexBuffer, IndexHeader), Keys, EndChild);
    }

    /* The walk loop of CheckDirectory. */
    void Run()
    {
        Check->WalkIndexNode(&Walk, Root, sizeof(RootBuffer), MAXULONG, 0, NULL, NULL);
        while (Walk.Depth)
        {
            VolumeCheck::WalkFrame Frame = Walk.Stack[--Walk.Depth];

            Check->WalkIndexNode(&Walk,
                                 Node(Frame.Block),
                                 BlockSize - FIELD_OFFSET(IndexBuffer, IndexHeader),
                                 Frame.Block,
                                 Frame.Block,
                                 Frame.Low,
                                 Frame.High);
        }
        Check->VerifyWholeCuts(&Walk);
    }

    std::multiset<std::string> Reached()
    {
        std::multiset<std::string> Names;

        CollectKeys(Root, 0, Names);
        for (ULONG Block = 0; Block < Blocks; Block++)
        {
            if (VolumeCheck::TestBit(Walk.Linked, Block))
                CollectKeys(Node(Block), Walk.Trim[Block], Names);
        }
        return Names;
    }
};

/* Before the split: L = [A->2, M->3, T->4, END->5]. The split moved M up and
 * T to the new right node R; the parent and R landed, L was not rewritten.
 * Root = [M->0, END->1], R (block 1) = [T->4, END->5]. */
void
InternalTail()
{
    const char* Test = "internal tail";
    Fixture F;

    BuildNode(F.Root, sizeof(F.RootBuffer), { { "M", 0 } }, 1);
    F.BuildBlock(0, { { "A", 2 }, { "M", 3 }, { "T", 4 } }, 5);
    F.BuildBlock(1, { { "T", 4 } }, 5);
    F.BuildBlock(2, { { "0", ~0ULL } }, ~0ULL);
    F.BuildBlock(3, { { "G", ~0ULL } }, ~0ULL);
    F.BuildBlock(4, { { "P", ~0ULL } }, ~0ULL);
    F.BuildBlock(5, { { "X", ~0ULL } }, ~0ULL);
    F.Run();

    Expect(!F.Walk.Unwalkable, Test, "the tree is walkable");
    Expect(F.Walk.Problems == 0 && F.Result.Errors == 0, Test, "no problem raised");
    Expect(F.Walk.Trim[0] == OffsetOf(F.Node(0), "M"), Test, "L is cut at the separator copy");
    Expect(F.Walk.TrimChild[0] == 3, Test, "L keeps the separator's child as its END child");
    for (ULONG Block = 1; Block < 6; Block++)
        Expect(F.Walk.Trim[Block] == 0, Test, "no other node is cut");
    for (ULONG Block = 0; Block < 6; Block++)
        Expect(VolumeCheck::TestBit(F.Walk.Linked, Block), Test, "every node is linked once");
    Expect(!VolumeCheck::TestBit(F.Walk.Linked, 6), Test, "nothing else is linked");
    Expect(F.Reached() == std::multiset<std::string>({ "0", "A", "G", "M", "P", "T", "X" }), Test,
           "every name is reached exactly once");
}

/* The same with the right node walked first and with leaves: a leaf L that
 * still holds M and T, under Root = [M->0, END->1], R = [T]. */
void
LeafTail()
{
    const char* Test = "leaf tail";
    Fixture F;

    BuildNode(F.Root, sizeof(F.RootBuffer), { { "M", 0 } }, 1);
    F.BuildBlock(0, { { "A", ~0ULL }, { "C", ~0ULL }, { "M", ~0ULL }, { "T", ~0ULL } }, ~0ULL);
    F.BuildBlock(1, { { "T", ~0ULL } }, ~0ULL);
    F.Run();

    Expect(!F.Walk.Unwalkable, Test, "the tree is walkable");
    Expect(F.Walk.Trim[0] == OffsetOf(F.Node(0), "M"), Test, "the leaf is cut at the separator copy");
    Expect(F.Walk.TrimChild[0] == ~0ULL, Test, "a leaf keeps no END child");
    Expect(F.Reached() == std::multiset<std::string>({ "A", "C", "M", "T" }), Test,
           "every name is reached exactly once");
}

/* A tail that is not above the range, or a node whose first key is already
 * above it, is not a split: the tree stays unwalkable and is reset. */
void
NotASplit()
{
    {
        const char* Test = "unsorted tail";
        Fixture F;

        BuildNode(F.Root, sizeof(F.RootBuffer), { { "M", 0 } }, 1);
        F.BuildBlock(0, { { "A", ~0ULL }, { "T", ~0ULL }, { "B", ~0ULL } }, ~0ULL);
        F.BuildBlock(1, { { "T", ~0ULL } }, ~0ULL);
        F.Run();
        Expect(F.Walk.Unwalkable, Test, "the tree is unwalkable");
        Expect(F.Walk.Trim[0] == 0, Test, "nothing is cut");
    }
    {
        const char* Test = "reused slot";
        Fixture F;

        BuildNode(F.Root, sizeof(F.RootBuffer), { { "M", 0 } }, 1);
        F.BuildBlock(0, { { "Q", ~0ULL }, { "R", ~0ULL } }, ~0ULL);
        F.BuildBlock(1, { { "T", ~0ULL } }, ~0ULL);
        F.Run();
        Expect(F.Walk.Unwalkable, Test, "the tree is unwalkable");
        Expect(F.Walk.Trim[0] == 0, Test, "nothing is cut");
    }
}

/* The split took the last entry as its median: L = [A->2, M->3, END->4]
 * became L' = [A->2, END->3] and R = [END->4] under Root = [M->0, END->1].
 * The stale L ends with the separator copy; its END child is R's. */
void
MedianLast()
{
    {
        const char* Test = "median last, internal";
        Fixture F;

        BuildNode(F.Root, sizeof(F.RootBuffer), { { "M", 0 } }, 1);
        F.BuildBlock(0, { { "A", 2 }, { "M", 3 } }, 4);
        F.BuildBlock(1, {}, 4);
        F.BuildBlock(2, { { "0", ~0ULL } }, ~0ULL);
        F.BuildBlock(3, { { "G", ~0ULL } }, ~0ULL);
        F.BuildBlock(4, { { "X", ~0ULL } }, ~0ULL);
        F.Run();
        Expect(!F.Walk.Unwalkable, Test, "the tree is walkable");
        Expect(F.Walk.Trim[0] == OffsetOf(F.Node(0), "M"), Test, "L is cut at the separator copy");
        Expect(F.Walk.TrimChild[0] == 3, Test, "L keeps the separator's child as its END child");
        for (ULONG Block = 0; Block < 5; Block++)
            Expect(VolumeCheck::TestBit(F.Walk.Linked, Block), Test, "every node is linked once");
        Expect(F.Reached() == std::multiset<std::string>({ "0", "A", "G", "M", "X" }), Test,
               "every name is reached exactly once");
    }
    {
        const char* Test = "median last, leaf";
        Fixture F;

        /* The same in a leaf, or a key left in a leaf and in its parent for
         * a while: the parent holds it, the leaf copy is cut. */
        BuildNode(F.Root, sizeof(F.RootBuffer), { { "M", 0 } }, 1);
        F.BuildBlock(0, { { "A", ~0ULL }, { "M", ~0ULL } }, ~0ULL);
        F.BuildBlock(1, { { "T", ~0ULL } }, ~0ULL);
        F.Run();
        Expect(!F.Walk.Unwalkable, Test, "the tree is walkable");
        Expect(F.Walk.Trim[0] == OffsetOf(F.Node(0), "M"), Test, "the leaf is cut at the separator copy");
        Expect(F.Reached() == std::multiset<std::string>({ "A", "M", "T" }), Test,
               "every name is reached exactly once");
    }
}

/* The split took the first entry as its median: the left node, never
 * rewritten, still holds every key and all of them sort above the bound
 * that leads to it. The right node and the parent hold the names, so the
 * node is cut to an empty leaf, or to an END pointing to the first
 * entry's child. */
void
MedianFirst()
{
    {
        const char* Test = "median first, leaf";
        Fixture F;

        /* L = [M, P, T] was split at M: Root = [M->0, END->1], R = [P, T]. */
        BuildNode(F.Root, sizeof(F.RootBuffer), { { "M", 0 } }, 1);
        F.BuildBlock(0, { { "P", ~0ULL }, { "T", ~0ULL } }, ~0ULL);
        F.BuildBlock(1, { { "P", ~0ULL }, { "T", ~0ULL } }, ~0ULL);
        F.Run();
        Expect(!F.Walk.Unwalkable, Test, "the tree is walkable");
        Expect(F.Walk.Problems == 0, Test, "no problem raised");
        Expect(F.Walk.Trim[0] == F.Node(0)->IndexOffset, Test, "the whole leaf is cut");
        Expect(F.Walk.TrimChild[0] == ~0ULL, Test, "a leaf keeps no END child");
        Expect(F.Reached() == std::multiset<std::string>({ "M", "P", "T" }), Test, "every name is reached once");
    }
    {
        const char* Test = "median first, separator copy";
        Fixture F;

        /* The separator is the old first key: L = [M, P, T], same split. */
        BuildNode(F.Root, sizeof(F.RootBuffer), { { "M", 0 } }, 1);
        F.BuildBlock(0, { { "M", ~0ULL }, { "P", ~0ULL }, { "T", ~0ULL } }, ~0ULL);
        F.BuildBlock(1, { { "P", ~0ULL }, { "T", ~0ULL } }, ~0ULL);
        F.Run();
        Expect(!F.Walk.Unwalkable, Test, "the tree is walkable");
        Expect(F.Walk.Trim[0] == F.Node(0)->IndexOffset, Test, "the whole leaf is cut");
        Expect(F.Reached() == std::multiset<std::string>({ "M", "P", "T" }), Test, "every name is reached once");
    }
    {
        const char* Test = "median first, internal";
        Fixture F;

        /* L = [M->2, P->3, END->4] was split at M, its first entry:
         * Root = [M->0, END->1], R = [P->3, END->4], and L should have
         * become [END->2]. The separator's child 2 stays with L. */
        BuildNode(F.Root, sizeof(F.RootBuffer), { { "M", 0 } }, 1);
        F.BuildBlock(0, { { "M", 2 }, { "P", 3 } }, 4);
        F.BuildBlock(1, { { "P", 3 } }, 4);
        F.BuildBlock(2, { { "A", ~0ULL } }, ~0ULL);
        F.BuildBlock(3, { { "O", ~0ULL } }, ~0ULL);
        F.BuildBlock(4, { { "X", ~0ULL } }, ~0ULL);
        F.Run();
        Expect(!F.Walk.Unwalkable, Test, "the tree is walkable");
        Expect(F.Walk.Problems == 0, Test, "no problem raised");
        Expect(F.Walk.Trim[0] == F.Node(0)->IndexOffset, Test, "the whole node is cut");
        Expect(F.Walk.TrimChild[0] == 2, Test, "the node keeps the first entry's child as its END child");
        for (ULONG Block = 0; Block < 5; Block++)
            Expect(VolumeCheck::TestBit(F.Walk.Linked, Block), Test, "every node is linked once");
        Expect(F.Reached() == std::multiset<std::string>({ "A", "M", "O", "P", "X" }), Test,
               "every name is reached once");
    }
    {
        const char* Test = "median first, names missing elsewhere";
        Fixture F;

        /* The right node lacks T: the node is not a split cut short. */
        BuildNode(F.Root, sizeof(F.RootBuffer), { { "M", 0 } }, 1);
        F.BuildBlock(0, { { "P", ~0ULL }, { "T", ~0ULL } }, ~0ULL);
        F.BuildBlock(1, { { "P", ~0ULL } }, ~0ULL);
        F.Run();
        Expect(F.Walk.Unwalkable, Test, "the tree is unwalkable");
        Expect(F.Walk.Trim[0] == 0, Test, "nothing is cut");
    }
    {
        const char* Test = "median first, key below the range";
        Fixture F;

        /* Under a Low bound a key at or below it is not the node's own. */
        BuildNode(F.Root, sizeof(F.RootBuffer), { { "C", 0 }, { "M", 1 } }, 2);
        F.BuildBlock(0, { { "A", ~0ULL } }, ~0ULL);
        F.BuildBlock(1, { { "B", ~0ULL }, { "T", ~0ULL } }, ~0ULL);
        F.BuildBlock(2, { { "T", ~0ULL } }, ~0ULL);
        F.Run();
        Expect(F.Walk.Unwalkable, Test, "the tree is unwalkable");
        Expect(F.Walk.Trim[1] == 0, Test, "nothing is cut");
    }
}

}

int
main()
{
    InternalTail();
    LeafTail();
    NotASplit();
    MedianLast();
    MedianFirst();
    if (Failures)
        return 1;
    std::printf("check_split_trim: ok\n");
    return 0;
}
