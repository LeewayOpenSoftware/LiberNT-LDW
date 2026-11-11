/*
 * LSA-mode and user-mode dispatch adapters of the SChannel security provider
 *
 * Copyright 2007 Yuval Fledel
 * Copyright 2026 Leeway Open Software
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "sspi.h"
#include "ntsecapi.h"
#include "ntsecpkg.h"
#include "schannel.h"

/*
 * LSA-side credential and context handles name engine objects indirectly.
 * secur32's handles pair a provider object with a provider-local handle, so
 * both words are kept per LSA handle.
 */
#define SCHANNEL_MAP_SIZE 128

typedef struct _SCHANNEL_MAP_ENTRY
{
    ULONG_PTR LsaHandle;
    ULONG_PTR EngineLower;
    ULONG_PTR EngineUpper;
    UCHAR Type;
    UCHAR InUse;
} SCHANNEL_MAP_ENTRY;

static SCHANNEL_MAP_ENTRY schannelMap[SCHANNEL_MAP_SIZE];
static CRITICAL_SECTION schannelMapLock;
static CRITICAL_SECTION_DEBUG schannelMapLockDebug =
{
    0, 0, &schannelMapLock,
    { &schannelMapLockDebug.ProcessLocksList,
      &schannelMapLockDebug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": schannelMapLock") }
};
static CRITICAL_SECTION schannelMapLock = { &schannelMapLockDebug, -1, 0, 0, 0, 0 };
static ULONG_PTR schannelNextHandle = 1;

enum
{
    SCHANNEL_HANDLE_CRED = 1,
    SCHANNEL_HANDLE_CTX = 2
};

static
PVOID
schannelLookupHandle(
    _In_ ULONG_PTR LsaHandle,
    _In_ UCHAR ExpectedType,
    _Out_opt_ PULONG_PTR EngineUpper)
{
    ULONG i;
    SCHANNEL_MAP_ENTRY *entry = NULL;

    EnterCriticalSection(&schannelMapLock);
    for (i = 0; i < SCHANNEL_MAP_SIZE; i++)
    {
        if (schannelMap[i].InUse && schannelMap[i].LsaHandle == LsaHandle)
        {
            if (schannelMap[i].Type == ExpectedType)
            {
                entry = &schannelMap[i];
                if (EngineUpper)
                    *EngineUpper = entry->EngineUpper;
            }
            break;
        }
    }
    LeaveCriticalSection(&schannelMapLock);
    return entry ? (PVOID)entry->EngineLower : NULL;
}

static
ULONG_PTR
schannelAddHandle(
    _In_ ULONG_PTR EngineLower,
    _In_ ULONG_PTR EngineUpper,
    _In_ UCHAR Type)
{
    ULONG i;
    ULONG_PTR lsaHandle = 0;

    EnterCriticalSection(&schannelMapLock);
    for (i = 0; i < SCHANNEL_MAP_SIZE; i++)
    {
        if (!schannelMap[i].InUse)
        {
            schannelMap[i].InUse = TRUE;
            schannelMap[i].Type = Type;
            schannelMap[i].EngineLower = EngineLower;
            schannelMap[i].EngineUpper = EngineUpper;
            schannelMap[i].LsaHandle = schannelNextHandle++;
            if (schannelNextHandle == 0)
                schannelNextHandle = 1;
            lsaHandle = schannelMap[i].LsaHandle;
            break;
        }
    }
    LeaveCriticalSection(&schannelMapLock);
    if (!lsaHandle)
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return lsaHandle;
}

static
BOOL
schannelRemoveHandle(
    _In_ ULONG_PTR LsaHandle,
    _In_ UCHAR ExpectedType,
    _Out_ PULONG_PTR EngineLower,
    _Out_ PULONG_PTR EngineUpper)
{
    ULONG i;
    BOOL found = FALSE;

    EnterCriticalSection(&schannelMapLock);
    for (i = 0; i < SCHANNEL_MAP_SIZE; i++)
    {
        if (schannelMap[i].InUse && schannelMap[i].LsaHandle == LsaHandle)
        {
            if (schannelMap[i].Type == ExpectedType)
            {
                *EngineLower = schannelMap[i].EngineLower;
                *EngineUpper = schannelMap[i].EngineUpper;
                schannelMap[i].InUse = FALSE;
                found = TRUE;
            }
            break;
        }
    }
    LeaveCriticalSection(&schannelMapLock);
    return found;
}

/***********************************************************************
 * LSA-mode table adapters
 */
NTSTATUS
NTAPI
SchannelSpInitialize(
    _In_ ULONG_PTR Version,
    _In_ PSECPKG_PARAMETERS Parameters,
    _In_ PLSA_SECPKG_FUNCTION_TABLE FunctionTable)
{
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
SchannelSpShutdown(VOID)
{
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
SchannelSpAcquireCredentialsHandle(
    _In_opt_ PUNICODE_STRING PrincipalName,
    _In_ ULONG CredentialUseFlags,
    _In_opt_ PLUID LogonId,
    _In_opt_ PVOID AuthData,
    _In_opt_ PVOID GetKeyFn,
    _In_opt_ PVOID GetKeyArg,
    _Out_ PLSA_SEC_HANDLE CredentialHandle,
    _Out_opt_ PTimeStamp Expiry)
{
    CredHandle credentials;
    SECURITY_STATUS status;

    status = AcquireCredentialsHandleW(
        PrincipalName ? PrincipalName->Buffer : NULL,
        SCHANNEL_NAME_W,
        CredentialUseFlags,
        LogonId,
        AuthData,
        (SEC_GET_KEY_FN)GetKeyFn,
        GetKeyArg,
        &credentials,
        Expiry);
    if (status == SEC_E_OK)
    {
        *CredentialHandle = schannelAddHandle(credentials.dwLower,
                                              credentials.dwUpper,
                                              SCHANNEL_HANDLE_CRED);
        if (!*CredentialHandle)
            FreeCredentialsHandle(&credentials);
    }
    return (NTSTATUS)status;
}

NTSTATUS
NTAPI
SchannelSpQueryCredentialsAttributes(
    _In_ LSA_SEC_HANDLE CredentialHandle,
    _In_ ULONG CredentialAttribute,
    _Inout_ PVOID Buffer)
{
    PCredHandle credentials = schannelLookupHandle(CredentialHandle,
                                                   SCHANNEL_HANDLE_CRED,
                                                   NULL);

    if (!credentials)
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    return (NTSTATUS)QueryCredentialsAttributesW(credentials,
                                                 CredentialAttribute,
                                                 Buffer);
}

NTSTATUS
NTAPI
SchannelSpFreeCredentialsHandle(
    _In_ LSA_SEC_HANDLE CredentialHandle)
{
    ULONG_PTR lower, upper;
    CredHandle credentials;

    if (!schannelRemoveHandle(CredentialHandle, SCHANNEL_HANDLE_CRED,
                              &lower, &upper))
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    credentials.dwLower = lower;
    credentials.dwUpper = upper;
    return (NTSTATUS)FreeCredentialsHandle(&credentials);
}

NTSTATUS
NTAPI
SchannelSpInitLsaModeContext(
    _In_ LSA_SEC_HANDLE CredentialHandle,
    _In_opt_ LSA_SEC_HANDLE ContextHandle,
    _In_opt_ PUNICODE_STRING TargetName,
    _In_ ULONG ContextReqFlags,
    _In_ ULONG TargetDataFlags,
    _In_opt_ PSecBufferDesc InputBuffer,
    _Out_ PLSA_SEC_HANDLE NewContextHandle,
    _Inout_opt_ PSecBufferDesc OutputBuffer,
    _Out_ PULONG ContextAttributes,
    _Out_opt_ PTimeStamp ExpirationTime,
    _Out_ PBOOLEAN MappedContext,
    _Out_ PSecBuffer ContextData)
{
    PCredHandle credentials = schannelLookupHandle(CredentialHandle,
                                                   SCHANNEL_HANDLE_CRED,
                                                   NULL);
    PCtxtHandle inputContext = NULL;
    CtxtHandle inputContextStorage;
    ULONG_PTR lower, upper;
    CtxtHandle newContext;
    SECURITY_STATUS status;

    if (!credentials)
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    if (ContextHandle)
    {
        if (!schannelRemoveHandle(ContextHandle, SCHANNEL_HANDLE_CTX,
                                  &lower, &upper))
            return (NTSTATUS)SEC_E_INVALID_HANDLE;
        inputContextStorage.dwLower = lower;
        inputContextStorage.dwUpper = upper;
        inputContext = &inputContextStorage;
    }
    status = InitializeSecurityContextW(
        credentials,
        inputContext,
        TargetName ? TargetName->Buffer : NULL,
        ContextReqFlags,
        0,
        TargetDataFlags,
        InputBuffer,
        0,
        &newContext,
        OutputBuffer,
        ContextAttributes,
        ExpirationTime);
    if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED ||
        status == SEC_I_COMPLETE_NEEDED || status == SEC_I_COMPLETE_AND_CONTINUE)
    {
        *NewContextHandle = schannelAddHandle(newContext.dwLower,
                                              newContext.dwUpper,
                                              SCHANNEL_HANDLE_CTX);
        if (!*NewContextHandle)
        {
            if (inputContext)
                DeleteSecurityContext(inputContext);
            DeleteSecurityContext(&newContext);
            status = SEC_E_INTERNAL_ERROR;
        }
    }
    else if (inputContext)
    {
        /* the engine kept the input context; re-register it */
        *NewContextHandle = schannelAddHandle(inputContext->dwLower,
                                              inputContext->dwUpper,
                                              SCHANNEL_HANDLE_CTX);
        if (!*NewContextHandle)
            DeleteSecurityContext(inputContext);
    }
    *MappedContext = TRUE;
    if (ContextData)
    {
        ContextData->cbBuffer = 0;
        ContextData->BufferType = SECBUFFER_EMPTY;
        ContextData->pvBuffer = NULL;
    }
    return (NTSTATUS)status;
}

NTSTATUS
NTAPI
SchannelSpAcceptLsaModeContext(
    _In_ LSA_SEC_HANDLE CredentialHandle,
    _In_opt_ LSA_SEC_HANDLE ContextHandle,
    _In_opt_ PSecBufferDesc InputBuffer,
    _In_ ULONG ContextReqFlags,
    _In_ ULONG TargetDataRep,
    _Out_ PLSA_SEC_HANDLE NewContextHandle,
    _Inout_opt_ PSecBufferDesc OutputBuffer,
    _Out_ PULONG ContextAttributes,
    _Out_opt_ PTimeStamp ExpirationTime,
    _Out_ PBOOLEAN MappedContext,
    _Out_ PSecBuffer ContextData)
{
    PCredHandle credentials = schannelLookupHandle(CredentialHandle,
                                                   SCHANNEL_HANDLE_CRED,
                                                   NULL);
    PCtxtHandle inputContext = NULL;
    CtxtHandle inputContextStorage;
    ULONG_PTR lower, upper;
    CtxtHandle newContext;
    SECURITY_STATUS status;

    if (!credentials)
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    if (ContextHandle)
    {
        if (!schannelRemoveHandle(ContextHandle, SCHANNEL_HANDLE_CTX,
                                  &lower, &upper))
            return (NTSTATUS)SEC_E_INVALID_HANDLE;
        inputContextStorage.dwLower = lower;
        inputContextStorage.dwUpper = upper;
        inputContext = &inputContextStorage;
    }
    status = AcceptSecurityContext(
        credentials,
        inputContext,
        InputBuffer,
        ContextReqFlags,
        TargetDataRep,
        &newContext,
        OutputBuffer,
        ContextAttributes,
        ExpirationTime);
    if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED ||
        status == SEC_I_COMPLETE_NEEDED || status == SEC_I_COMPLETE_AND_CONTINUE)
    {
        *NewContextHandle = schannelAddHandle(newContext.dwLower,
                                              newContext.dwUpper,
                                              SCHANNEL_HANDLE_CTX);
        if (!*NewContextHandle)
        {
            if (inputContext)
                DeleteSecurityContext(inputContext);
            DeleteSecurityContext(&newContext);
            status = SEC_E_INTERNAL_ERROR;
        }
    }
    else if (inputContext)
    {
        *NewContextHandle = schannelAddHandle(inputContext->dwLower,
                                              inputContext->dwUpper,
                                              SCHANNEL_HANDLE_CTX);
        if (!*NewContextHandle)
            DeleteSecurityContext(inputContext);
    }
    *MappedContext = TRUE;
    if (ContextData)
    {
        ContextData->cbBuffer = 0;
        ContextData->BufferType = SECBUFFER_EMPTY;
        ContextData->pvBuffer = NULL;
    }
    return (NTSTATUS)status;
}

NTSTATUS
NTAPI
SchannelSpDeleteContext(
    _In_ LSA_SEC_HANDLE ContextHandle)
{
    ULONG_PTR lower, upper;
    CtxtHandle context;

    if (!schannelRemoveHandle(ContextHandle, SCHANNEL_HANDLE_CTX,
                              &lower, &upper))
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    context.dwLower = lower;
    context.dwUpper = upper;
    return (NTSTATUS)DeleteSecurityContext(&context);
}

NTSTATUS
NTAPI
SchannelSpApplyControlToken(
    _In_ LSA_SEC_HANDLE ContextHandle,
    _In_ PSecBufferDesc ControlToken)
{
    PCtxtHandle context = schannelLookupHandle(ContextHandle,
                                               SCHANNEL_HANDLE_CTX,
                                               NULL);

    if (!context)
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    return (NTSTATUS)ApplyControlToken(context, ControlToken);
}

NTSTATUS
NTAPI
SchannelSpQueryContextAttributes(
    _In_ LSA_SEC_HANDLE ContextHandle,
    _In_ ULONG ContextAttribute,
    _Inout_ PVOID Buffer)
{
    PCtxtHandle context = schannelLookupHandle(ContextHandle,
                                               SCHANNEL_HANDLE_CTX,
                                               NULL);

    if (!context)
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    return (NTSTATUS)QueryContextAttributesW(context, ContextAttribute, Buffer);
}

NTSTATUS
NTAPI
SchannelSpAddCredentials(
    _In_ LSA_SEC_HANDLE CredentialHandle,
    _In_opt_ PUNICODE_STRING PrincipalName,
    _In_opt_ PUNICODE_STRING PackageName,
    _In_ ULONG CredentialUseFlags,
    _In_opt_ PVOID AuthData,
    _In_opt_ PVOID GetKeyFn,
    _In_opt_ PVOID GetKeyArg,
    _Out_opt_ PTimeStamp Expiry)
{
    PCredHandle credentials = schannelLookupHandle(CredentialHandle,
                                                   SCHANNEL_HANDLE_CRED,
                                                   NULL);

    if (!credentials)
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    return (NTSTATUS)AddCredentialsW(credentials,
                                     PrincipalName ? PrincipalName->Buffer : NULL,
                                     PackageName ? PackageName->Buffer : NULL,
                                     CredentialUseFlags,
                                     AuthData,
                                     (SEC_GET_KEY_FN)GetKeyFn,
                                     GetKeyArg,
                                     Expiry);
}

NTSTATUS
NTAPI
SchannelSpSetContextAttributes(
    _In_ LSA_SEC_HANDLE ContextHandle,
    _In_ ULONG ContextAttribute,
    _In_ PVOID Buffer,
    _In_ ULONG BufferSize)
{
    PCtxtHandle context = schannelLookupHandle(ContextHandle,
                                               SCHANNEL_HANDLE_CTX,
                                               NULL);

    if (!context)
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    return (NTSTATUS)SetContextAttributesW(context, ContextAttribute, Buffer,
                                           BufferSize);
}

NTSTATUS
NTAPI
SchannelSpSetCredentialsAttributes(
    _In_ LSA_SEC_HANDLE CredentialHandle,
    _In_ ULONG CredentialAttribute,
    _In_ PVOID Buffer,
    _In_ ULONG BufferSize)
{
    PCredHandle credentials = schannelLookupHandle(CredentialHandle,
                                                   SCHANNEL_HANDLE_CRED,
                                                   NULL);

    if (!credentials)
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    return (NTSTATUS)SetCredentialsAttributesW(credentials,
                                               CredentialAttribute,
                                               Buffer,
                                               BufferSize);
}

/***********************************************************************
 * User-mode table adapters
 */
NTSTATUS
NTAPI
SchannelSpInstanceInit(
    _In_ ULONG Version,
    _In_ PSECPKG_DLL_FUNCTIONS FunctionTable,
    _Inout_ PVOID *UserFunctions)
{
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
SchannelUsrMakeSignature(
    _In_ LSA_SEC_HANDLE ContextHandle,
    _In_ ULONG QualityOfProtection,
    _Inout_ PSecBufferDesc MessageBuffers,
    _In_ ULONG MessageSequenceNumber)
{
    PCtxtHandle context = schannelLookupHandle(ContextHandle,
                                               SCHANNEL_HANDLE_CTX,
                                               NULL);

    if (!context)
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    return (NTSTATUS)MakeSignature(context, QualityOfProtection,
                                   MessageBuffers, MessageSequenceNumber);
}

NTSTATUS
NTAPI
SchannelUsrVerifySignature(
    _In_ LSA_SEC_HANDLE ContextHandle,
    _Inout_ PSecBufferDesc MessageBuffers,
    _In_ ULONG MessageSequenceNumber,
    _Out_opt_ PULONG QualityOfProtection)
{
    PCtxtHandle context = schannelLookupHandle(ContextHandle,
                                               SCHANNEL_HANDLE_CTX,
                                               NULL);

    if (!context)
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    return (NTSTATUS)VerifySignature(context, MessageBuffers,
                                     MessageSequenceNumber,
                                     QualityOfProtection);
}

NTSTATUS
NTAPI
SchannelUsrSealMessage(
    _In_ LSA_SEC_HANDLE ContextHandle,
    _In_ ULONG QualityOfProtection,
    _Inout_ PSecBufferDesc MessageBuffers,
    _In_ ULONG MessageSequenceNumber)
{
    PCtxtHandle context = schannelLookupHandle(ContextHandle,
                                               SCHANNEL_HANDLE_CTX,
                                               NULL);

    if (!context)
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    return (NTSTATUS)EncryptMessage(context, QualityOfProtection,
                                    MessageBuffers, MessageSequenceNumber);
}

NTSTATUS
NTAPI
SchannelUsrUnsealMessage(
    _In_ LSA_SEC_HANDLE ContextHandle,
    _Inout_ PSecBufferDesc MessageBuffers,
    _In_ ULONG MessageSequenceNumber,
    _Out_opt_ PULONG QualityOfProtection)
{
    PCtxtHandle context = schannelLookupHandle(ContextHandle,
                                               SCHANNEL_HANDLE_CTX,
                                               NULL);

    if (!context)
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    return (NTSTATUS)DecryptMessage(context, MessageBuffers,
                                    MessageSequenceNumber,
                                    QualityOfProtection);
}

NTSTATUS
NTAPI
SchannelUsrGetContextToken(
    _In_ LSA_SEC_HANDLE ContextHandle,
    _Out_ PHANDLE TokenHandle)
{
    PCtxtHandle context = schannelLookupHandle(ContextHandle,
                                               SCHANNEL_HANDLE_CTX,
                                               NULL);

    if (!context)
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    return (NTSTATUS)QuerySecurityContextToken(context, TokenHandle);
}

NTSTATUS
NTAPI
SchannelUsrCompleteAuthToken(
    _In_ LSA_SEC_HANDLE ContextHandle,
    _In_ PSecBufferDesc Token)
{
    PCtxtHandle context = schannelLookupHandle(ContextHandle,
                                               SCHANNEL_HANDLE_CTX,
                                               NULL);

    if (!context)
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    return (NTSTATUS)CompleteAuthToken(context, Token);
}

NTSTATUS
NTAPI
SchannelUsrDeleteUserModeContext(
    _In_ LSA_SEC_HANDLE ContextHandle)
{
    return SchannelSpDeleteContext(ContextHandle);
}

NTSTATUS
NTAPI
SchannelUsrExportContext(
    _In_ LSA_SEC_HANDLE ContextHandle,
    _In_ ULONG Flags,
    _Inout_ PSecBuffer PackedContext,
    _Out_opt_ PHANDLE Token)
{
    PCtxtHandle context = schannelLookupHandle(ContextHandle,
                                               SCHANNEL_HANDLE_CTX,
                                               NULL);

    if (!context)
        return (NTSTATUS)SEC_E_INVALID_HANDLE;
    return (NTSTATUS)ExportSecurityContext(context, Flags, PackedContext,
                                           Token);
}

NTSTATUS
NTAPI
SchannelUsrImportContext(
    _In_ PSecBuffer PackedContext,
    _In_opt_ HANDLE Token,
    _Out_ PLSA_SEC_HANDLE ContextHandle)
{
    CtxtHandle context;
    SECURITY_STATUS status;

    status = ImportSecurityContextW(SCHANNEL_NAME_W, PackedContext, Token,
                                    &context);
    if (status == SEC_E_OK)
    {
        *ContextHandle = schannelAddHandle(context.dwLower, context.dwUpper,
                                           SCHANNEL_HANDLE_CTX);
        if (!*ContextHandle)
        {
            DeleteSecurityContext(&context);
            status = SEC_E_INTERNAL_ERROR;
        }
    }
    return (NTSTATUS)status;
}
