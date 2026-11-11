/*
 * PROJECT:     ReactOS NTFS library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     A lookup that fails while it refills the volume's index-block
 *              buffer must not leave that buffer marked as holding the node
 *              it held before.
 *
 * Name lookups keep the last directory index block they read in one buffer
 * per volume, keyed by (directory, VCN). A refill reads the new block into
 * that buffer and then checks it; when the check fails (a bad fixup here),
 * the buffer holds the rejected block while the key still names the previous
 * one. The next lookup through the previous block then parsed the rejected
 * one and did not find a name that is there.
 *
 * The test fills a directory until its index spans several blocks, breaks
 * the fixup of one block on disk, and looks up a name in a good block, a name
 * in the broken block (must fail), and the first name again (must succeed).
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

const ULONG FileCount = 400;
const ULONG IndexBlockSize = 4096;

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
        std::fprintf(stderr, "lookup_index_cache: %s\n", Message);
    return Condition;
}

bool
FormatImage(const char* Path)
{
    const ULONG BytesPerSector = 512;
    const ULONGLONG SizeBytes = 32ULL * 1024 * 1024;
    HostContext Host;
    NtfsFormatParameters Parameters;
    WCHAR Label[] = { 'I', 'N', 'D', 'X', 0 };
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

std::string
NameOf(ULONG Index)
{
    char Name[64];

    std::snprintf(Name, sizeof(Name), "index_cache_entry_with_a_long_name_%04u.bin", Index);
    return Name;
}

std::vector<WCHAR>
Wide(const std::string& Text)
{
    return std::vector<WCHAR>(Text.begin(), Text.end());
}

bool
Populate(const char* Path)
{
    PNtfsVolume Volume = NULL;
    ULONG BytesPerSector = 0;
    WCHAR DirectoryPath[] = { '\\', 'd', 0 };
    PNtfsFileRecord File = NULL;
    NTSTATUS Status;
    bool Ok = false;

    Status = NtfsDiskInitializeLinuxWritable(Path, &BytesPerSector);
    if (!Check(NT_SUCCESS(Status), "NtfsDiskInitializeLinuxWritable failed"))
        return false;
    NtfsSetReadOnlyMode(FALSE);
    Status = NtfsProbePartitionAndOpenVolume(BytesPerSector, &Volume);
    if (!Check(NT_SUCCESS(Status), "open for populate failed"))
        goto Out;
    Status = NtfsMasterFileTableCreateFile(NtfsVolumeGetMft(Volume), DirectoryPath, 2, TRUE, 0, &File);
    if (!Check(NT_SUCCESS(Status), "create directory failed"))
        goto Out;
    NtfsFileRecordDestroy(File);

    for (ULONG Index = 0; Index < FileCount; Index++)
    {
        std::vector<WCHAR> Name = Wide("\\d\\" + NameOf(Index));

        File = NULL;
        Status = NtfsMasterFileTableCreateFile(NtfsVolumeGetMft(Volume),
                                               Name.data(), (ULONG)Name.size(),
                                               FALSE, 0, &File);
        if (!Check(NT_SUCCESS(Status), "create file failed"))
            goto Out;
        NtfsFileRecordDestroy(File);
    }
    Ok = true;

Out:
    if (Volume)
        NtfsVolumeDestroy(Volume);
    NtfsDiskCloseLinux();
    return Ok;
}

/* Index of the first of our names whose UTF-16 form lies whole in Block. */
long
FirstNameIn(const std::vector<unsigned char>& Block)
{
    for (ULONG Index = 0; Index < FileCount; Index++)
    {
        std::string Name = NameOf(Index);
        std::vector<unsigned char> Utf16;

        for (char Character : Name)
        {
            Utf16.push_back(static_cast<unsigned char>(Character));
            Utf16.push_back(0);
        }
        for (size_t Offset = 0; Offset + Utf16.size() <= Block.size(); Offset++)
        {
            if (std::memcmp(&Block[Offset], Utf16.data(), Utf16.size()) == 0)
                return (long)Index;
        }
    }
    return -1;
}

/*
 * Finds two index blocks holding our names, and breaks the fixup of the
 * second: the update sequence number no longer matches the end of its first
 * sector. Returns the name indexes found in the good and the broken block.
 */
bool
BreakOneIndexBlock(const char* Path, long* GoodName, long* BrokenName)
{
    int Descriptor = open(Path, O_RDWR | O_BINARY);
    std::vector<unsigned char> Block(IndexBlockSize);
    off_t End;
    off_t Broken = -1;

    *GoodName = -1;
    *BrokenName = -1;
    if (Descriptor < 0)
    {
        perror("open");
        return false;
    }
    End = lseek(Descriptor, 0, SEEK_END);
    for (off_t Offset = 0; Offset + (off_t)IndexBlockSize <= End; Offset += IndexBlockSize)
    {
        long Name;

        if (pread(Descriptor, Block.data(), IndexBlockSize, Offset) != (ssize_t)IndexBlockSize ||
            std::memcmp(Block.data(), "INDX", 4) != 0)
        {
            continue;
        }
        Name = FirstNameIn(Block);
        if (Name < 0)
            continue;
        if (*GoodName < 0)
        {
            *GoodName = Name;
        }
        else if (Name != *GoodName)
        {
            *BrokenName = Name;
            Broken = Offset;
            break;
        }
    }

    if (Broken >= 0)
    {
        unsigned char SectorEnd[2];

        /* Sector-end bytes must equal the update sequence number; flip them. */
        if (pread(Descriptor, SectorEnd, 2, Broken + 510) == 2)
        {
            SectorEnd[0] ^= 0xFF;
            SectorEnd[1] ^= 0xFF;
            if (pwrite(Descriptor, SectorEnd, 2, Broken + 510) != 2)
                Broken = -1;
        }
        else
        {
            Broken = -1;
        }
    }
    fsync(Descriptor);
    close(Descriptor);
    return Check(*GoodName >= 0 && Broken >= 0, "did not find two index blocks with our names");
}

/*
 * One name inside the open directory, as the driver resolves a leaf once it
 * holds the parent (NtfsMasterFileTableGetFileRecordInDirectory). The walk
 * starts at the directory's own index, so the first node it visits is the
 * one the previous lookup in this directory left in the buffer.
 */
NTSTATUS
LookUp(PNtfsVolume Volume, PNtfsFileRecord Directory, long Index, ULONGLONG* Reference)
{
    std::vector<WCHAR> Name = Wide(NameOf((ULONG)Index));
    PNtfsFileRecord File = NULL;
    NTSTATUS Status;

    *Reference = 0;
    Status = NtfsMasterFileTableGetFileRecordInDirectory(NtfsVolumeGetMft(Volume),
                                                         Directory,
                                                         Name.data(),
                                                         (ULONG)Name.size(),
                                                         &File);
    if (File)
    {
        *Reference = NtfsFileRecordGetReference(File);
        NtfsFileRecordDestroy(File);
    }
    return Status;
}

} /* namespace */

int
main()
{
    const char* TempRoot = std::getenv("TMPDIR");
    std::string Template = std::string(TempRoot && *TempRoot ? TempRoot : "/tmp") +
                           "/ntfslib-lookup-index-cache-XXXXXX";
    std::vector<char> PathBuffer(Template.begin(), Template.end());
    char* Path;
    PNtfsVolume Volume = NULL;
    PNtfsFileRecord Directory = NULL;
    WCHAR DirectoryPath[] = { '\\', 'd', 0 };
    ULONG Remaining = 0;
    ULONG BytesPerSector = 0;
    ULONGLONG First = 0, Again = 0, Ignored = 0;
    long GoodName, BrokenName;
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

    if (!FormatImage(Path) ||
        !Populate(Path) ||
        !BreakOneIndexBlock(Path, &GoodName, &BrokenName))
    {
        goto Out;
    }

    Status = NtfsDiskInitializeLinux(Path, &BytesPerSector);
    if (!Check(NT_SUCCESS(Status), "NtfsDiskInitializeLinux failed"))
        goto Out;
    Status = NtfsProbePartitionAndOpenVolume(BytesPerSector, &Volume);
    if (!Check(NT_SUCCESS(Status), "reopen failed"))
        goto Out;
    Status = NtfsMasterFileTableGetFileRecordFromQueryEx(NtfsVolumeGetMft(Volume),
                                                         DirectoryPath, 2, FALSE,
                                                         &Remaining, &Directory);
    if (!Check(NT_SUCCESS(Status) && Remaining == 0, "directory d not found"))
        goto Out;

    Status = LookUp(Volume, Directory, GoodName, &First);
    std::printf("lookup in the good block: 0x%08x\n", (unsigned)Status);
    if (!Check(NT_SUCCESS(Status), "name in a good block not found"))
        goto Out;

    Status = LookUp(Volume, Directory, BrokenName, &Ignored);
    std::printf("lookup in the broken block: 0x%08x\n", (unsigned)Status);
    if (!Check(!NT_SUCCESS(Status), "name in the broken block was found"))
        goto Out;

    Status = LookUp(Volume, Directory, GoodName, &Again);
    std::printf("lookup in the good block again: 0x%08x\n", (unsigned)Status);
    if (!Check(NT_SUCCESS(Status), "name in the good block lost after a failed lookup") ||
        !Check(Again == First, "name in the good block resolved to another file"))
    {
        goto Out;
    }

    std::printf("lookup_index_cache: passed\n");
    Result = 0;

Out:
    if (Directory)
        NtfsFileRecordDestroy(Directory);
    if (Volume)
        NtfsVolumeDestroy(Volume);
    NtfsDiskCloseLinux();
    unlink(Path);
    return Result;
}
