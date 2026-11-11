/*
 * PROJECT:     ReactOS NTFS library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     A size change made with the automatic timestamp fields masked
 *              out leaves LastWriteTime and ChangeTime alone; the same change
 *              with the handle's normal mask moves them.
 *
 * The driver persists the size that cached writes grew at cleanup (and just
 * before an explicit size change) through SetFileDataSize and
 * SetFileAllocationSize with the automatic fields masked, because that is
 * bookkeeping for writes that stamped the times when they happened; Windows
 * moves nothing at close (Lab03 L3 P1). This pins the library side of that:
 * every stamping site inside the two size calls honours the record's mask,
 * for a non-resident stream and for a resident one.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <unistd.h>

#include <ntfslib_new.h>
#include "ntfsformat.h"
#include "ntfs_linux.h"

#define HOST_STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)

namespace
{

struct HostContext
{
    int Descriptor;
};

NTSTATUS
HostWrite(void* Context, ULONGLONG Offset, ULONG Length, const void* Buffer)
{
    HostContext* Host = static_cast<HostContext*>(Context);
    const unsigned char* Bytes = static_cast<const unsigned char*>(Buffer);
    ULONG Done = 0;

    while (Done < Length)
    {
        ssize_t Written = pwrite(Host->Descriptor,
                                 Bytes + Done,
                                 Length - Done,
                                 static_cast<off_t>(Offset + Done));
        if (Written <= 0)
        {
            perror("pwrite");
            return HOST_STATUS_UNSUCCESSFUL;
        }
        Done += static_cast<ULONG>(Written);
    }
    return STATUS_SUCCESS;
}

void*
HostAllocate(void* Context, ULONG Length)
{
    (void)Context;
    return malloc(Length);
}

void
HostFree(void* Context, void* Buffer)
{
    (void)Context;
    free(Buffer);
}

BOOLEAN
HostProgress(void* Context, ULONG Percent)
{
    (void)Context;
    (void)Percent;
    return TRUE;
}

ULONGLONG
HostNtTime(void)
{
    return (static_cast<ULONGLONG>(time(nullptr)) + 11644473600ULL) *
           10000000ULL;
}

bool
Check(bool Condition, const char* Message)
{
    if (!Condition)
        std::fprintf(stderr, "size_persist_times: %s\n", Message);
    return Condition;
}

bool
FormatImage(const char* Path)
{
    const ULONG BytesPerSector = 512;
    const ULONGLONG SizeBytes = 16ULL * 1024 * 1024;
    HostContext Host;
    NtfsFormatParameters Parameters;
    WCHAR Label[] = { 'T', 'I', 'M', 'E', 'S', 0 };
    NTSTATUS Status;

    Host.Descriptor = open(Path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (Host.Descriptor < 0)
    {
        perror("open");
        return false;
    }
    if (ftruncate(Host.Descriptor, static_cast<off_t>(SizeBytes)) != 0)
    {
        perror("ftruncate");
        close(Host.Descriptor);
        return false;
    }

    std::memset(&Parameters, 0, sizeof(Parameters));
    Parameters.TotalSectors = SizeBytes / BytesPerSector;
    Parameters.BytesPerSector = BytesPerSector;
    Parameters.SectorsPerCluster = 8;
    Parameters.SectorsPerTrack = 63;
    Parameters.NumberOfHeads = 255;
    Parameters.MediaDescriptor = 0xF8;
    Parameters.CurrentTime = HostNtTime();
    Parameters.VolumeLabel = Label;
    Parameters.QuickFormat = TRUE;
    Parameters.IoContext = &Host;
    Parameters.Write = HostWrite;
    Parameters.Allocate = HostAllocate;
    Parameters.Free = HostFree;
    Parameters.Progress = HostProgress;

    Status = NtfsVolumeFormat(&Parameters);
    fsync(Host.Descriptor);
    close(Host.Descriptor);
    return Check(NT_SUCCESS(Status), "NtfsVolumeFormat failed");
}

/* Enough for a stamp to differ even on a coarse clock. */
void
LetTimePass(void)
{
    usleep(20000);
}

bool
Times(PNtfsFileRecord File, UINT64* LastWrite, UINT64* Change)
{
    NtfsFileBasicInformation Basic;

    if (!Check(NT_SUCCESS(NtfsFileRecordGetBasicInformation(File, &Basic)),
               "GetBasicInformation failed"))
    {
        return false;
    }
    *LastWrite = Basic.LastWriteTime;
    *Change = Basic.ChangeTime;
    return true;
}

/*
 * Shrinks the stream twice by SizeStep: first with the automatic fields
 * masked (the times must stay), then with the full mask (they must move).
 * TrimAllocation also exercises SetFileAllocationSize under the mask.
 */
bool
Exercise(PNtfsFileRecord File, const char* What, ULONGLONG Size, ULONGLONG SizeStep, bool TrimAllocation)
{
    UINT64 Lw0, Ct0, Lw1, Ct1, Lw2, Ct2;
    NTSTATUS Status;

    if (!Times(File, &Lw0, &Ct0))
        return false;
    LetTimePass();

    Status = NtfsFileRecordSetAutomaticTimestampMask(File, 0);
    if (!Check(NT_SUCCESS(Status), "SetAutomaticTimestampMask(0) failed"))
        return false;
    Status = NtfsFileRecordSetFileDataSize(File, TypeData, NULL, Size - SizeStep);
    if (!Check(NT_SUCCESS(Status), "masked SetFileDataSize failed"))
        return false;
    if (TrimAllocation)
    {
        Status = NtfsFileRecordSetFileAllocationSize(File, TypeData, NULL, Size - SizeStep);
        if (!Check(NT_SUCCESS(Status), "masked SetFileAllocationSize failed"))
            return false;
    }
    if (!Times(File, &Lw1, &Ct1))
        return false;
    std::printf("%s: masked size change: LastWriteTime %s, ChangeTime %s\n", What,
                Lw1 == Lw0 ? "same" : "MOVED", Ct1 == Ct0 ? "same" : "MOVED");
    if (!Check(Lw1 == Lw0, "masked size change moved LastWriteTime") ||
        !Check(Ct1 == Ct0, "masked size change moved ChangeTime"))
    {
        return false;
    }

    LetTimePass();
    Status = NtfsFileRecordSetAutomaticTimestampMask(File, NTFS_AUTOMATIC_TIMESTAMP_FIELDS);
    if (!Check(NT_SUCCESS(Status), "SetAutomaticTimestampMask(all) failed"))
        return false;
    Status = NtfsFileRecordSetFileDataSize(File, TypeData, NULL, Size - 2 * SizeStep);
    if (!Check(NT_SUCCESS(Status), "stamping SetFileDataSize failed"))
        return false;
    if (!Times(File, &Lw2, &Ct2))
        return false;
    std::printf("%s: explicit size change: LastWriteTime %s, ChangeTime %s\n", What,
                Lw2 != Lw1 ? "moved" : "SAME", Ct2 != Ct1 ? "moved" : "SAME");
    return Check(Lw2 != Lw1, "explicit size change left LastWriteTime") &&
           Check(Ct2 != Ct1, "explicit size change left ChangeTime");
}

PNtfsFileRecord
CreateFilled(PNtfsVolume Volume, PNtfsFileRecord Root, const WCHAR* Name, ULONG NameLength, ULONG Bytes)
{
    PNtfsFileRecord File = NULL;
    LARGE_INTEGER Offset;
    ULONG Length = Bytes;
    PUCHAR Buffer;
    NTSTATUS Status;

    Status = NtfsMasterFileTableCreateFileInDirectory(NtfsVolumeGetMft(Volume), Root,
                                                      const_cast<PWCHAR>(Name), NameLength,
                                                      FALSE, 0x20 /* FILE_ATTRIBUTE_ARCHIVE */,
                                                      TRUE, NULL, 0, &File);
    if (!Check(NT_SUCCESS(Status), "CreateFileInDirectory failed"))
        return NULL;

    Buffer = static_cast<PUCHAR>(malloc(Bytes));
    if (!Buffer)
    {
        NtfsFileRecordDestroy(File);
        return NULL;
    }
    std::memset(Buffer, 0x5A, Bytes);
    Offset.QuadPart = 0;
    Status = NtfsFileRecordWriteFileData(File, TypeData, NULL, Buffer, &Length, &Offset);
    free(Buffer);
    if (!Check(NT_SUCCESS(Status) && Length == Bytes, "WriteFileData failed"))
    {
        NtfsFileRecordDestroy(File);
        return NULL;
    }
    return File;
}

} /* namespace */

int
main()
{
    char Path[] = "/tmp/ntfslib-size-persist-times-XXXXXX";
    PNtfsVolume Volume = NULL;
    PNtfsFileRecord Root = NULL;
    PNtfsFileRecord Big = NULL;
    PNtfsFileRecord Small = NULL;
    ULONG BytesPerSector = 0;
    NTSTATUS Status;
    int Result = 1;
    int Fd;

    static const WCHAR BigName[] = { 'b', 'i', 'g', '.', 'b', 'i', 'n' };
    static const WCHAR SmallName[] = { 's', 'm', 'a', 'l', 'l', '.', 'b', 'i', 'n' };

    Fd = mkstemp(Path);
    if (Fd < 0)
    {
        perror("mkstemp");
        return 1;
    }
    close(Fd);

    if (!FormatImage(Path))
        goto Out;

    Status = NtfsDiskInitializeLinuxWritable(Path, &BytesPerSector);
    if (!Check(NT_SUCCESS(Status), "NtfsDiskInitializeLinuxWritable failed"))
        goto Out;
    NtfsSetReadOnlyMode(FALSE);
    Status = NtfsProbePartitionAndOpenVolume(BytesPerSector, &Volume);
    if (!Check(NT_SUCCESS(Status), "NtfsProbePartitionAndOpenVolume failed"))
        goto Out;

    /* A file reference carries the sequence number in its top 16 bits and the
     * lookup refuses a mismatch; the formatter gives record N sequence N. */
    Status = NtfsMasterFileTableGetFileRecordByReference(NtfsVolumeGetMft(Volume), (5ULL << 48) | 5, &Root);
    if (!NT_SUCCESS(Status))
        std::fprintf(stderr, "size_persist_times: root lookup status 0x%08x\n", static_cast<unsigned>(Status));
    if (!Check(NT_SUCCESS(Status), "root record not readable"))
        goto Out;

    /* 64 KiB: a non-resident $DATA, the common case of a grown file. */
    Big = CreateFilled(Volume, Root, BigName, 7, 64 * 1024);
    if (!Big)
        goto Out;
    if (!Exercise(Big, "non-resident", 64 * 1024, 8 * 1024, true))
        goto Out;

    /* 200 bytes: a resident $DATA, which the resident path resizes in place. */
    Small = CreateFilled(Volume, Root, SmallName, 9, 200);
    if (!Small)
        goto Out;
    if (!Exercise(Small, "resident", 200, 40, false))
        goto Out;

    Result = 0;

Out:
    if (Small)
        NtfsFileRecordDestroy(Small);
    if (Big)
        NtfsFileRecordDestroy(Big);
    if (Root)
        NtfsFileRecordDestroy(Root);
    if (Volume)
        NtfsVolumeDestroy(Volume);
    NtfsDiskCloseLinux();
    unlink(Path);
    return Result;
}
