
#ifndef _SCHANNEL_PCH_
#define _SCHANNEL_PCH_

#include <stdarg.h>

#define _INC_WINDOWS
#define COM_NO_WINDOWS_H

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include <winreg.h>
#include <sspi.h>
#include <ntsecapi.h>
#include <ntsecpkg.h>
#include <schannel.h>

#include <wine/debug.h>
#include <wine/list.h>
#include <wine/unicode.h>

/* adapters.c */
NTSTATUS NTAPI SchannelSpInitialize(ULONG_PTR, PSECPKG_PARAMETERS, PLSA_SECPKG_FUNCTION_TABLE);
NTSTATUS NTAPI SchannelSpShutdown(VOID);
NTSTATUS WINAPI SchannelSpAcquireCredentialsHandle(PUNICODE_STRING, ULONG, PLUID, PVOID, PVOID, PVOID, PLSA_SEC_HANDLE, PTimeStamp);
NTSTATUS NTAPI SchannelSpQueryCredentialsAttributes(LSA_SEC_HANDLE, ULONG, PVOID);
NTSTATUS NTAPI SchannelSpFreeCredentialsHandle(LSA_SEC_HANDLE);
NTSTATUS NTAPI SchannelSpInitLsaModeContext(LSA_SEC_HANDLE, LSA_SEC_HANDLE, PUNICODE_STRING, ULONG, ULONG, PSecBufferDesc, PLSA_SEC_HANDLE, PSecBufferDesc, PULONG, PTimeStamp, PBOOLEAN, PSecBuffer);
NTSTATUS NTAPI SchannelSpAcceptLsaModeContext(LSA_SEC_HANDLE, LSA_SEC_HANDLE, PSecBufferDesc, ULONG, ULONG, PLSA_SEC_HANDLE, PSecBufferDesc, PULONG, PTimeStamp, PBOOLEAN, PSecBuffer);
NTSTATUS NTAPI SchannelSpDeleteContext(LSA_SEC_HANDLE);
NTSTATUS NTAPI SchannelSpApplyControlToken(LSA_SEC_HANDLE, PSecBufferDesc);
NTSTATUS NTAPI SchannelSpQueryContextAttributes(LSA_SEC_HANDLE, ULONG, PVOID);
NTSTATUS NTAPI SchannelSpAddCredentials(LSA_SEC_HANDLE, PUNICODE_STRING, PUNICODE_STRING, ULONG, PVOID, PVOID, PVOID, PTimeStamp);
NTSTATUS NTAPI SchannelSpSetContextAttributes(LSA_SEC_HANDLE, ULONG, PVOID, ULONG);
NTSTATUS NTAPI SchannelSpSetCredentialsAttributes(LSA_SEC_HANDLE, ULONG, PVOID, ULONG);
NTSTATUS NTAPI SchannelSpInstanceInit(ULONG, PSECPKG_DLL_FUNCTIONS, PVOID *);
NTSTATUS NTAPI SchannelUsrMakeSignature(LSA_SEC_HANDLE, ULONG, PSecBufferDesc, ULONG);
NTSTATUS NTAPI SchannelUsrVerifySignature(LSA_SEC_HANDLE, PSecBufferDesc, ULONG, PULONG);
NTSTATUS NTAPI SchannelUsrSealMessage(LSA_SEC_HANDLE, ULONG, PSecBufferDesc, ULONG);
NTSTATUS NTAPI SchannelUsrUnsealMessage(LSA_SEC_HANDLE, PSecBufferDesc, ULONG, PULONG);
NTSTATUS NTAPI SchannelUsrGetContextToken(LSA_SEC_HANDLE, PHANDLE);
NTSTATUS NTAPI SchannelUsrCompleteAuthToken(LSA_SEC_HANDLE, PSecBufferDesc);
NTSTATUS NTAPI SchannelUsrDeleteUserModeContext(LSA_SEC_HANDLE);
NTSTATUS NTAPI SchannelUsrExportContext(LSA_SEC_HANDLE, ULONG, PSecBuffer, PHANDLE);
NTSTATUS NTAPI SchannelUsrImportContext(PSecBuffer, HANDLE, PLSA_SEC_HANDLE);

#endif /* !_SCHANNEL_PCH_ */
