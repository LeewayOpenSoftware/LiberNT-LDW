/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NTFS FS library
 * FILE:        lib/fslib/ntfslib/ntfslib.c
 * PURPOSE:     NTFS lib
 * PROGRAMMERS: Pierre Schweitzer
 *              Ahmed ARIF
 */

#define NTOS_MODE_USER
#include <ndk/umtypes.h>
#include <ndk/iofuncs.h>
#include <ndk/kefuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>
#include <fmifs/fmifs.h>
#include <fslib/ntfslib.h>
#include <stdio.h>

#include <ntfsformat.h>
#include <ntfslib_new.h>
#include <ntfs_um.h>

#define NDEBUG
#include <debug.h>

#ifndef FSCTL_ALLOW_EXTENDED_DASD_IO
#define FSCTL_ALLOW_EXTENDED_DASD_IO \
    CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 32, METHOD_NEITHER, FILE_ANY_ACCESS)
#endif

typedef struct _NTFS_FORMAT_CONTEXT
{
    HANDLE FileHandle;
    PFMIFSCALLBACK Callback;
    ULONG Percent;
} NTFS_FORMAT_CONTEXT, *PNTFS_FORMAT_CONTEXT;

static
NTSTATUS
NtfsFormatWrite(
    IN PVOID Context,
    IN ULONGLONG Offset,
    IN ULONG Length,
    IN const VOID* Buffer)
{
    PNTFS_FORMAT_CONTEXT FormatContext = (PNTFS_FORMAT_CONTEXT)Context;
    IO_STATUS_BLOCK IoStatusBlock;
    LARGE_INTEGER ByteOffset;
    NTSTATUS Status;

    ByteOffset.QuadPart = (LONGLONG)Offset;

    Status = NtWriteFile(FormatContext->FileHandle,
                         NULL,
                         NULL,
                         NULL,
                         &IoStatusBlock,
                         (PVOID)Buffer,
                         Length,
                         &ByteOffset,
                         NULL);
    if (Status == STATUS_PENDING)
    {
        Status = NtWaitForSingleObject(FormatContext->FileHandle, FALSE, NULL);
        if (NT_SUCCESS(Status))
            Status = IoStatusBlock.Status;
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NtWriteFile() failed at offset %I64u (Status 0x%08x)\n",
                Offset, Status);
        return Status;
    }

    if (IoStatusBlock.Information != Length)
    {
        DPRINT1("Short write at offset %I64u: %lu of %lu bytes\n",
                Offset, (ULONG)IoStatusBlock.Information, Length);
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

static
PVOID
NtfsFormatAllocate(
    IN PVOID Context,
    IN ULONG Length)
{
    UNREFERENCED_PARAMETER(Context);
    return RtlAllocateHeap(RtlGetProcessHeap(), 0, Length);
}

static
VOID
NtfsFormatFree(
    IN PVOID Context,
    IN PVOID Buffer)
{
    UNREFERENCED_PARAMETER(Context);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
}

static
BOOLEAN
NtfsFormatProgress(
    IN PVOID Context,
    IN ULONG PercentComplete)
{
    PNTFS_FORMAT_CONTEXT FormatContext = (PNTFS_FORMAT_CONTEXT)Context;

    if (!FormatContext->Callback)
        return TRUE;

    FormatContext->Percent = PercentComplete;

    return FormatContext->Callback(PROGRESS,
                                   0,
                                   (PVOID)&FormatContext->Percent);
}

static
VOID
NtfsFormatReport(
    IN PFMIFSCALLBACK Callback,
    IN PCSTR Format,
    ...)
{
    TEXTOUTPUT TextOut;
    CHAR TextBuf[256];
    va_list Args;

    if (!Callback)
        return;

    va_start(Args, Format);
    _vsnprintf(TextBuf, sizeof(TextBuf) - 1, Format, Args);
    va_end(Args);
    TextBuf[sizeof(TextBuf) - 1] = ANSI_NULL;

    TextOut.Lines = 1;
    TextOut.Output = TextBuf;
    Callback(OUTPUT, 0, &TextOut);
}

/*
 * A check's decision (dirty, in use, problems found, restart needed): shown
 * like any report line and also written to the debug log, so a serial capture
 * has it however many per-problem lines came before.
 */
static
VOID
NtfsVerdictReport(
    IN PFMIFSCALLBACK Callback,
    IN PCSTR Format,
    ...)
{
    TEXTOUTPUT TextOut;
    CHAR TextBuf[256];
    va_list Args;

    va_start(Args, Format);
    _vsnprintf(TextBuf, sizeof(TextBuf) - 1, Format, Args);
    va_end(Args);
    TextBuf[sizeof(TextBuf) - 1] = ANSI_NULL;

    DPRINT1("NTFSLIB: %s", TextBuf);
    if (!Callback)
        return;
    TextOut.Lines = 1;
    TextOut.Output = TextBuf;
    Callback(OUTPUT, 0, &TextOut);
}

BOOLEAN
NTAPI
NtfsFormat(
    IN PUNICODE_STRING DriveRoot,
    IN PFMIFSCALLBACK Callback,
    IN BOOLEAN QuickFormat,
    IN BOOLEAN BackwardCompatible,
    IN MEDIA_TYPE MediaType,
    IN PUNICODE_STRING Label,
    IN ULONG ClusterSize)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    DISK_GEOMETRY DiskGeometry;
    PARTITION_INFORMATION_EX PartitionInfo;
    IO_STATUS_BLOCK IoStatusBlock;
    NTFS_FORMAT_CONTEXT Context;
    NtfsFormatParameters Parameters;
    WCHAR LabelBuffer[33];
    LARGE_INTEGER SystemTime;
    ULONGLONG PartitionLength;
    NTSTATUS Status, LockStatus;

    DPRINT("NtfsFormat(DriveRoot '%wZ')\n", DriveRoot);

    /* NTFS has no backwards-compatible on-disk variant to select. */
    UNREFERENCED_PARAMETER(BackwardCompatible);

    RtlZeroMemory(&Context, sizeof(Context));
    Context.Callback = Callback;

    InitializeObjectAttributes(&ObjectAttributes,
                               DriveRoot,
                               0,
                               NULL,
                               NULL);

    Status = NtOpenFile(&Context.FileHandle,
                        FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ,
                        FILE_SYNCHRONOUS_IO_ALERT);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NtOpenFile() failed with status 0x%08x\n", Status);
        NtfsFormatReport(Callback, "Unable to open the volume (status 0x%08lx)", Status);
        return FALSE;
    }

    Status = NtDeviceIoControlFile(Context.FileHandle,
                                   NULL,
                                   NULL,
                                   NULL,
                                   &IoStatusBlock,
                                   IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                   NULL,
                                   0,
                                   &DiskGeometry,
                                   sizeof(DiskGeometry));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("IOCTL_DISK_GET_DRIVE_GEOMETRY failed with status 0x%08x\n", Status);
        NtfsFormatReport(Callback, "Unable to query the drive geometry (status 0x%08lx)", Status);
        NtClose(Context.FileHandle);
        return FALSE;
    }

    if (DiskGeometry.MediaType == FixedMedia)
    {
        Status = NtDeviceIoControlFile(Context.FileHandle,
                                       NULL,
                                       NULL,
                                       NULL,
                                       &IoStatusBlock,
                                       IOCTL_DISK_GET_PARTITION_INFO_EX,
                                       NULL,
                                       0,
                                       &PartitionInfo,
                                       sizeof(PartitionInfo));
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("IOCTL_DISK_GET_PARTITION_INFO_EX failed with status 0x%08x\n", Status);
            NtfsFormatReport(Callback, "Unable to query the partition information (status 0x%08lx)", Status);
            NtClose(Context.FileHandle);
            return FALSE;
        }

        PartitionLength = (ULONGLONG)PartitionInfo.PartitionLength.QuadPart;
    }
    else
    {
        RtlZeroMemory(&PartitionInfo, sizeof(PartitionInfo));
        PartitionInfo.PartitionStyle = PARTITION_STYLE_MBR;

        PartitionLength = DiskGeometry.Cylinders.QuadPart *
                          (ULONGLONG)DiskGeometry.TracksPerCylinder *
                          (ULONGLONG)DiskGeometry.SectorsPerTrack *
                          (ULONGLONG)DiskGeometry.BytesPerSector;
    }

    if (DiskGeometry.BytesPerSector == 0)
    {
        DPRINT1("Device reported a zero sector size\n");
        NtfsFormatReport(Callback, "The device reports a sector size of zero");
        NtClose(Context.FileHandle);
        return FALSE;
    }

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.TotalSectors = PartitionLength / DiskGeometry.BytesPerSector;
    Parameters.BytesPerSector = DiskGeometry.BytesPerSector;

    /* ClusterSize arrives in bytes; 0 lets the formatter choose. */
    if (ClusterSize != 0)
        Parameters.SectorsPerCluster = ClusterSize / DiskGeometry.BytesPerSector;

    Parameters.SectorsPerTrack = (USHORT)DiskGeometry.SectorsPerTrack;
    Parameters.NumberOfHeads = (USHORT)DiskGeometry.TracksPerCylinder;

    if (PartitionInfo.PartitionStyle == PARTITION_STYLE_MBR)
        Parameters.HiddenSectors = PartitionInfo.Mbr.HiddenSectors;

    /* MediaType is already an NT MEDIA_TYPE here: FormatVolume() converts the
     * FMIFS media flag before calling us. */
    Parameters.MediaDescriptor = (MediaType == FixedMedia) ? 0xF8 : 0xF0;

    NtQuerySystemTime(&SystemTime);
    Parameters.CurrentTime = (ULONGLONG)SystemTime.QuadPart;

    if (Label && Label->Buffer && Label->Length != 0)
    {
        ULONG LabelLength = Label->Length / sizeof(WCHAR);

        if (LabelLength > RTL_NUMBER_OF(LabelBuffer) - 1)
            LabelLength = RTL_NUMBER_OF(LabelBuffer) - 1;

        RtlCopyMemory(LabelBuffer, Label->Buffer, LabelLength * sizeof(WCHAR));
        LabelBuffer[LabelLength] = UNICODE_NULL;
        Parameters.VolumeLabel = LabelBuffer;
    }

    Parameters.QuickFormat = QuickFormat;
    Parameters.IoContext = &Context;
    Parameters.Write = NtfsFormatWrite;
    Parameters.Allocate = NtfsFormatAllocate;
    Parameters.Free = NtfsFormatFree;
    Parameters.Progress = NtfsFormatProgress;

    if (Callback != NULL)
    {
        Context.Percent = 0;
        Callback(PROGRESS, 0, (PVOID)&Context.Percent);
    }

    LockStatus = NtFsControlFile(Context.FileHandle,
                                 NULL,
                                 NULL,
                                 NULL,
                                 &IoStatusBlock,
                                 FSCTL_LOCK_VOLUME,
                                 NULL,
                                 0,
                                 NULL,
                                 0);
    if (!NT_SUCCESS(LockStatus))
    {
        DPRINT1("Failed to lock volume for formatting (Status: 0x%x)\n",
                LockStatus);
        NtfsFormatReport(Callback, "Unable to lock the volume (status 0x%08lx)", LockStatus);
        NtClose(Context.FileHandle);
        return FALSE;
    }

    /*
     * A mounted filesystem normally bounds raw I/O to the size recorded in
     * its own boot sector. That size can be smaller than the current
     * partition after delete/recreate or resize operations. Allow the
     * formatter to address the complete partition reported above.
     */
    LockStatus = NtFsControlFile(Context.FileHandle,
                                 NULL,
                                 NULL,
                                 NULL,
                                 &IoStatusBlock,
                                 FSCTL_ALLOW_EXTENDED_DASD_IO,
                                 NULL,
                                 0,
                                 NULL,
                                 0);
    /* RawFS and filesystems that already expose the full device need not
     * implement this control. */
    if (!NT_SUCCESS(LockStatus) &&
        LockStatus != STATUS_INVALID_PARAMETER &&
        LockStatus != STATUS_INVALID_DEVICE_REQUEST &&
        LockStatus != STATUS_NOT_SUPPORTED)
    {
        DPRINT1("Failed to enable extended raw volume I/O (Status: 0x%x)\n",
                LockStatus);
        NtfsFormatReport(Callback, "Unable to enable raw access to the whole volume (status 0x%08lx)", LockStatus);
        NtFsControlFile(Context.FileHandle,
                        NULL,
                        NULL,
                        NULL,
                        &IoStatusBlock,
                        FSCTL_UNLOCK_VOLUME,
                        NULL,
                        0,
                        NULL,
                        0);
        NtClose(Context.FileHandle);
        return FALSE;
    }

    Status = NtfsVolumeFormat(&Parameters);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NtfsVolumeFormat() failed with status 0x%08x\n", Status);
        NtfsFormatReport(Callback, "Unable to write the NTFS structures (status 0x%08lx)", Status);
    }

    /* Attempt to dismount the formatted volume */
    LockStatus = NtFsControlFile(Context.FileHandle,
                                 NULL,
                                 NULL,
                                 NULL,
                                 &IoStatusBlock,
                                 FSCTL_DISMOUNT_VOLUME,
                                 NULL,
                                 0,
                                 NULL,
                                 0);
    if (!NT_SUCCESS(LockStatus))
        DPRINT1("Failed to umount volume (Status: 0x%x)\n", LockStatus);

    LockStatus = NtFsControlFile(Context.FileHandle,
                                 NULL,
                                 NULL,
                                 NULL,
                                 &IoStatusBlock,
                                 FSCTL_UNLOCK_VOLUME,
                                 NULL,
                                 0,
                                 NULL,
                                 0);
    if (!NT_SUCCESS(LockStatus))
        DPRINT1("Failed to unlock volume (Status: 0x%x)\n", LockStatus);

    NtClose(Context.FileHandle);

    DPRINT("NtfsFormat() done. Status 0x%08x\n", Status);
    return NT_SUCCESS(Status);
}

typedef struct _NTFS_CHECK_CONTEXT
{
    PFMIFSCALLBACK Callback;
    BOOLEAN Verbose;
    ULONG Reported;
    ULONG RelinkDiagnosed;
} NTFS_CHECK_CONTEXT, *PNTFS_CHECK_CONTEXT;

/* Relink diagnostics come after every per-problem line, so the cap of 32
 * would always hide them: they get a small cap of their own and go to the
 * debug log as well. A verbose check (autochk at boot) shows nearly all of
 * them, since its log is the record of what the repair did. */
#define NTFS_RELINK_DIAGNOSTICS 16

/* NtfsShapeInvariant, in order. */
static const PCSTR NtfsShapeInvariantNames[] =
{
    "none",
    "record unreadable",
    "record malformed",
    "not a base record",
    "attribute list unreadable",
    "bad attribute list entry",
    "attribute list names a missing attribute",
    "attribute not in the attribute list",
    "extension record without an attribute list",
    "extension record not in the attribute list",
    "mapping does not cover the allocation",
    "bad index root",
    "index block beyond the allocation",
    "invalid index block",
    "index block in use but marked free",
    "index block referenced twice",
    "index block in use but unreachable"
};
#define NTFS_RELINK_DIAGNOSTICS_VERBOSE 4000

static
VOID
NtfsCheckReport(
    IN PVOID Context,
    IN NtfsCheckEvent Event,
    IN UINT64 Argument1,
    IN UINT64 Argument2)
{
    PNTFS_CHECK_CONTEXT Check = (PNTFS_CHECK_CONTEXT)Context;

    if (Event == NtfsCheckNameRelinkFailed || Event == NtfsCheckNameFoundUncounted)
    {
        ULONG Limit = Check->Verbose ? NTFS_RELINK_DIAGNOSTICS_VERBOSE : NTFS_RELINK_DIAGNOSTICS;

        if (Check->RelinkDiagnosed > Limit)
            return;
        if (Check->RelinkDiagnosed++ == Limit)
        {
            NtfsVerdictReport(Check->Callback, "Further relink diagnostics are not shown.\n");
            return;
        }
        if (Event == NtfsCheckNameRelinkFailed)
            NtfsVerdictReport(Check->Callback,
                              "A name of file %I64X could not be added to the index of directory file %I64X (status 0x%08lx).\n",
                              Argument1, Argument2 & 0xFFFFFFFFULL, (ULONG)(Argument2 >> 32));
        else
            NtfsVerdictReport(Check->Callback,
                              "A name of file %I64X is found in directory file %I64X by lookup, but the index walk did not count it.\n",
                              Argument1, Argument2);
        return;
    }

    /* Shape verdicts are never capped: the first one decides the result. */
    if (Event == NtfsCheckShapeFault)
    {
        ULONG Invariant = (ULONG)(Argument1 >> 48);

        NtfsVerdictReport(Check->Callback,
                          "The shape check found a problem in file record segment %I64X: invariant %lu (%s), detail 0x%I64x.\n",
                          Argument1 & 0x0000FFFFFFFFFFFFULL, Invariant,
                          Invariant < RTL_NUMBER_OF(NtfsShapeInvariantNames) ? NtfsShapeInvariantNames[Invariant] : "unknown",
                          Argument2);
        return;
    }
    if (Event == NtfsCheckShapeIndexesSkipped)
    {
        NtfsVerdictReport(Check->Callback,
                          "The shape check skipped %I64u indexes larger than %I64u MB.\n",
                          Argument1, Argument2 / (1024 * 1024));
        return;
    }
    if (Event == NtfsCheckShapeNotRun)
    {
        NtfsVerdictReport(Check->Callback,
                          "The shape check could not run (status 0x%08lx).\n", (ULONG)Argument1);
        return;
    }

    if (Event != NtfsCheckLostClusters && Event != NtfsCheckRepairsBlocked &&
        !Check->Verbose && Check->Reported >= 32)
        return;
    Check->Reported++;

    switch (Event)
    {
        case NtfsCheckRecordCorrupt:
            NtfsFormatReport(Check->Callback, "File record segment %I64X is corrupt.\n", Argument1);
            break;
        case NtfsCheckRecordBitmap:
            NtfsFormatReport(Check->Callback,
                             Argument2 ? "File record segment %I64X is in use but marked free in the MFT bitmap.\n"
                                       : "File record segment %I64X is free but marked in use in the MFT bitmap.\n",
                             Argument1);
            break;
        case NtfsCheckBadRunList:
            NtfsFormatReport(Check->Callback,
                             "Attribute type 0x%I64x in file record segment %I64X has an invalid mapping pairs array.\n",
                             Argument2, Argument1);
            break;
        case NtfsCheckClusterOutOfRange:
            NtfsFormatReport(Check->Callback,
                             "File record segment %I64X refers to cluster 0x%I64x beyond the end of the volume.\n",
                             Argument1, Argument2);
            break;
        case NtfsCheckCrossLinked:
            NtfsFormatReport(Check->Callback,
                             "File record segment %I64X shares cluster 0x%I64x with another file.\n",
                             Argument1, Argument2);
            break;
        case NtfsCheckIndexCorrupt:
            NtfsFormatReport(Check->Callback,
                             "Index $I30 of file %I64X is corrupt at VCN 0x%I64x.\n",
                             Argument1, Argument2);
            break;
        case NtfsCheckIndexBadEntry:
            NtfsFormatReport(Check->Callback,
                             "An index entry in index $I30 of file %I64X points to unused or stale file 0x%I64x.\n",
                             Argument1, Argument2);
            break;
        case NtfsCheckIndexStaleEntry:
            NtfsFormatReport(Check->Callback,
                             "An index entry in index $I30 of file %I64X has a stale reference 0x%I64x.\n",
                             Argument1, Argument2);
            break;
        case NtfsCheckIndexEntryFixed:
            NtfsFormatReport(Check->Callback,
                             "Corrected %I64u index entries in index $I30 of file %I64X.\n",
                             Argument2, Argument1);
            break;
        case NtfsCheckIndexEntryRemoved:
            NtfsFormatReport(Check->Callback,
                             "Deleted the index entry for file 0x%I64x from index $I30 of file %I64X.\n",
                             Argument2, Argument1);
            break;
        case NtfsCheckNameNotIndexed:
            NtfsFormatReport(Check->Callback,
                             "A name of file %I64X is missing from its parent index.\n",
                             Argument1);
            break;
        case NtfsCheckUsedClustersFree:
            NtfsFormatReport(Check->Callback,
                             "Clusters 0x%I64x to 0x%I64x are in use but marked free in the volume bitmap.\n",
                             Argument1, Argument1 + Argument2 - 1);
            break;
        case NtfsCheckLostClusters:
            NtfsFormatReport(Check->Callback,
                             "%I64u clusters are marked allocated in the volume bitmap but not in use.\n",
                             Argument1);
            break;
        case NtfsCheckUsedClustersClaimed:
            NtfsFormatReport(Check->Callback,
                             "Marked clusters 0x%I64x to 0x%I64x as in use in the volume bitmap.\n",
                             Argument1, Argument1 + Argument2 - 1);
            break;
        case NtfsCheckRecordFreed:
            NtfsFormatReport(Check->Callback, "Deleted corrupt file record segment %I64X.\n", Argument1);
            break;
        case NtfsCheckRecordBitmapFixed:
            NtfsFormatReport(Check->Callback, "Corrected %I64u bits in the MFT bitmap.\n", Argument1);
            break;
        case NtfsCheckNameRelinked:
            NtfsFormatReport(Check->Callback,
                             "Recovered orphaned file %I64X into directory file %I64X.\n",
                             Argument1, Argument2);
            break;
        case NtfsCheckLostClustersFreed:
            NtfsFormatReport(Check->Callback,
                             "Freed clusters 0x%I64x to 0x%I64x in the volume bitmap.\n",
                             Argument1, Argument1 + Argument2 - 1);
            break;
        case NtfsCheckCrossLinkResolved:
            NtfsFormatReport(Check->Callback,
                             "Moved the shared clusters of file %I64X to cluster 0x%I64x.\n",
                             Argument1, Argument2);
            break;
        case NtfsCheckLostAndFound:
            NtfsFormatReport(Check->Callback,
                             "Recovering the remaining unindexed files. Lost and found is located at \\found.%03I64u\n",
                             Argument2);
            break;
        case NtfsCheckFileRecovered:
            NtfsFormatReport(Check->Callback,
                             "Recovered file %I64X into directory file %I64X.\n",
                             Argument1, Argument2);
            break;
        case NtfsCheckAttributeListBad:
            NtfsFormatReport(Check->Callback,
                             "%I64u attribute list entries in file %I64X refer to corrupt or unused file record segments.\n",
                             Argument2, Argument1);
            break;
        case NtfsCheckAttributeListFixed:
            NtfsFormatReport(Check->Callback,
                             "Deleted %I64u corrupt attribute list entries in file %I64X.\n",
                             Argument2, Argument1);
            break;
        case NtfsCheckDataAttributeInserted:
            NtfsFormatReport(Check->Callback, "Inserting data attribute into file %I64X.\n", Argument1);
            break;
        case NtfsCheckRepairsBlocked:
            if (Argument2 == ~0ULL)
                NtfsVerdictReport(Check->Callback,
                                  "Index repairs and freeing of unused clusters were skipped: %I64u structural problems remain.\n",
                                  Argument1);
            else
                NtfsVerdictReport(Check->Callback,
                                  "Index repairs and freeing of unused clusters were skipped: %I64u structural problems remain (first in file record segment %I64X).\n",
                                  Argument1, Argument2);
            break;
        case NtfsCheckLostNameRemoved:
            NtfsFormatReport(Check->Callback,
                             "Removed %I64u names of file %I64X that belonged to a deleted directory.\n",
                             Argument2, Argument1);
            break;
        case NtfsCheckNameRelinkFailed:
        case NtfsCheckNameFoundUncounted:
            /* Reported above, under their own cap. */
            break;
        case NtfsCheckShapeFault:
        case NtfsCheckShapeIndexesSkipped:
        case NtfsCheckShapeNotRun:
            /* Reported above, never capped. */
            break;
        case NtfsCheckIndexNameMissing:
            NtfsFormatReport(Check->Callback,
                             "An index entry in index $I30 of file %I64X names file 0x%I64x, which does not have that name.\n",
                             Argument1, Argument2);
            break;
        case NtfsCheckAttributeListUnreadable:
            NtfsFormatReport(Check->Callback,
                             "The attribute list in file %I64X cannot be read (status 0x%08lx).\n",
                             Argument1, (ULONG)Argument2);
            break;
        case NtfsCheckAttributeListRemoved:
            NtfsFormatReport(Check->Callback,
                             "Removed the attribute list of file %I64X: every attribute is in the base record.\n",
                             Argument1);
            break;
        case NtfsCheckIndexBlockUnreferenced:
            NtfsFormatReport(Check->Callback,
                             "Index $I30 of file %I64X has an unreferenced block at VCN 0x%I64x.\n",
                             Argument1, Argument2);
            break;
        case NtfsCheckIndexReset:
            NtfsFormatReport(Check->Callback,
                             "Rebuilding index $I30 of file %I64X (%I64u blocks dropped).\n",
                             Argument1, Argument2);
            break;
        case NtfsCheckIndexDuplicateEntry:
            NtfsFormatReport(Check->Callback,
                             "Index $I30 of file %I64X holds a second entry for file 0x%I64x.\n",
                             Argument1, Argument2);
            break;
        case NtfsCheckAttributeListRebuilt:
            NtfsFormatReport(Check->Callback,
                             "Rebuilt the unreadable attribute list of file %I64X from %I64u extension records.\n",
                             Argument1, Argument2);
            break;
        case NtfsCheckIndexTailTrimmed:
            NtfsFormatReport(Check->Callback,
                             "Index $I30 of file %I64X held entries beyond its range at VCN 0x%I64x.\n",
                             Argument1, Argument2);
            break;
        case NtfsCheckAttributeListUnneeded:
            NtfsFormatReport(Check->Callback,
                             "The attribute list of file %I64X is not needed: every entry is in the base record.\n",
                             Argument1);
            break;
        case NtfsCheckRecordBeyondMft:
            NtfsFormatReport(Check->Callback,
                             "File record segments %I64X to %I64X (%I64u) are marked in use in the MFT bitmap "
                             "but lie past the end of the MFT data, so they read as zeros.\n",
                             Argument1, Argument1 + Argument2 - 1, Argument2);
            break;
        case NtfsCheckRecordBeyondMftInUse:
            NtfsFormatReport(Check->Callback,
                             "File record segment %I64X is in use but lies past the end of the MFT data. "
                             "It was left unchanged.\n",
                             Argument1);
            break;
        case NtfsCheckAttributeSizeInvalid:
            NtfsFormatReport(Check->Callback,
                             "Attribute type 0x%I64X of file %I64X has invalid sizes: its data size is larger than "
                             "its allocated size, its initialized size is larger than its data size, or its "
                             "allocated size is not a whole number of clusters.\n",
                             Argument2, Argument1);
            break;
        case NtfsCheckAttributeMappingMismatch:
            NtfsFormatReport(Check->Callback,
                             "The cluster mapping of attribute type 0x%I64X of file %I64X does not cover "
                             "exactly its allocated size.\n",
                             Argument2, Argument1);
            break;
        case NtfsCheckIndexEntryNotRemoved:
            NtfsFormatReport(Check->Callback,
                             "Could not delete the index entry for file 0x%I64x from index $I30 of file %I64X; "
                             "the record it names is kept in use.\n",
                             Argument2, Argument1);
            break;
        case NtfsCheckRecordKept:
            NtfsFormatReport(Check->Callback,
                             "File record segment %I64X has damaged attributes (%I64u problems). It was not deleted, "
                             "so no other repair was made; copy its data elsewhere before repairing it.\n",
                             Argument1, Argument2);
            break;
        case NtfsCheckMftBitmapNotWritten:
            NtfsFormatReport(Check->Callback,
                             "The corrected MFT bitmap (%I64u bytes) could not be written (status 0x%08lx); "
                             "the records it frees stay marked in use.\n",
                             Argument2, (ULONG)Argument1);
            break;
    }
}

static
NTSTATUS
NtfsCheckControl(
    IN HANDLE Handle,
    IN ULONG Code,
    OUT PVOID Output,
    IN ULONG OutputLength)
{
    IO_STATUS_BLOCK IoStatusBlock;

    return NtFsControlFile(Handle, NULL, NULL, NULL, &IoStatusBlock, Code, NULL, 0, Output, OutputLength);
}

static
BOOLEAN
NtfsNeedsOnlineScanOnly(
    IN HANDLE Handle)
{
    PNtfsVolume Volume = NULL;
    ULONG BytesPerSector = 0;
    USHORT Flags = 0;
    NTSTATUS Status;

    Status = NtfsDiskInitializeUm(Handle, &BytesPerSector);
    if (NT_SUCCESS(Status))
    {
        NtfsSetReadOnlyMode(TRUE);
        Status = NtfsProbePartitionAndOpenVolume(BytesPerSector, &Volume);
        NtfsSetReadOnlyMode(FALSE);
    }
    if (NT_SUCCESS(Status))
        Status = NtfsVolumeQueryFlags(Volume, &Flags);
    if (Volume)
        NtfsVolumeDestroy(Volume);
    return NT_SUCCESS(Status) && (Flags & VOL_DIRTY) && !NtfsVolumeNeedsStartupRepair(Flags);
}

BOOLEAN
NTAPI
NtfsChkdsk(
    IN PUNICODE_STRING DriveRoot,
    IN PFMIFSCALLBACK Callback,
    IN BOOLEAN FixErrors,
    IN BOOLEAN Verbose,
    IN BOOLEAN CheckOnlyIfDirty,
    IN BOOLEAN ScanDrive,
    IN PVOID pUnknown1,
    IN PVOID pUnknown2,
    IN PVOID pUnknown3,
    IN PVOID pUnknown4,
    IN PULONG ExitStatus)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    NTFS_CHECK_CONTEXT Check;
    NtfsCheckResult Result;
    PNtfsVolume Volume = NULL;
    HANDLE Handle;
    ULONG BytesPerSector = 0;
    ULONG DirtyMask = 0;
    USHORT Flags = 0;
    BOOLEAN Locked = FALSE;
    ULONG CheckWrites = 0;
    BOOLEAN Dirty;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ScanDrive);
    UNREFERENCED_PARAMETER(pUnknown1);
    UNREFERENCED_PARAMETER(pUnknown2);
    UNREFERENCED_PARAMETER(pUnknown3);
    UNREFERENCED_PARAMETER(pUnknown4);

    *ExitStatus = (ULONG)STATUS_DISK_CORRUPT_ERROR;
    RtlZeroMemory(&Check, sizeof(Check));
    Check.Callback = Callback;
    Check.Verbose = Verbose;

    InitializeObjectAttributes(&ObjectAttributes, DriveRoot, 0, NULL, NULL);
    Status = NtOpenFile(&Handle,
                        FILE_GENERIC_READ | (FixErrors ? FILE_GENERIC_WRITE : 0) | SYNCHRONIZE,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_SYNCHRONOUS_IO_ALERT);
    if (!NT_SUCCESS(Status))
    {
        NtfsFormatReport(Callback, "Unable to open the volume (status 0x%08lx).\n", Status);
        return FALSE;
    }

    Status = NtfsCheckControl(Handle, FSCTL_IS_VOLUME_DIRTY, &DirtyMask, sizeof(DirtyMask));
    Dirty = NT_SUCCESS(Status) && (DirtyMask & VOLUME_IS_DIRTY);
    if (CheckOnlyIfDirty && NT_SUCCESS(Status) && !Dirty)
    {
        NtClose(Handle);
        *ExitStatus = (ULONG)STATUS_SUCCESS;
        return TRUE;
    }
    if (CheckOnlyIfDirty && Dirty && !FixErrors)
    {
        if (NtfsNeedsOnlineScanOnly(Handle))
        {
            NtfsVerdictReport(Callback, "The volume needs an online scan, which is not run at startup.\n");
            NtClose(Handle);
            *ExitStatus = (ULONG)STATUS_SUCCESS;
            return TRUE;
        }
        NtfsVerdictReport(Callback, "The volume is dirty.\n");
        NtClose(Handle);
        return TRUE;
    }

    if (FixErrors)
    {
        Status = NtfsCheckControl(Handle, FSCTL_LOCK_VOLUME, NULL, 0);
        Locked = NT_SUCCESS(Status);
        if (!Locked)
            NtfsVerdictReport(Callback, "The volume is in use, so problems will be reported but not fixed.\n");
    }

    Status = NtfsDiskInitializeUm(Handle, &BytesPerSector);
    if (NT_SUCCESS(Status))
    {
        NtfsSetReadOnlyMode(Locked ? FALSE : TRUE);
        Status = NtfsProbePartitionAndOpenVolume(BytesPerSector, &Volume);
        NtfsSetReadOnlyMode(FALSE);
    }
    if (NT_SUCCESS(Status) && NT_SUCCESS(NtfsVolumeQueryFlags(Volume, &Flags)) && (Flags & VOL_DIRTY))
        Dirty = TRUE;
    if (NT_SUCCESS(Status))
    {
        NtfsFormatReport(Callback, "Checking file records, indexes and the volume bitmap...\n");
        Status = NtfsVolumeCheck(Volume, Locked, NtfsCheckReport, &Check, &Result);
        /* Writes made by the check itself, before the dirty flag is cleared. */
        CheckWrites = NtfsDiskWriteCountUm();
    }
    /* VD4: do NOT clear the on-disk dirty flag while the mounted FSD still holds
     * a stale lifecycle. Dismount first and clear only after a SUCCESSFUL
     * dismount, so a failed dismount leaves BOTH the mounted instance's
     * DirtyMarked state AND the on-disk flag dirty (consistent: re-checked next
     * boot) rather than clearing the medium under a stale instance that would
     * then skip MarkDirtyBeforeWrite on later writes. The raw clear still runs
     * through the locked handle (VPB_DIRECT_WRITES_ALLOWED), as chkdsk /f does. */
    if (Locked)
    {
        NTSTATUS DismountStatus =
            NtfsCheckControl(Handle, FSCTL_DISMOUNT_VOLUME, NULL, 0);

        if (NT_SUCCESS(Status) && Dirty && Result.Errors == 0)
        {
            if (NT_SUCCESS(DismountStatus))
                (void)NtfsVolumeSetFlags(Volume, 0, VOL_DIRTY | VOL_FULL_CHKDSK | VOL_ONLINE_SCAN);
            else
                NtfsVerdictReport(Callback,
                    "The volume could not be dismounted after the check (status 0x%08lx); the dirty flag was left set and it will be checked again next boot.\n",
                    DismountStatus);
        }
        NtfsCheckControl(Handle, FSCTL_UNLOCK_VOLUME, NULL, 0);
    }

    if (Volume)
        NtfsVolumeDestroy(Volume);
    NtClose(Handle);

    if (!NT_SUCCESS(Status))
    {
        NtfsVerdictReport(Callback, "The volume could not be checked (status 0x%08lx).\n", Status);
        return FALSE;
    }

    NtfsFormatReport(Callback, "%I64u file records processed.\n", Result.FileRecords);
    NtfsFormatReport(Callback, "%I64u index entries processed in %I64u directories.\n",
                     Result.IndexEntries, Result.Directories);
    if (Result.ClaimedClusters)
        NtfsFormatReport(Callback, "%I64u clusters in use were marked allocated.\n", Result.ClaimedClusters);
    if (Result.FreedClusters)
        NtfsFormatReport(Callback, "%I64u clusters were returned to free space.\n", Result.FreedClusters);
    if (Result.Errors)
    {
        NtfsVerdictReport(Callback, "Problems were found on the volume (%I64u left of %I64u found).\n",
                          Result.Errors, Result.FoundErrors);
        /* The check may still have repaired part of the volume around the
         * mounted file system, which was dismounted above: what was opened on
         * it before is stale, so restart as after a full repair. The dirty
         * flag stays set and the next boot checks again. Restart only when
         * the check wrote AND fewer problems are left than it found: a check
         * that writes nothing, or a repair that writes on every boot without
         * removing anything, does not restart, so neither can loop. */
        if (Locked && CheckWrites != 0 && Result.Errors < Result.FoundErrors)
        {
            NtfsVerdictReport(Callback, "%lu writes were made to the volume; it must be restarted.\n",
                             CheckWrites);
            *ExitStatus = NTFSLIB_CHKDSK_VOLUME_RELOADED;
        }
        return TRUE;
    }

    NtfsVerdictReport(Callback, "No problems were found on the volume.\n");
    /* The repair went around the mounted file system, which was then
     * dismounted: only a restart drops what was opened on it before. */
    *ExitStatus = Locked ? NTFSLIB_CHKDSK_VOLUME_RELOADED : (ULONG)STATUS_SUCCESS;
    return TRUE;
}
