/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NTFS filesystem library
 * FILE:        include/reactos/libs/fslib/ntfslib.h
 * PURPOSE:     Public definitions for NTFS filesystem library
 */

#ifndef __NTFSLIB_H
#define __NTFSLIB_H

#include <fmifs/fmifs.h>

/*
 * ExitStatus of NtfsChkdsk when the volume was checked locked, has no
 * problems left, and was dismounted afterwards. A success code: the check
 * passed. It tells a boot-time caller that file objects opened on the
 * volume before the check (images, current directories) still describe the
 * volume as it was before the repair, so the system must be restarted
 * before anything else uses it.
 */
#define NTFSLIB_CHKDSK_VOLUME_RELOADED ((ULONG)0x60000001L)

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
    IN PULONG ExitStatus);

BOOLEAN
NTAPI
NtfsFormat(
    IN PUNICODE_STRING DriveRoot,
    IN PFMIFSCALLBACK Callback,
    IN BOOLEAN QuickFormat,
    IN BOOLEAN BackwardCompatible,
    IN MEDIA_TYPE MediaType,
    IN PUNICODE_STRING Label,
    IN ULONG ClusterSize);

#endif /* __NTFSLIB_H */
