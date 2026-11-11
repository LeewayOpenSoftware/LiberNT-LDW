#pragma once

#include "wine/nsi.h"

BOOL nsi_reactos_init(void);
void nsi_reactos_cleanup(void);
DWORD nsi_reactos_cancel_change_notification(OVERLAPPED *overlapped);
DWORD nsi_reactos_enumerate_all(struct nsi_enumerate_all_ex *params);
DWORD nsi_reactos_get_all(struct nsi_get_all_parameters_ex *params);
DWORD nsi_reactos_get_parameter(struct nsi_get_parameter_ex *params);
DWORD nsi_reactos_request_change_notification(struct nsi_request_change_notification_ex *params);

#define NSI_BACKEND_EMULATION 1

BOOL nsi_reactos_use_device(const NPI_MODULEID *module, UINT table);
void nsi_reactos_overlay_rows(const NPI_MODULEID *module, UINT table, const void *key_data, void *rw_data, void *static_data, UINT count);
BOOL nsi_reactos_parameter_needs_overlay(const struct nsi_get_parameter_ex *params);
DWORD nsi_reactos_device_get_parameter(struct nsi_get_parameter_ex *params);
