/*
 * PROJECT:     ReactOS NTFS library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     A system file record whose $MFT copy is unreadable is read
 *              from $MFTMirr.
 *
 * $MFTMirr holds the first records of $MFT (at least 4: $MFT, $MFTMirr,
 * $LogFile, $Volume). GetFileRecord falls back to it when the $MFT copy of
 * a record fails its checks, and the fallback first asks whether the record
 * is in the mirror. That test was a macro without outer parentheses, so
 * "!IsFileRecordInMFTMirr(n)" read as "(!cond) ? n < x : n < 4": the
 * fallback refused exactly the records the mirror holds.
 *
 * The test breaks the "FILE" signature of $Volume (record 3) in $MFT, and
 * expects the volume to open and record 3 to be read, from the mirror.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <ntfslib_new.h>
#include "ntfsformat.h"
#include "ntfs_linux.h"

#define HOST_STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)

#ifndef O_BINARY
#define O_BINARY 0
#endif

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

bool
Check(bool Condition, const char* Message)
{
    if (!Condition)
        std::fprintf(stderr, "mft_mirror_fallback: %s\n", Message);
    return Condition;
}

bool
FormatImage(const char* Path)
{
    const ULONG BytesPerSector = 512;
    const ULONGLONG SizeBytes = 32ULL * 1024 * 1024;
    HostContext Host;
    NtfsFormatParameters Parameters;
    WCHAR Label[] = { 'M', 'I', 'R', 'R', 0 };
    NTSTATUS Status;

    Host.Descriptor = open(Path, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0644);
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
    Parameters.CurrentTime = (static_cast<ULONGLONG>(time(nullptr)) + 11644473600ULL) * 10000000ULL;
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

/* Byte offset of $MFT record Number, from the boot sector. */
bool
BreakMftRecordSignature(const char* Path, ULONG Number)
{
    int Descriptor = open(Path, O_RDWR | O_BINARY);
    unsigned char Boot[512];
    unsigned char Signature[4];
    ULONGLONG ClusterSize, MftOffset;
    signed char RecordSizeCode;
    ULONG RecordSize;
    bool Ok = false;

    if (Descriptor < 0)
    {
        perror("open");
        return false;
    }
    if (pread(Descriptor, Boot, sizeof(Boot), 0) != (ssize_t)sizeof(Boot))
        goto Out;
    ClusterSize = (ULONGLONG)(Boot[11] | (Boot[12] << 8)) * Boot[13];
    std::memcpy(&MftOffset, &Boot[0x30], sizeof(MftOffset));
    MftOffset *= ClusterSize;
    RecordSizeCode = (signed char)Boot[0x40];
    RecordSize = RecordSizeCode > 0 ? (ULONG)(RecordSizeCode * ClusterSize) : 1u << -RecordSizeCode;
    MftOffset += (ULONGLONG)Number * RecordSize;
    if (pread(Descriptor, Signature, 4, (off_t)MftOffset) != 4 ||
        !Check(std::memcmp(Signature, "FILE", 4) == 0, "record has no FILE signature"))
    {
        goto Out;
    }
    Ok = pwrite(Descriptor, "BAAD", 4, (off_t)MftOffset) == 4;

Out:
    fsync(Descriptor);
    close(Descriptor);
    return Check(Ok, "could not break the record");
}

} /* namespace */

int
main()
{
    const char* TempRoot = std::getenv("TMPDIR");
    std::string Template = std::string(TempRoot && *TempRoot ? TempRoot : "/tmp") +
                           "/ntfslib-mft-mirror-fallback-XXXXXX";
    std::vector<char> PathBuffer(Template.begin(), Template.end());
    char* Path;
    PNtfsVolume Volume = NULL;
    PNtfsFileRecord Record = NULL;
    ULONG BytesPerSector = 0;
    NTSTATUS Status;
    int Result = 1;
    int Fd;

    PathBuffer.push_back(0);
    Path = PathBuffer.data();
    Fd = mkstemp(Path);
    if (Fd < 0)
    {
        perror("mkstemp");
        return 1;
    }
    close(Fd);

    if (!FormatImage(Path) || !BreakMftRecordSignature(Path, 3))
        goto Out;

    Status = NtfsDiskInitializeLinux(Path, &BytesPerSector);
    if (!Check(NT_SUCCESS(Status), "NtfsDiskInitializeLinux failed"))
        goto Out;
    Status = NtfsProbePartitionAndOpenVolume(BytesPerSector, &Volume);
    std::printf("open with $Volume unreadable in $MFT: 0x%08x\n", (unsigned)Status);
    if (!Check(NT_SUCCESS(Status), "volume did not open from the mirror"))
        goto Out;

    Status = NtfsMasterFileTableGetFileRecordByReference(NtfsVolumeGetMft(Volume), (3ULL << 48) | 3, &Record);
    std::printf("record 3 by reference: 0x%08x\n", (unsigned)Status);
    if (!Check(NT_SUCCESS(Status), "record 3 not read from the mirror"))
        goto Out;

    std::printf("mft_mirror_fallback: passed\n");
    Result = 0;

Out:
    if (Record)
        NtfsFileRecordDestroy(Record);
    if (Volume)
        NtfsVolumeDestroy(Volume);
    NtfsDiskCloseLinux();
    unlink(Path);
    return Result;
}
