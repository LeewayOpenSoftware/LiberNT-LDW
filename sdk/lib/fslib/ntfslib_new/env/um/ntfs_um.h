#ifndef _NTFS_UM_H_
#define _NTFS_UM_H_

#ifdef __cplusplus
extern "C" {
#endif

NTSTATUS
NtfsDiskInitializeUm(
    _In_      HANDLE FileHandle,
    _Out_opt_ ULONG* BytesPerSector);

/* Successful volume writes since the last NtfsDiskInitializeUm. */
ULONG
NtfsDiskWriteCountUm(VOID);

#ifdef __cplusplus
}
#endif

#endif /* _NTFS_UM_H_ */
