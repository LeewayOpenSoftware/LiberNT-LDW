/*
 * PROJECT:     ReactOS AutoChk
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     FileSystem checker in Native mode.
 * COPYRIGHT:   Copyright 2002-2018 Eric Kohl
 *              Copyright 2006-2018 Aleksey Bragin
 *              Copyright 2006-2018 Hervé Poussineau
 *              Copyright 2008-2018 Pierre Schweitzer
 *              Copyright 2018 Hermes Belusca-Maito
 */

/* INCLUDES *****************************************************************/

#include <stdio.h>
#include <string.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <ntddkbd.h>

#define NTOS_MODE_USER
#include <ndk/exfuncs.h>
#include <ndk/iofuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/psfuncs.h>
#include <ndk/rtlfuncs.h>
#include <ndk/setypes.h>
#include <fmifs/fmifs.h>

#include <fslib/vfatlib.h>
#include <fslib/vfatxlib.h>
#include <fslib/ntfslib.h>
#include <fslib/btrfslib.h>
#include <fslib/ext2lib.h>
#include <fslib/cdfslib.h>

#define NDEBUG
#include <debug.h>

/* DEFINES ******************************************************************/

/* Complete checker output lines copied to the debug log before the mirror
 * stops. The serial log is the only record a lab keeps of a boot-time
 * repair, and at 115200 baud a few thousand lines cost seconds. */
#define AUTOCHK_MIRROR_LINES 4000

typedef struct _FILESYSTEM_CHKDSK
{
    WCHAR Name[10];
    PULIB_CHKDSK ChkdskFunc;
} FILESYSTEM_CHKDSK, *PFILESYSTEM_CHKDSK;

FILESYSTEM_CHKDSK FileSystems[] =
{
    { L"FAT", VfatChkdsk },
    { L"FAT32", VfatChkdsk },
    { L"FATX" , VfatxChkdsk },
    { L"NTFS", NtfsChkdsk },
    { L"BTRFS", BtrfsChkdsk },
    { L"EXT2", Ext2Chkdsk },
    { L"EXT3", Ext2Chkdsk },
    { L"EXT4", Ext2Chkdsk },
    { L"CDFS", CdfsChkdsk },
};

HANDLE KeyboardHandle = NULL;
/* Set while PrintString shows a line from the file system checker. */
static BOOLEAN InCheckerOutput = FALSE;

/* bootex.log capture state; volumes are checked sequentially */
static PUCHAR LogBuffer;
static ULONG LogLength;
static ULONG LogCapacity;
static WCHAR LogVolumePath[128];
static BOOLEAN LogVolumeWritable;

/* FUNCTIONS ****************************************************************/

//
// FMIFS function
//

static INT
PrintString(char* fmt,...)
{
    INT Len;
    char buffer[512];
    va_list ap;
    UNICODE_STRING UnicodeString;
    ANSI_STRING AnsiString;

    va_start(ap, fmt);
    Len = vsprintf(buffer, fmt, ap);
    va_end(ap);

    /*
     * Mirror complete lines to the debug log, so a serial capture shows what
     * the check decided (dirty, skipped, in use, problems found). Progress and
     * countdown lines end in a bare carriage return and are not mirrored. The
     * mirror is capped so a large repair cannot slow the boot down.
     */
    if (strchr(buffer, '\n'))
    {
        static ULONG MirroredLines = 0;

        /* Only the checker's per-problem lines count against the cap; its
         * verdicts reach the log on their own (NTFSLIB:), and autochk's own
         * decisions are always mirrored. */
        if (!InCheckerOutput)
            DbgPrint("AUTOCHK: %s", buffer);
        else if (MirroredLines < AUTOCHK_MIRROR_LINES)
            DbgPrint("AUTOCHK: %s", buffer);
        else if (MirroredLines == AUTOCHK_MIRROR_LINES)
            DbgPrint("AUTOCHK: (further checker output not mirrored)\n");
        if (InCheckerOutput && MirroredLines <= AUTOCHK_MIRROR_LINES)
            MirroredLines++;
    }

    RtlInitAnsiString(&AnsiString, buffer);
    RtlAnsiStringToUnicodeString(&UnicodeString,
                                 &AnsiString,
                                 TRUE);
    NtDisplayString(&UnicodeString);
    RtlFreeUnicodeString(&UnicodeString);

    return Len;
}

static VOID
LogClear(IN PCWSTR VolumePath)
{
    ULONG Length;

    LogLength = 0;
    LogVolumeWritable = TRUE;

    Length = (ULONG)wcslen(VolumePath);
    if (Length > RTL_NUMBER_OF(LogVolumePath) - 12)
        Length = RTL_NUMBER_OF(LogVolumePath) - 12;
    RtlCopyMemory(LogVolumePath, VolumePath, Length * sizeof(WCHAR));
    LogVolumePath[Length] = UNICODE_NULL;
}

static VOID
__cdecl
LogAppend(IN PCSTR fmt, ...)
{
    va_list ap;
    CHAR Line[512];
    INT Len;
    PUCHAR NewBuffer;
    ULONG NewCapacity;

    if (!LogBuffer)
    {
        LogCapacity = 8192;
        LogBuffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, LogCapacity);
        if (!LogBuffer)
            return;
    }

    /* Native image: no _vscprintf. Format into a bounded stack buffer and
     * truncate a single over-long line rather than measuring it first. */
    va_start(ap, fmt);
    Len = _vsnprintf(Line, sizeof(Line) - 1, fmt, ap);
    va_end(ap);
    if (Len < 0)
        Len = sizeof(Line) - 1;
    Line[Len] = ANSI_NULL;

    while (LogLength + (ULONG)Len + 1 > LogCapacity)
    {
        NewCapacity = LogCapacity * 2;
        NewBuffer = RtlReAllocateHeap(RtlGetProcessHeap(), 0, LogBuffer, NewCapacity);
        if (!NewBuffer)
            return;
        LogBuffer = NewBuffer;
        LogCapacity = NewCapacity;
    }

    RtlCopyMemory((PCHAR)LogBuffer + LogLength, Line, Len + 1);
    LogLength += (ULONG)Len;
}

static VOID
LogWriteBootEx(VOID)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING FileName;
    IO_STATUS_BLOCK IoStatusBlock;
    WCHAR FullPath[160];
    ULONG Length;
    HANDLE FileHandle;
    NTSTATUS Status;

    if (!LogBuffer || LogLength == 0)
        return;

    if (!LogVolumeWritable)
    {
        DPRINT1("AUTOCHK: %ws is read-only, bootex.log not written\n", LogVolumePath);
        return;
    }

    Length = (ULONG)wcslen(LogVolumePath);
    RtlCopyMemory(FullPath, LogVolumePath, Length * sizeof(WCHAR));
    RtlCopyMemory(FullPath + Length, L"\\bootex.log", sizeof(L"\\bootex.log"));
    RtlInitUnicodeString(&FileName, FullPath);
    InitializeObjectAttributes(&ObjectAttributes,
                               &FileName,
                               OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    Status = NtCreateFile(&FileHandle,
                          FILE_GENERIC_WRITE | SYNCHRONIZE,
                          &ObjectAttributes,
                          &IoStatusBlock,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          0,
                          FILE_OVERWRITE_IF,
                          FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH,
                          NULL,
                          0);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("AUTOCHK: bootex.log create on %ws failed (0x%08lx)\n", LogVolumePath, Status);
        return;
    }

    Status = NtWriteFile(FileHandle,
                         NULL,
                         NULL,
                         NULL,
                         &IoStatusBlock,
                         LogBuffer,
                         LogLength,
                         NULL,
                         NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("AUTOCHK: bootex.log write on %ws failed (0x%08lx)\n", LogVolumePath, Status);
        NtClose(FileHandle);
        return;
    }

    /*
     * FILE_WRITE_THROUGH covers the data only. The new file's record and
     * the root directory entry that names it are held back in the file
     * system's write-back cache, and a check that changed the volume ends
     * in a restart that does not wait for that cache (Lab03, b774d48: a
     * dirty boot ended in "was changed and dismounted" and left no
     * bootex.log on the volume). Flush the file (its data, size and
     * record), then the volume (every held-back metadata block, the
     * directory entry included, and the device cache), so the log is on
     * the disk before autochk returns, whatever follows.
     */
    Status = NtFlushBuffersFile(FileHandle, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
        DPRINT1("AUTOCHK: bootex.log flush on %ws failed (0x%08lx)\n", LogVolumePath, Status);
    NtClose(FileHandle);

    RtlInitUnicodeString(&FileName, LogVolumePath);
    InitializeObjectAttributes(&ObjectAttributes,
                               &FileName,
                               OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);
    Status = NtOpenFile(&FileHandle,
                        FILE_GENERIC_WRITE | SYNCHRONIZE,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("AUTOCHK: volume open of %ws for the bootex.log flush failed (0x%08lx)\n", LogVolumePath, Status);
        return;
    }
    Status = NtFlushBuffersFile(FileHandle, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
        DPRINT1("AUTOCHK: volume flush of %ws after bootex.log failed (0x%08lx)\n", LogVolumePath, Status);
    NtClose(FileHandle);
}

static VOID
EraseLine(
    IN INT LineLength)
{
    INT Len;
    UNICODE_STRING UnicodeString;
    WCHAR buffer[512];

    if (LineLength <= 0)
        return;

    /* Go to the beginning of the line */
    RtlInitUnicodeString(&UnicodeString, L"\r");
    NtDisplayString(&UnicodeString);

    /* Fill the buffer chunk with spaces */
    Len = min(LineLength, ARRAYSIZE(buffer));
    while (Len > 0) buffer[--Len] = L' ';

    RtlInitEmptyUnicodeString(&UnicodeString, buffer, sizeof(buffer));
    while (LineLength > 0)
    {
        /* Display the buffer */
        Len = min(LineLength, ARRAYSIZE(buffer));
        LineLength -= Len;
        UnicodeString.Length = Len * sizeof(WCHAR);
        NtDisplayString(&UnicodeString);
    }

    /* Go to the beginning of the line */
    RtlInitUnicodeString(&UnicodeString, L"\r");
    NtDisplayString(&UnicodeString);
}

static NTSTATUS
OpenKeyboard(
    OUT PHANDLE KeyboardHandle)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    UNICODE_STRING KeyboardName = RTL_CONSTANT_STRING(L"\\Device\\KeyboardClass0");

    /* Just open the class driver */
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyboardName,
                               0,
                               NULL,
                               NULL);
    return NtOpenFile(KeyboardHandle,
                      FILE_ALL_ACCESS,
                      &ObjectAttributes,
                      &IoStatusBlock,
                      FILE_OPEN,
                      0);
}

/*
 * Discard key presses queued before autochk asked anything. WaitForKeyboard
 * returns on the first queued key-down, so a key typed at the boot menu (or
 * sent by a test harness) would otherwise cancel the first check at once.
 */
static VOID
DrainKeyboard(
    IN HANDLE KeyboardHandle)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatusBlock;
    LARGE_INTEGER Offset;
    KEYBOARD_INPUT_DATA InputData;
    ULONG Discarded = 0;

    do
    {
        Offset.QuadPart = 0;
        Status = NtReadFile(KeyboardHandle,
                            NULL,
                            NULL,
                            NULL,
                            &IoStatusBlock,
                            &InputData,
                            sizeof(KEYBOARD_INPUT_DATA),
                            &Offset,
                            NULL);
        if (!NT_SUCCESS(Status))
            return;
        if (Status != STATUS_PENDING)
            Discarded++;
    } while (Status != STATUS_PENDING);

    /* Nothing is queued any more: drop the read that is now waiting */
    NtCancelIoFile(KeyboardHandle, &IoStatusBlock);
    if (Discarded)
        DPRINT1("AUTOCHK: discarded %lu queued key event(s)\n", Discarded);
}

static NTSTATUS
WaitForKeyboard(
    IN HANDLE KeyboardHandle,
    IN LONG TimeOut)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatusBlock;
    LARGE_INTEGER Offset, Timeout;
    KEYBOARD_INPUT_DATA InputData;
    INT Len = 0;

    /* Skip the wait if there is no timeout */
    if (TimeOut <= 0)
        return STATUS_TIMEOUT;

    /* Attempt to read a down key-press from the keyboard */
    do
    {
        Offset.QuadPart = 0;
        Status = NtReadFile(KeyboardHandle,
                            NULL,
                            NULL,
                            NULL,
                            &IoStatusBlock,
                            &InputData,
                            sizeof(KEYBOARD_INPUT_DATA),
                            &Offset,
                            NULL);
        if (!NT_SUCCESS(Status))
        {
            /* Something failed, bail out */
            return Status;
        }
        if ((Status != STATUS_PENDING) && !(InputData.Flags & KEY_BREAK))
        {
            /* We have a down key-press, return */
            return Status;
        }
    } while (Status != STATUS_PENDING);

    /* Perform the countdown of TimeOut seconds */
    Status = STATUS_TIMEOUT;
    while (TimeOut > 0)
    {
        /*
         * Display the countdown string.
         * Note that we only do a carriage return to go back to the
         * beginning of the line to possibly erase it. Note also that
         * we display a trailing space to erase any last character
         * when the string length decreases.
         */
        Len = PrintString("Press any key within %d second(s) to cancel and resume startup. \r", TimeOut);

        /* Decrease the countdown */
        --TimeOut;

        /* Wait one second for a key press */
        Timeout.QuadPart = -10000000;
        Status = NtWaitForSingleObject(KeyboardHandle, FALSE, &Timeout);
        if (Status != STATUS_TIMEOUT)
            break;
    }

    /* Erase the countdown string */
    EraseLine(Len);

    if (Status == STATUS_TIMEOUT)
    {
        /* The user did not press any key, cancel the read */
        NtCancelIoFile(KeyboardHandle, &IoStatusBlock);
    }
    else
    {
        /* Otherwise, return some status */
        Status = IoStatusBlock.Status;
    }

    return Status;
}

static NTSTATUS
GetFileSystem(
    IN PUNICODE_STRING VolumePathU,
    IN OUT PWSTR FileSystemName,
    IN SIZE_T FileSystemNameSize,
    OUT PULONG FileSystemAttributes)
{
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE FileHandle;
    IO_STATUS_BLOCK IoStatusBlock;
    PFILE_FS_ATTRIBUTE_INFORMATION FileFsAttribute;
    UCHAR Buffer[sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + MAX_PATH * sizeof(WCHAR)];

    FileFsAttribute = (PFILE_FS_ATTRIBUTE_INFORMATION)Buffer;

    InitializeObjectAttributes(&ObjectAttributes,
                               VolumePathU,
                               OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    Status = NtCreateFile(&FileHandle,
                          FILE_GENERIC_READ,
                          &ObjectAttributes,
                          &IoStatusBlock,
                          NULL,
                          0,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          FILE_OPEN,
                          0,
                          NULL,
                          0);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Could not open volume '%wZ' to obtain its file system, Status 0x%08lx\n",
                VolumePathU, Status);
        return Status;
    }

    Status = NtQueryVolumeInformationFile(FileHandle,
                                          &IoStatusBlock,
                                          FileFsAttribute,
                                          sizeof(Buffer),
                                          FileFsAttributeInformation);
    NtClose(FileHandle);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NtQueryVolumeInformationFile() failed, Status 0x%08lx\n", Status);
        return Status;
    }

    *FileSystemAttributes = FileFsAttribute->FileSystemAttributes;

    if (FileSystemNameSize >= FileFsAttribute->FileSystemNameLength + sizeof(WCHAR))
    {
        RtlCopyMemory(FileSystemName,
                      FileFsAttribute->FileSystemName,
                      FileFsAttribute->FileSystemNameLength);
        FileSystemName[FileFsAttribute->FileSystemNameLength / sizeof(WCHAR)] = UNICODE_NULL;
    }
    else
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    return STATUS_SUCCESS;
}

/* This is based on SysInternals' ChkDsk application */
static BOOLEAN
NTAPI
ChkdskCallback(
    IN CALLBACKCOMMAND Command,
    IN ULONG Modifier,
    IN PVOID Argument)
{
    PDWORD      Percent;
    PBOOLEAN    Status;
    PTEXTOUTPUT Output;

    //
    // We get other types of commands,
    // but we don't have to pay attention to them
    //
    switch(Command)
    {
    case UNKNOWN2:
        DPRINT("UNKNOWN2\n");
        break;

    case UNKNOWN3:
        DPRINT("UNKNOWN3\n");
        break;

    case UNKNOWN4:
        DPRINT("UNKNOWN4\n");
        break;

    case UNKNOWN5:
        DPRINT("UNKNOWN5\n");
        break;

    case UNKNOWN9:
        DPRINT("UNKNOWN9\n");
        break;

    case UNKNOWNA:
        DPRINT("UNKNOWNA\n");
        break;

    case UNKNOWNC:
        DPRINT("UNKNOWNC\n");
        break;

    case UNKNOWND:
        DPRINT("UNKNOWND\n");
        break;

    case INSUFFICIENTRIGHTS:
        DPRINT("INSUFFICIENTRIGHTS\n");
        break;

    case FSNOTSUPPORTED:
        DPRINT("FSNOTSUPPORTED\n");
        break;

    case VOLUMEINUSE:
        DPRINT("VOLUMEINUSE\n");
        break;

    case STRUCTUREPROGRESS:
        DPRINT("STRUCTUREPROGRESS\n");
        break;

    case DONEWITHSTRUCTURE:
        DPRINT("DONEWITHSTRUCTURE\n");
        break;

    case CLUSTERSIZETOOSMALL:
        DPRINT("CLUSTERSIZETOOSMALL\n");
        break;

    case PROGRESS:
        Percent = (PDWORD) Argument;
        PrintString("%d percent completed.\r", *Percent);
        break;

    case OUTPUT:
        Output = (PTEXTOUTPUT) Argument;
        InCheckerOutput = TRUE;
        PrintString("%s", Output->Output);
        LogAppend("%s", Output->Output);
        InCheckerOutput = FALSE;
        break;

    case DONE:
        Status = (PBOOLEAN)Argument;
        if (*Status == FALSE)
        {
            PrintString("The file system check was unable to complete successfully.\r\n\r\n");
            LogAppend("The file system check was unable to complete successfully.\r\n");
            // Error = TRUE;
        }
        break;
    }
    return TRUE;
}

/*
 * Set when a volume was repaired or marked clean while it was mounted, and
 * then dismounted. Programs started before the check (the Session Manager,
 * autochk itself) still have that volume's files open and mapped, and the
 * file system keeps serving those opens with what it read before the
 * repair. Windows restarts after a boot-time repair of the system volume;
 * do the same once all volumes are checked.
 */
static BOOLEAN RestartAfterCheck = FALSE;

static NTSTATUS
CheckVolume(
    IN PCWSTR VolumePath,
    IN LONG TimeOut,
    IN BOOLEAN CheckOnlyIfDirty)
{
    NTSTATUS Status;
    BOOLEAN Success;
    BOOLEAN FixAttempted = FALSE;
    PCWSTR DisplayName;
    ULONG FileSystemAttributes;
    UNICODE_STRING VolumePathU;
    ULONG Count;
    WCHAR FileSystem[128];

    RtlInitUnicodeString(&VolumePathU, VolumePath);

    /* Get a drive string for display purposes only */
    if (wcslen(VolumePath) == 6 &&
        VolumePath[0] == L'\\'  &&
        VolumePath[1] == L'?'   &&
        VolumePath[2] == L'?'   &&
        VolumePath[3] == L'\\'  &&
        VolumePath[5] == L':')
    {
        /* DOS drive */
        DisplayName = &VolumePath[4];
    }
    else
    {
        DisplayName = VolumePath;
    }

    DPRINT1("AUTOCHK: Checking %wZ\n", &VolumePathU);

    LogClear(VolumePath);
    LogAppend("Checking file system on %S\r\n", DisplayName);

    /* Get the file system */
    FileSystemAttributes = 0;
    Status = GetFileSystem(&VolumePathU,
                           FileSystem,
                           sizeof(FileSystem),
                           &FileSystemAttributes);
    LogVolumeWritable = !(FileSystemAttributes & FILE_READ_ONLY_VOLUME);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GetFileSystem() failed, Status 0x%08lx\n", Status);
        PrintString("  Unable to detect the file system of volume %S\r\n", DisplayName);
        goto Quit;
    }

    /* Find a suitable file system provider */
    for (Count = 0; Count < RTL_NUMBER_OF(FileSystems); ++Count)
    {
        if (wcscmp(FileSystem, FileSystems[Count].Name) == 0)
            break;
    }
    if (Count >= RTL_NUMBER_OF(FileSystems))
    {
        DPRINT1("File system %S not supported\n", FileSystem);
        PrintString("  Unable to verify the volume. The %S file system is not supported.\r\n", FileSystem);
        Status = STATUS_DLL_NOT_FOUND;
        goto Quit;
    }

    /* Check whether the volume is dirty */
    Status = STATUS_SUCCESS;
    Success = FileSystems[Count].ChkdskFunc(&VolumePathU,
                                            ChkdskCallback,
                                            FALSE, // FixErrors
                                            TRUE,  // Verbose
                                            TRUE,  // CheckOnlyIfDirty
                                            FALSE, // ScanDrive
                                            NULL,
                                            NULL,
                                            NULL,
                                            NULL,
                                            (PULONG)&Status);

    /* Perform the check either when the volume is dirty or a check is forced */
    if (Success && ((Status == STATUS_DISK_CORRUPT_ERROR) || !CheckOnlyIfDirty))
    {
        /* Let the user decide whether to repair */
        if (Status == STATUS_DISK_CORRUPT_ERROR)
        {
            PrintString("The file system on volume %S needs to be checked for problems.\r\n", DisplayName);
            PrintString("You may cancel this check, but it is recommended that you continue.\r\n");
            LogAppend("The file system on volume %S needs to be checked for problems.\r\n", DisplayName);
            LogAppend("You may cancel this check, but it is recommended that you continue.\r\n");
        }
        else
        {
            PrintString("A volume check has been scheduled.\r\n");
            LogAppend("A volume check has been scheduled.\r\n");
        }

        if (!KeyboardHandle || WaitForKeyboard(KeyboardHandle, TimeOut) == STATUS_TIMEOUT)
        {
            PrintString("The system will now check the file system.\r\n\r\n");
            LogAppend("The system will now check the file system.\r\n\r\n");
            FixAttempted = TRUE;
            Status = STATUS_SUCCESS;
            Success = FileSystems[Count].ChkdskFunc(&VolumePathU,
                                                    ChkdskCallback,
                                                    TRUE,  // FixErrors
                                                    TRUE,  // Verbose
                                                    CheckOnlyIfDirty,
                                                    FALSE, // ScanDrive
                                                    NULL,
                                                    NULL,
                                                    NULL,
                                                    NULL,
                                                    (PULONG)&Status);
            if (Success && (ULONG)Status == NTFSLIB_CHKDSK_VOLUME_RELOADED)
            {
                RestartAfterCheck = TRUE;
                DPRINT1("AUTOCHK: %wZ was changed and dismounted, restart needed\n", &VolumePathU);
            }
        }
        else
        {
            PrintString("The file system check has been skipped.\r\n");
        }
    }

Quit:
    PrintString("\r\n\r\n");
    if (FixAttempted)
        LogWriteBootEx();
    return Status;
}

static VOID
QueryTimeout(
    IN OUT PLONG TimeOut)
{
    RTL_QUERY_REGISTRY_TABLE QueryTable[2];

    RtlZeroMemory(QueryTable, sizeof(QueryTable));
    QueryTable[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    QueryTable[0].Name = L"AutoChkTimeOut";
    QueryTable[0].EntryContext = TimeOut;

    RtlQueryRegistryValues(RTL_REGISTRY_CONTROL, L"Session Manager", QueryTable, NULL, NULL);
    /* See: https://docs.microsoft.com/en-us/windows-server/administration/windows-commands/autochk */
    *TimeOut = min(max(*TimeOut, 0), 259200);
}

INT
__cdecl
_main(
    IN INT argc,
    IN PCHAR argv[],
    IN PCHAR envp[],
    IN ULONG DebugFlag)
{
    NTSTATUS Status;
    LONG TimeOut;
    ULONG i;
    BOOLEAN CheckAllVolumes = FALSE;
    BOOLEAN CheckOnlyIfDirty = TRUE;
    PROCESS_DEVICEMAP_INFORMATION DeviceMap;
    PCHAR SkipDrives = NULL;
    ANSI_STRING VolumePathA;
    UNICODE_STRING VolumePathU;
    WCHAR VolumePath[128] = L"";

    /*
     * Parse the command-line: optional command switches,
     * then the NT volume name (or drive letter) to be analysed.
     * If "*" is passed this means that we check all the volumes.
     */
    if (argc <= 1)
    {
        /* Only one parameter (the program name), bail out */
        return 1;
    }
    for (i = 1; i < argc; ++i)
    {
        if (argv[i][0] == '/' || argv[i][0] == '-')
        {
            DPRINT("Parameter %d: %s\n", i, argv[i]);
            switch (toupper(argv[i][1]))
            {
                case 'K': /* List of drive letters to skip */
                {
                    /* Check for the separator */
                    if (argv[i][2] != ':')
                        goto Default;

                    SkipDrives = &argv[i][3];
                    break;
                }

                case 'R': /* Repair the errors, implies /P */
                case 'P': /* Check even if not dirty */
                {
                    if (argv[i][2] != ANSI_NULL)
                        goto Default;

                    CheckOnlyIfDirty = FALSE;
                    break;
                }

                default: Default:
                    DPRINT1("Unknown switch %d: %s\n", i, argv[i]);
                    break;
            }
        }
        else
        {
            DPRINT("Parameter %d - Volume specification: %s\n", i, argv[i]);
            if (strcmp(argv[i], "*") == 0)
            {
                CheckAllVolumes = TRUE;
            }
            else
            {
                RtlInitEmptyUnicodeString(&VolumePathU,
                                          VolumePath,
                                          sizeof(VolumePath));
                RtlInitAnsiString(&VolumePathA, argv[i]);
                Status = RtlAnsiStringToUnicodeString(&VolumePathU,
                                                      &VolumePathA,
                                                      FALSE);
            }

            /* Stop the parsing now */
            break;
        }
    }

    /*
     * FIXME: We should probably use here the mount manager to be
     * able to check volumes which don't have a drive letter.
     */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessDeviceMap,
                                       &DeviceMap.Query,
                                       sizeof(DeviceMap.Query),
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NtQueryInformationProcess() failed, Status 0x%08lx\n", Status);
        return 1;
    }

    /* Filter out the skipped drives from the map */
    if (SkipDrives && *SkipDrives)
    {
        DPRINT1("Skipping drives:");
        while (*SkipDrives)
        {
#if DBG
            DbgPrint(" %c:", *SkipDrives);
#endif
            /* Retrieve the index and filter the drive out */
            i = toupper(*SkipDrives) - 'A';
            if (0 <= i && i <= 'Z'-'A')
                DeviceMap.Query.DriveMap &= ~(1 << i);

            /* Go to the next drive letter */
            ++SkipDrives;
        }
        DbgPrint("\n");
    }

    /* Query the timeout */
    TimeOut = 3;
    QueryTimeout(&TimeOut);

    /* Open the keyboard */
    Status = OpenKeyboard(&KeyboardHandle);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("OpenKeyboard() failed, Status 0x%08lx\n", Status);
        /* Ignore keyboard interaction */
    }
    else
    {
        DrainKeyboard(KeyboardHandle);
    }

    if (CheckAllVolumes)
    {
        /* Enumerate and check each of the available volumes */
        for (i = 0; i <= 'Z'-'A'; i++)
        {
            if ((DeviceMap.Query.DriveMap & (1 << i)) &&
                (DeviceMap.Query.DriveType[i] == DOSDEVICE_DRIVE_FIXED))
            {
                _swprintf(VolumePath, L"\\??\\%c:", L'A' + i);
                CheckVolume(VolumePath, TimeOut, CheckOnlyIfDirty);
            }
        }
    }
    else
    {
        /* Retrieve our index and analyse the volume */
        if (wcslen(VolumePath) == 6 &&
            VolumePath[0] == L'\\'  &&
            VolumePath[1] == L'?'   &&
            VolumePath[2] == L'?'   &&
            VolumePath[3] == L'\\'  &&
            VolumePath[5] == L':')
        {
            i = toupper(VolumePath[4]) - 'A';
            if ((DeviceMap.Query.DriveMap & (1 << i)) &&
                (DeviceMap.Query.DriveType[i] == DOSDEVICE_DRIVE_FIXED))
            {
                CheckVolume(VolumePath, TimeOut, CheckOnlyIfDirty);
            }
        }
        else
        {
            /* Just perform the check on the specified volume */
            // TODO: Check volume type using NtQueryVolumeInformationFile(FileFsDeviceInformation)
            CheckVolume(VolumePath, TimeOut, CheckOnlyIfDirty);
        }
    }

    /* Close the keyboard */
    if (KeyboardHandle)
        NtClose(KeyboardHandle);

    if (RestartAfterCheck)
    {
        BOOLEAN WasEnabled;

        PrintString("A volume was repaired while it was in use. The system will now restart.\r\n");
        DPRINT1("AUTOCHK: restarting after the check\n");
        Status = RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, TRUE, FALSE, &WasEnabled);
        if (NT_SUCCESS(Status))
            Status = NtShutdownSystem(ShutdownReboot);
        DPRINT1("AUTOCHK: restart failed, Status 0x%08lx\n", Status);
        PrintString("The system could not be restarted. Restart it before using it.\r\n");
    }

    // PrintString("Done\r\n\r\n");
    return 0;
}

/* EOF */
