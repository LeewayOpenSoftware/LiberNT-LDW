/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     LiberNTUpdateSvc registration for the online bootstrap
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "bootstrap.h"

DWORD
BsServiceQuery(
    _Out_ BOOL *Exists)
{
    SC_HANDLE Manager, Service;
    DWORD Error = ERROR_SUCCESS;

    *Exists = FALSE;
    Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!Manager)
        return GetLastError();
    Service = OpenServiceW(Manager, BS_SERVICE_NAME, SERVICE_QUERY_STATUS);
    if (Service)
    {
        *Exists = TRUE;
        CloseServiceHandle(Service);
    }
    else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
        Error = GetLastError();
    CloseServiceHandle(Manager);
    return Error;
}

DWORD
BsServiceInstall(
    _In_ const BS_TARGET *Target,
    _In_ BOOL Enable,
    _Out_ BOOL *Installed)
{
    WCHAR BinaryPath[BS_MAX_PATH_BUF];
    SC_HANDLE Manager, Service;
    DWORD Error = ERROR_SUCCESS;

    *Installed = FALSE;
    if (Target->IsImage)
        return ERROR_NOT_SUPPORTED; /* services are registered online only */

    if (BsBuildTargetPath(BinaryPath, RTL_NUMBER_OF(BinaryPath), Target,
                          L"LiberNT\\Servicing\\bin\\LiberNTUpdateSvc.exe") != ERROR_SUCCESS)
        return ERROR_FILENAME_EXCED_RANGE;

    Manager = OpenSCManagerW(NULL, NULL,
                             SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    if (!Manager)
        return GetLastError();
    Service = OpenServiceW(Manager, BS_SERVICE_NAME,
                           SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS);
    if (Service)
    {
        /* Already registered: idempotent re-run, and the requested start
         * type is enforced either way, so a failed re-install self-test
         * cannot leave an enabled service behind (Pam review B6). */
        ChangeServiceConfigW(Service, SERVICE_NO_CHANGE,
                             Enable ? SERVICE_DEMAND_START : SERVICE_DISABLED,
                             SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL);
        CloseServiceHandle(Service);
        CloseServiceHandle(Manager);
        *Installed = TRUE;
        return ERROR_SUCCESS;
    }
    if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
    {
        Error = GetLastError();
        CloseServiceHandle(Manager);
        return Error;
    }

    /* Created disabled; the bootstrap enables it only after the self-test. */
    Service = CreateServiceW(Manager, BS_SERVICE_NAME,
                             L"LiberNT Update Servicing",
                             SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS,
                             SERVICE_WIN32_OWN_PROCESS,
                             Enable ? SERVICE_DEMAND_START : SERVICE_DISABLED,
                             SERVICE_ERROR_NORMAL, BinaryPath,
                             NULL, NULL, NULL, NULL, NULL);
    if (!Service)
        Error = GetLastError();
    else
    {
        *Installed = TRUE;
        CloseServiceHandle(Service);
    }
    CloseServiceHandle(Manager);
    return Error;
}

DWORD
BsServiceRemove(
    _In_ const BS_TARGET *Target)
{
    SC_HANDLE Manager, Service;
    DWORD Error = ERROR_SUCCESS;
    BOOL Exists;

    if (Target->IsImage)
        return ERROR_SUCCESS; /* offline images carry no live service record */
    Error = BsServiceQuery(&Exists);
    if (Error != ERROR_SUCCESS || !Exists)
        return Error;
    Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!Manager)
        return GetLastError();
    Service = OpenServiceW(Manager, BS_SERVICE_NAME, DELETE | SERVICE_STOP);
    if (!Service)
    {
        CloseServiceHandle(Manager);
        return GetLastError();
    }
    ControlService(Service, SERVICE_CONTROL_STOP, &(SERVICE_STATUS){0});
    if (!DeleteService(Service))
        Error = GetLastError();
    CloseServiceHandle(Service);
    CloseServiceHandle(Manager);
    return Error;
}
