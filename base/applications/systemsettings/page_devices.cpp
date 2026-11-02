/*
 * PROJECT:     LiberNT Settings
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Bluetooth and devices pages
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "settings.h"
#include <initguid.h>
#include <bthdef.h>
#include <bluetoothapis.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <winspool.h>
#include <map>
#include <umpnpmgr/drvinst.h>

typedef INT_PTR (WINAPI *PFN_DEVICEPROPERTIES)(HWND, LPCWSTR, LPCWSTR, BOOL);
typedef BOOL (WINAPI *PFN_INSTALLDEVINST)(HWND, LPCWSTR, BOOL, LPDWORD);

static void
UpdateDeviceDriver(HWND owner, const std::wstring &instance)
{
    HMODULE module = LoadLibraryW(L"newdev.dll");
    PFN_INSTALLDEVINST install;
    DWORD reboot = 0;

    if (!module)
        return;
    install = (PFN_INSTALLDEVINST)GetProcAddress(module, "InstallDevInst");
    if (install)
        install(owner, instance.c_str(), TRUE, &reboot);
    FreeLibrary(module);
}

static void
ShowDeviceProperties(HWND owner, const std::wstring &instance)
{
    HMODULE module = LoadLibraryW(L"devmgr.dll");
    PFN_DEVICEPROPERTIES properties;

    if (!module)
        return;
    properties = (PFN_DEVICEPROPERTIES)GetProcAddress(module, "DevicePropertiesW");
    if (properties)
        properties(owner, NULL, instance.c_str(), FALSE);
    FreeLibrary(module);
}

static HANDLE
OpenRadio(BLUETOOTH_RADIO_INFO *info)
{
    BLUETOOTH_FIND_RADIO_PARAMS params = { sizeof(params) };
    HBLUETOOTH_RADIO_FIND find;
    HANDLE radio = NULL;

    find = BluetoothFindFirstRadio(&params, &radio);
    if (!find)
        return NULL;
    BluetoothFindRadioClose(find);
    if (info)
    {
        ZeroMemory(info, sizeof(*info));
        info->dwSize = sizeof(*info);
        if (BluetoothGetRadioInfo(radio, info) != ERROR_SUCCESS)
            info->szName[0] = UNICODE_NULL;
    }
    return radio;
}

static std::wstring
BluetoothAddressText(const BLUETOOTH_ADDRESS &address)
{
    return Printf(L"%02X:%02X:%02X:%02X:%02X:%02X", address.rgBytes[5], address.rgBytes[4], address.rgBytes[3],
                  address.rgBytes[2], address.rgBytes[1], address.rgBytes[0]);
}

static LPCWSTR
BluetoothIcon(ULONG classOfDevice)
{
    switch (GET_COD_MAJOR(classOfDevice))
    {
        case COD_MAJOR_COMPUTER: return L"laptop_20_regular";
        case COD_MAJOR_PHONE: return L"phone_laptop_20_regular";
        case COD_MAJOR_AUDIO: return L"headphones_20_regular";
        case COD_MAJOR_PERIPHERAL: return L"keyboard_20_regular";
        case COD_MAJOR_IMAGING: return L"print_20_regular";
        default: return L"bluetooth_20_regular";
    }
}

static std::vector<BLUETOOTH_DEVICE_INFO> FoundDevices;
static bool Searching;
static bool Searched;

static std::vector<BLUETOOTH_DEVICE_INFO>
BluetoothDevices(HANDLE radio, bool inquiry)
{
    std::vector<BLUETOOTH_DEVICE_INFO> devices;
    BLUETOOTH_DEVICE_SEARCH_PARAMS params = { sizeof(params) };
    BLUETOOTH_DEVICE_INFO info = { sizeof(info) };
    HBLUETOOTH_DEVICE_FIND find;

    params.fReturnAuthenticated = !inquiry;
    params.fReturnRemembered = !inquiry;
    params.fReturnConnected = !inquiry;
    params.fReturnUnknown = inquiry;
    params.fIssueInquiry = inquiry;
    params.cTimeoutMultiplier = inquiry ? 6 : 0;
    params.hRadio = radio;
    find = BluetoothFindFirstDevice(&params, &info);
    if (!find)
        return devices;
    do
    {
        devices.push_back(info);
        info.dwSize = sizeof(info);
    } while (BluetoothFindNextDevice(find, &info));
    BluetoothFindDeviceClose(find);
    return devices;
}

struct PairContext
{
    HANDLE Radio;
    HWND Owner;
};

static BOOL CALLBACK
PairCallback(void *parameter, BLUETOOTH_AUTHENTICATION_CALLBACK_PARAMS *request)
{
    PairContext *context = (PairContext *)parameter;
    BLUETOOTH_AUTHENTICATE_RESPONSE response;
    std::wstring name = request->deviceInfo.szName;

    ZeroMemory(&response, sizeof(response));
    response.bthAddressRemote = request->deviceInfo.Address;
    response.authMethod = request->authenticationMethod;
    switch (request->authenticationMethod)
    {
        case BLUETOOTH_AUTHENTICATION_METHOD_NUMERIC_COMPARISON:
        case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY_NOTIFICATION:
            response.numericCompInfo.NumericValue = request->Numeric_Value;
            response.negativeResponse = !Confirm(context->Owner, Format(IDS_PAIR_CODE, name.c_str()),
                                                  Printf(L"%06lu", request->Numeric_Value), Str(IDS_PAIR));
            break;
        case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY:
        {
            std::wstring passkey;
            response.negativeResponse = !InputBox(context->Owner, Str(IDS_PAIR), Format(IDS_PAIR_PASSKEY, name.c_str()),
                                                  passkey);
            response.passkeyInfo.passkey = wcstoul(passkey.c_str(), NULL, 10);
            break;
        }
        case BLUETOOTH_AUTHENTICATION_METHOD_LEGACY:
        {
            std::wstring pin;
            response.negativeResponse = !InputBox(context->Owner, Str(IDS_PAIR), Format(IDS_PAIR_PIN, name.c_str()), pin);
            for (size_t i = 0; i < pin.length() && i < BTH_MAX_PIN_SIZE; i++)
                response.pinInfo.pin[i] = (UCHAR)pin[i];
            response.pinInfo.pinLength = (UCHAR)min(pin.length(), (size_t)BTH_MAX_PIN_SIZE);
            break;
        }
        default:
            response.negativeResponse = TRUE;
            break;
    }
    BluetoothSendAuthenticationResponseEx(context->Radio, &response);
    return TRUE;
}

static void
PairDevice(SettingsHost *host, BLUETOOTH_DEVICE_INFO device)
{
    DWORD *result = new DWORD(ERROR_SUCCESS);

    host->RunJob([host, device, result]() mutable
                 {
                     PairContext context;
                     HBLUETOOTH_AUTHENTICATION_REGISTRATION registration = NULL;
                     context.Owner = host->Window();
                     context.Radio = OpenRadio(NULL);
                     if (!context.Radio)
                     {
                         *result = ERROR_DEVICE_NOT_CONNECTED;
                         return;
                     }
                     BluetoothRegisterForAuthenticationEx(&device, &registration, PairCallback, &context);
                     *result = BluetoothAuthenticateDeviceEx(context.Owner, context.Radio, &device, NULL,
                                                             MITMProtectionNotRequiredGeneralBonding);
                     if (registration)
                         BluetoothUnregisterAuthentication(registration);
                     CloseHandle(context.Radio);
                 },
                 [host, result]()
                 {
                     if (*result != ERROR_SUCCESS)
                         ShowError(host->Window(), *result);
                     delete result;
                     FoundDevices.clear();
                     Searched = false;
                     host->Refresh();
                 });
}

static SRWLOCK DiscoveredLock = SRWLOCK_INIT;
static std::vector<BLUETOOTH_DEVICE_INFO> Discovered;
static HCMNOTIFICATION DiscoveryNotify;
static HANDLE DiscoveryRadio;

static void
AddFoundDevice(const BLUETOOTH_DEVICE_INFO &device)
{
    for (size_t i = 0; i < FoundDevices.size(); i++)
    {
        if (FoundDevices[i].Address.ullLong == device.Address.ullLong)
        {
            if (device.szName[0] || !FoundDevices[i].szName[0])
                FoundDevices[i] = device;
            return;
        }
    }
    FoundDevices.push_back(device);
}

static DWORD CALLBACK
DeviceInRange(HCMNOTIFICATION notify, PVOID context, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA data, DWORD size)
{
    const BTH_RADIO_IN_RANGE *range;
    BLUETOOTH_DEVICE_INFO device = { sizeof(device) };

    if (action != CM_NOTIFY_ACTION_DEVICECUSTOMEVENT ||
        size < FIELD_OFFSET(CM_NOTIFY_EVENT_DATA, u.DeviceHandle.Data) + sizeof(BTH_RADIO_IN_RANGE) ||
        !IsEqualGUID(data->u.DeviceHandle.EventGuid, GUID_BLUETOOTH_RADIO_IN_RANGE) ||
        data->u.DeviceHandle.DataSize < sizeof(BTH_RADIO_IN_RANGE))
        return ERROR_SUCCESS;
    range = (const BTH_RADIO_IN_RANGE *)data->u.DeviceHandle.Data;
    if (!(range->deviceInfo.flags & BDIF_ADDRESS) || (range->deviceInfo.flags & BDIF_PAIRED))
        return ERROR_SUCCESS;
    device.Address.ullLong = range->deviceInfo.address;
    device.ulClassofDevice = range->deviceInfo.classOfDevice;
    device.fConnected = (range->deviceInfo.flags & BDIF_CONNECTED) != 0;
    device.fRemembered = (range->deviceInfo.flags & BDIF_PERSONAL) != 0;
    if (range->deviceInfo.flags & BDIF_NAME)
        MultiByteToWideChar(CP_UTF8, 0, range->deviceInfo.name, -1, device.szName, BLUETOOTH_MAX_NAME_SIZE);
    AcquireSRWLockExclusive(&DiscoveredLock);
    Discovered.push_back(device);
    ReleaseSRWLockExclusive(&DiscoveredLock);
    PostMessageW((HWND)context, WM_SETTINGS_REFRESH, 0, 0);
    return ERROR_SUCCESS;
}

static void
WatchDiscovery(SettingsHost *host, bool on)
{
    if (on && !DiscoveryNotify)
    {
        CM_NOTIFY_FILTER filter = { sizeof(filter) };

        DiscoveryRadio = OpenRadio(NULL);
        if (!DiscoveryRadio)
            return;
        filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE;
        filter.u.DeviceHandle.hTarget = DiscoveryRadio;
        if (CM_Register_Notification(&filter, host->Window(), DeviceInRange, &DiscoveryNotify) != CR_SUCCESS)
            DiscoveryNotify = NULL;
    }
    else if (!on)
    {
        if (DiscoveryNotify)
            CM_Unregister_Notification(DiscoveryNotify);
        DiscoveryNotify = NULL;
        AcquireSRWLockExclusive(&DiscoveredLock);
        Discovered.clear();
        ReleaseSRWLockExclusive(&DiscoveredLock);
    }
    if (!DiscoveryNotify && DiscoveryRadio)
    {
        CloseHandle(DiscoveryRadio);
        DiscoveryRadio = NULL;
    }
}

static void
SearchDevices(SettingsHost *host)
{
    std::vector<BLUETOOTH_DEVICE_INFO> *found = new std::vector<BLUETOOTH_DEVICE_INFO>();

    Searching = true;
    FoundDevices.clear();
    WatchDiscovery(host, true);
    host->Refresh();
    host->RunJob([found]()
                 {
                     HANDLE radio = OpenRadio(NULL);
                     if (radio)
                     {
                         *found = BluetoothDevices(radio, true);
                         CloseHandle(radio);
                     }
                 },
                 [host, found]()
                 {
                     WatchDiscovery(host, false);
                     for (size_t i = 0; i < found->size(); i++)
                         AddFoundDevice((*found)[i]);
                     delete found;
                     Searching = false;
                     Searched = true;
                     host->Refresh();
                 });
}

void
BuildBluetooth(PageBuilder &page)
{
    SettingsHost *host = page.Host;
    BLUETOOTH_RADIO_INFO info;
    HANDLE radio;
    std::vector<BLUETOOTH_DEVICE_INFO> devices;
    BOOL discoverable, connectable;

    if (page.Indexing)
    {
        page.Group();
        page.Toggle(Str(IDS_BT_DISCOVERABLE), Str(IDS_BT_DISCOVERABLE_DESC), L"eye_20_regular", false, nullptr);
        page.Toggle(Str(IDS_BT_CONNECTABLE), Str(IDS_BT_CONNECTABLE_DESC), L"bluetooth_20_regular", false, nullptr);
        page.Button(Str(IDS_BT_ADD), Str(IDS_BT_ADD_DESC), L"add_circle_20_regular", Str(IDS_ADD_DEVICE), nullptr)
            .Keywords = Str(IDS_BT_KEYWORDS);
        return;
    }
    AcquireSRWLockExclusive(&DiscoveredLock);
    for (size_t i = 0; i < Discovered.size(); i++)
        AddFoundDevice(Discovered[i]);
    Discovered.clear();
    ReleaseSRWLockExclusive(&DiscoveredLock);
    radio = OpenRadio(&info);
    if (!radio)
    {
        page.Hero(Str(IDS_PAGE_BLUETOOTH), Str(IDS_BT_NO_RADIO), L"bluetooth_20_regular");
        return;
    }
    discoverable = BluetoothIsDiscoverable(radio);
    connectable = BluetoothIsConnectable(radio);
    page.Hero(info.szName[0] ? info.szName : Str(IDS_PAGE_BLUETOOTH),
              Format(discoverable ? IDS_BT_RADIO_DISCOVERABLE : IDS_BT_RADIO, BluetoothAddressText(info.address).c_str()),
              L"bluetooth_20_regular");
    page.Group();
    page.Toggle(Str(IDS_BT_DISCOVERABLE), Str(IDS_BT_DISCOVERABLE_DESC), L"eye_20_regular", discoverable != FALSE,
                [](int on)
                {
                    HANDLE radio = OpenRadio(NULL);
                    if (radio)
                    {
                        BluetoothEnableDiscovery(radio, on != 0);
                        CloseHandle(radio);
                    }
                });
    page.Toggle(Str(IDS_BT_CONNECTABLE), Str(IDS_BT_CONNECTABLE_DESC), L"bluetooth_20_regular", connectable != FALSE,
                [](int on)
                {
                    HANDLE radio = OpenRadio(NULL);
                    if (radio)
                    {
                        BluetoothEnableIncomingConnections(radio, on != 0);
                        CloseHandle(radio);
                    }
                });

    devices = BluetoothDevices(radio, false);
    CloseHandle(radio);
    page.Group(Str(IDS_BT_DEVICES));
    if (devices.empty())
        page.Text(Str(IDS_BT_NO_DEVICES));
    for (size_t i = 0; i < devices.size(); i++)
    {
        BLUETOOTH_DEVICE_INFO device = devices[i];
        UINT state = device.fConnected ? IDS_BT_CONNECTED : (device.fAuthenticated ? IDS_BT_PAIRED : IDS_BT_REMEMBERED);
        SettingRow &row = page.Button(device.szName[0] ? device.szName : BluetoothAddressText(device.Address),
                                      Str(state), BluetoothIcon(device.ulClassofDevice), Str(IDS_REMOVE),
                                      [host, device](int)
                                      {
                                          BLUETOOTH_ADDRESS address = device.Address;
                                          if (!Confirm(host->Window(), Format(IDS_BT_REMOVE_TITLE, device.szName),
                                                       Str(IDS_BT_REMOVE_TEXT), Str(IDS_REMOVE)))
                                              return;
                                          if (BluetoothRemoveDevice(&address) != ERROR_SUCCESS)
                                              ShowError(host->Window(), ERROR_NOT_FOUND);
                                      });
        UNREFERENCED_PARAMETER(row);
    }

    page.Group(Str(IDS_BT_ADD_GROUP));
    {
        SettingRow &row = page.Button(Str(IDS_BT_ADD), Str(Searching ? IDS_BT_SEARCHING : IDS_BT_ADD_DESC),
                                      L"add_circle_20_regular", Str(IDS_SEARCH_DEVICES),
                                      [host](int) { SearchDevices(host); });
        row.Enabled = !Searching;
        row.Accent = true;
        row.Keywords = Str(IDS_BT_KEYWORDS);
    }
    for (size_t i = 0; i < FoundDevices.size(); i++)
    {
        BLUETOOTH_DEVICE_INFO device = FoundDevices[i];
        bool known = false;
        for (size_t j = 0; j < devices.size() && !known; j++)
            known = devices[j].Address.ullLong == device.Address.ullLong;
        if (known)
            continue;
        page.Button(device.szName[0] ? device.szName : BluetoothAddressText(device.Address),
                    BluetoothAddressText(device.Address), BluetoothIcon(device.ulClassofDevice), Str(IDS_PAIR),
                    [host, device](int) { PairDevice(host, device); });
    }
    if (Searched && !Searching && FoundDevices.empty())
        page.Text(Str(IDS_BT_NOTHING_FOUND));
}

static const GUID *DeviceClasses[] =
{
    &GUID_DEVCLASS_MOUSE, &GUID_DEVCLASS_KEYBOARD, &GUID_DEVCLASS_MONITOR, &GUID_DEVCLASS_MEDIA,
    &GUID_DEVCLASS_IMAGE, &GUID_DEVCLASS_PRINTER, &GUID_DEVCLASS_BLUETOOTH, &GUID_DEVCLASS_DISKDRIVE,
};

static std::wstring
DeviceText(HDEVINFO set, SP_DEVINFO_DATA *data, DWORD property)
{
    WCHAR text[256];

    if (!SetupDiGetDeviceRegistryPropertyW(set, data, property, NULL, (BYTE *)text, sizeof(text), NULL))
        return std::wstring();
    return text;
}

static void
AddDeviceList(PageBuilder &page)
{
    SettingsHost *host = page.Host;
    HDEVINFO set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    SP_DEVINFO_DATA data = { sizeof(data) };

    if (set == INVALID_HANDLE_VALUE)
        return;
    for (UINT c = 0; c < _countof(DeviceClasses); c++)
    {
        WCHAR className[128] = L"";
        bool header = false;

        SetupDiGetClassDescriptionW(DeviceClasses[c], className, _countof(className), NULL);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); i++, data.cbSize = sizeof(data))
        {
            WCHAR instance[MAX_DEVICE_ID_LEN];
            std::wstring name;
            HICON icon = NULL;

            if (!IsEqualGUID(data.ClassGuid, *DeviceClasses[c]))
                continue;
            name = DeviceText(set, &data, SPDRP_FRIENDLYNAME);
            if (name.empty())
                name = DeviceText(set, &data, SPDRP_DEVICEDESC);
            if (name.empty() || !SetupDiGetDeviceInstanceIdW(set, &data, instance, _countof(instance), NULL))
                continue;
            if (!header)
            {
                page.Group(className);
                header = true;
            }
            SetupDiLoadClassIcon(DeviceClasses[c], &icon, NULL);
            {
                std::wstring id = instance;
                SettingRow &row = page.Link(name, std::wstring(), NULL,
                                            [host, id](int) { ShowDeviceProperties(host->Window(), id); });
                row.Image = icon;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
}

typedef DWORD (WINAPI *PFN_DRVINST_SUBMIT)(PCWSTR, DWORD, DRVINST_JOB_ID *);
typedef DWORD (WINAPI *PFN_DRVINST_GETJOB)(DRVINST_JOB_ID, DRVINST_JOB_INFO *);
typedef DWORD (WINAPI *PFN_DRVINST_ENUMJOBS)(DRVINST_JOB_ID *, DWORD *);

struct DrvJobEntry
{
    DRVINST_JOB_ID Id = 0;
    ULONG State = 0;
    ULONG Win32 = 0;
    ULONG Problem = 0;
    ULONG Flags = 0;
    ULONG Failures = 0;
    bool Finished = false;
};

static std::wstring
NormalizeInstanceId(const std::wstring &id)
{
    std::wstring key(id.size(), L'\0');

    for (size_t i = 0; i < id.size(); i++)
        key[i] = (WCHAR)towlower(id[i]);
    return key;
}

/* newdev.dll is cached for the process lifetime; it is never freed while a
 * submitted job can still be running in the service. */
static HMODULE g_drvInstModule;
static bool g_drvInstTried;
static PFN_DRVINST_SUBMIT g_drvInstSubmit;
static PFN_DRVINST_GETJOB g_drvInstGetJob;
static PFN_DRVINST_ENUMJOBS g_drvInstEnumJobs;
static std::map<std::wstring, DrvJobEntry> g_driverJobs;

static bool
DrvInstAvailable(void)
{
    if (!g_drvInstTried)
    {
        g_drvInstTried = true;
        g_drvInstModule = LoadLibraryW(L"newdev.dll");
        if (g_drvInstModule)
        {
            g_drvInstSubmit = (PFN_DRVINST_SUBMIT)GetProcAddress(g_drvInstModule, "DrvInstSubmitDevice");
            g_drvInstGetJob = (PFN_DRVINST_GETJOB)GetProcAddress(g_drvInstModule, "DrvInstGetJob");
            g_drvInstEnumJobs = (PFN_DRVINST_ENUMJOBS)GetProcAddress(g_drvInstModule, "DrvInstEnumJobs");
        }
    }
    return g_drvInstSubmit && g_drvInstGetJob;
}

static bool
PollDriverJobs(void)
{
    bool active = false;

    if (!DrvInstAvailable())
        return false;
    for (auto &pair : g_driverJobs)
    {
        DrvJobEntry &entry = pair.second;
        DRVINST_JOB_INFO info;
        DWORD err;

        if (entry.Finished)
            continue;
        ZeroMemory(&info, sizeof(info));
        info.Size = sizeof(info);
        err = g_drvInstGetJob(entry.Id, &info);
        if (err == ERROR_SUCCESS)
        {
            entry.State = info.State;
            entry.Win32 = info.Result.Win32;
            entry.Problem = info.Result.Problem;
            entry.Flags = info.Result.Flags;
            entry.Failures = 0;
            if (info.State >= 2 || info.EndTick != 0)
                entry.Finished = true;
            else
                active = true;
        }
        else
        {
            entry.Failures++;
            if (entry.Failures >= 3)
            {
                entry.State = 3;
                entry.Win32 = err;
                entry.Finished = true;
            }
            else
            {
                active = true;
            }
        }
    }
    return active;
}

static void
AutoSearchDriver(const std::wstring &id)
{
    const std::wstring key = NormalizeInstanceId(id);
    auto existing = g_driverJobs.find(key);
    DRVINST_JOB_ID job = 0;
    DrvJobEntry entry;
    DWORD err;

    if (!DrvInstAvailable())
        return;
    if (existing != g_driverJobs.end() && !existing->second.Finished)
        return;
    entry.Id = 0;
    entry.State = 0;
    entry.Win32 = 0;
    entry.Problem = 0;
    entry.Flags = 0;
    entry.Failures = 0;
    entry.Finished = false;
    err = g_drvInstSubmit(id.c_str(), DRVINST_F_SILENT, &job);
    if (err != ERROR_SUCCESS)
    {
        entry.State = 3;
        entry.Win32 = err;
        entry.Finished = true;
    }
    else
    {
        entry.Id = job;
    }
    g_driverJobs[key] = entry;
}

static std::wstring
JobStatusText(const DrvJobEntry &entry)
{
    std::wstring text;

    switch (entry.State)
    {
        case 0: text = Str(IDS_DRIVER_STATUS_QUEUED); break;
        case 1: text = Str(IDS_DRIVER_STATUS_INSTALLING); break;
        case 2:
            if (!(entry.Flags & DRVINST_RF_STARTED))
                text = Str(IDS_DRIVER_STATUS_PENDING_START);
            else
                text = Str(IDS_DRIVER_STATUS_SUCCEEDED);
            break;
        case 3:
            if (entry.Flags & DRVINST_RF_ROLLEDBACK)
                text = Printf(Str(IDS_DRIVER_STATUS_ROLLBACK_PROBLEM).c_str(), entry.Problem);
            else
                text = Printf(Str(IDS_DRIVER_STATUS_FAILED).c_str(), entry.Win32);
            break;
        case 4: text = Str(IDS_DRIVER_STATUS_TIMED_OUT); break;
        case 5: text = Str(IDS_DRIVER_STATUS_CANCELED); break;
        case DrvInstJobRolledBack:
            text = Printf(Str(IDS_DRIVER_STATUS_ROLLED_BACK).c_str(), entry.Win32);
            break;
        default: text = Str(IDS_DRIVER_STATUS_UNKNOWN); break;
    }
    if (entry.State == 2 && (entry.Flags & DRVINST_RF_NEEDREBOOT))
        text += L" (" + Str(IDS_DRIVER_NEED_REBOOT) + L")";
    return text;
}

static void
BuildDriverJobHistory(PageBuilder &page)
{
    DWORD err;

    page.Group(Str(IDS_DRIVERS_HISTORY));
    if (!DrvInstAvailable())
    {
        page.Text(Str(IDS_DRIVER_SERVICE_UNAVAILABLE));
        return;
    }
    if (g_drvInstEnumJobs)
    {
        DRVINST_JOB_ID ids[64];
        DWORD count = _countof(ids);

        err = g_drvInstEnumJobs(ids, &count);
        if ((err == ERROR_SUCCESS || err == ERROR_MORE_DATA) && count > 0)
        {
            for (DWORD i = 0; i < count; i++)
            {
                DRVINST_JOB_INFO info;
                DrvJobEntry entry;
                std::wstring title;

                ZeroMemory(&info, sizeof(info));
                info.Size = sizeof(info);
                if (g_drvInstGetJob(ids[i], &info) != ERROR_SUCCESS)
                    continue;
                entry.Id = info.JobId;
                entry.State = info.State;
                entry.Win32 = info.Result.Win32;
                entry.Problem = info.Result.Problem;
                entry.Flags = info.Result.Flags;
                entry.Finished = info.State >= 2 || info.EndTick != 0;
                title = Printf(L"%I64u  %s", info.JobId, info.DeviceInstanceId);
                if (info.EndTick != 0 && info.EndTick >= info.SubmitTick)
                    title += Printf(L"  (%lus)", (info.EndTick - info.SubmitTick) / 1000);
                page.Info(title, JobStatusText(entry), NULL);
            }
            return;
        }
        if (err == ERROR_SUCCESS)
        {
            page.Text(Str(IDS_DRIVER_HISTORY_EMPTY));
            return;
        }
    }
    page.Text(Str(IDS_DRIVER_SERVICE_UNAVAILABLE));
}

static std::wstring
DriverStatusText(SP_DEVINFO_DATA *data)
{
    DWORD status = 0;
    DWORD problem = 0;
    CONFIGRET result = CM_Get_DevNode_Status(&status, &problem, data->DevInst, 0);

    if (result != CR_SUCCESS)
        return Str(IDS_DRIVER_STATUS_UNKNOWN);
    if (problem == CM_PROB_FAILED_INSTALL)
        return Printf(Str(IDS_DRIVER_STATUS_NO_DRIVER).c_str(), problem);
    if (problem != 0 || (status & DN_HAS_PROBLEM))
        return Printf(Str(IDS_DRIVER_STATUS_PROBLEM).c_str(), problem);
    return Str(IDS_DRIVER_STATUS_OK);
}

void
BuildDrivers(PageBuilder &page)
{
    SettingsHost *host = page.Host;
    HDEVINFO set;
    SP_DEVINFO_DATA data = { sizeof(data) };
    UINT count = 0;
    bool active;

    page.Hero(Str(IDS_PAGE_DRIVERS), Str(IDS_DRIVERS_DESC), L"device_meeting_room_20_regular");
    if (page.Indexing)
        return;
    active = PollDriverJobs();
    set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE)
        return;
    page.Group(Str(IDS_DRIVERS_DEVICES));
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); i++, data.cbSize = sizeof(data))
    {
        WCHAR instance[MAX_DEVICE_ID_LEN];
        std::wstring name;
        HICON icon = NULL;

        name = DeviceText(set, &data, SPDRP_FRIENDLYNAME);
        if (name.empty())
            name = DeviceText(set, &data, SPDRP_DEVICEDESC);
        if (name.empty())
            name = DeviceText(set, &data, SPDRP_HARDWAREID);
        if (name.empty())
            name = Str(IDS_DRIVER_UNKNOWN_DEVICE);
        if (!SetupDiGetDeviceInstanceIdW(set, &data, instance, _countof(instance), NULL))
            continue;
        SetupDiLoadClassIcon(&data.ClassGuid, &icon, NULL);
        {
            std::wstring id = instance;
            auto job = g_driverJobs.find(NormalizeInstanceId(id));
            SettingRow &row = page.Button(name,
                                          job != g_driverJobs.end() ? JobStatusText(job->second)
                                                                    : DriverStatusText(&data),
                                          NULL, Str(IDS_UPDATE_DRIVER),
                                          [host, id](int action) {
                                              if (action == 0)
                                              {
                                                  g_driverJobs.erase(NormalizeInstanceId(id));
                                                  UpdateDeviceDriver(host->Window(), id);
                                              }
                                              else
                                              {
                                                  AutoSearchDriver(id);
                                              }
                                          });
            if (DrvInstAvailable())
                row.Actions.push_back(Str(IDS_DRIVER_AUTO_SEARCH));
            row.Image = icon;
        }
        count++;
    }
    if (count == 0)
        page.Text(Str(IDS_DRIVERS_NONE));
    SetupDiDestroyDeviceInfoList(set);
    BuildDriverJobHistory(page);
    if (active)
        page.Host->SetTimerRefresh(1500);
}

void
BuildDevices(PageBuilder &page)
{
    std::wstring bluetooth;

    if (!page.Indexing)
    {
        HANDLE radio = OpenRadio(NULL);
        bluetooth = Str(radio ? IDS_BT_AVAILABLE : IDS_BT_NO_RADIO);
        if (radio)
            CloseHandle(radio);
    }
    page.Group();
    page.Navigate(Str(IDS_PAGE_BLUETOOTH), bluetooth.empty() ? Str(IDS_BT_DESC) : bluetooth, L"bluetooth_20_regular",
                  PageId::Bluetooth).Keywords = Str(IDS_BT_KEYWORDS);
    page.Navigate(Str(IDS_PAGE_MOUSE), Str(IDS_MOUSE_DESC), L"cursor_20_regular", PageId::Mouse);
    page.Navigate(Str(IDS_PAGE_TYPING), Str(IDS_TYPING_DESC), L"keyboard_20_regular", PageId::Typing);
    page.Navigate(Str(IDS_PAGE_PRINTERS), Str(IDS_PRINTERS_DESC), L"print_20_regular", PageId::Printers);
    page.Navigate(Str(IDS_PAGE_DRIVERS), Str(IDS_DRIVERS_DESC), L"device_meeting_room_20_regular", PageId::Drivers);
    if (!page.Indexing)
        AddDeviceList(page);
}

static void
SetSpiValue(UINT action, UINT uiParam, PVOID pvParam)
{
    SystemParametersInfoW(action, uiParam, pvParam, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

void
BuildMouse(PageBuilder &page)
{
    std::vector<std::wstring> buttons, wheel;
    int speed = 10, mouse[3] = { 0, 0, 0 };
    UINT lines = 3;
    BOOL swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;

    if (!page.Indexing)
    {
        SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &speed, 0);
        SystemParametersInfoW(SPI_GETMOUSE, 0, mouse, 0);
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    }
    buttons.push_back(Str(IDS_LEFT));
    buttons.push_back(Str(IDS_RIGHT));
    page.Group(Str(IDS_PAGE_MOUSE));
    page.Choice(Str(IDS_PRIMARY_BUTTON), std::wstring(), L"cursor_click_20_regular", buttons, swapped ? 1 : 0,
                [](int index)
                {
                    SwapMouseButton(index == 1);
                    SetSpiValue(SPI_SETMOUSEBUTTONSWAP, index == 1, NULL);
                });
    {
        SettingRow &row = page.Slider(Str(IDS_POINTER_SPEED), std::wstring(), L"cursor_20_regular", 1, 20, speed,
                                      [](int value) { SetSpiValue(SPI_SETMOUSESPEED, 0, (PVOID)(INT_PTR)value); });
        row.Live = true;
        row.Label = [](int value) { return Printf(L"%d", value); };
    }
    page.Toggle(Str(IDS_ENHANCE_PRECISION), Str(IDS_ENHANCE_PRECISION_DESC), L"cursor_20_regular", mouse[2] != 0,
                [](int on)
                {
                    int values[3] = { on ? 6 : 0, on ? 10 : 0, on ? 1 : 0 };
                    SetSpiValue(SPI_SETMOUSE, 0, values);
                });

    page.Group(Str(IDS_SCROLLING));
    wheel.push_back(Str(IDS_SCROLL_LINES));
    wheel.push_back(Str(IDS_SCROLL_SCREEN));
    page.Choice(Str(IDS_WHEEL_SCROLLS), std::wstring(), NULL, wheel, lines == WHEEL_PAGESCROLL ? 1 : 0,
                [](int index) { SetSpiValue(SPI_SETWHEELSCROLLLINES, index == 1 ? WHEEL_PAGESCROLL : 3, NULL); });
    {
        SettingRow &row = page.Slider(Str(IDS_SCROLL_LINE_COUNT), std::wstring(), NULL, 1, 100,
                                      lines == WHEEL_PAGESCROLL ? 3 : (int)lines,
                                      [](int value) { SetSpiValue(SPI_SETWHEELSCROLLLINES, (UINT)value, NULL); });
        row.Enabled = lines != WHEEL_PAGESCROLL;
        row.Label = [](int value) { return Printf(L"%d", value); };
    }
    {
        SettingRow &row = page.Slider(Str(IDS_DOUBLE_CLICK), Str(IDS_DOUBLE_CLICK_DESC), NULL, 200, 900,
                                      (int)GetDoubleClickTime(),
                                      [](int value) { SetSpiValue(SPI_SETDOUBLECLICKTIME, (UINT)value, NULL); });
        row.Label = [](int value) { return Format(IDS_MILLISECONDS, value); };
    }
}

void
BuildTyping(PageBuilder &page)
{
    int delay = 1, speed = 31;
    UINT blink = GetCaretBlinkTime();

    if (!page.Indexing)
    {
        SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay, 0);
        SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &speed, 0);
    }
    page.Group(Str(IDS_KEYBOARD_REPEAT));
    {
        SettingRow &row = page.Slider(Str(IDS_REPEAT_DELAY), Str(IDS_REPEAT_DELAY_DESC), L"keyboard_20_regular", 0, 3,
                                      3 - delay, [](int value) { SetSpiValue(SPI_SETKEYBOARDDELAY, (UINT)(3 - value), NULL); });
        row.Label = [](int value) { return Str(value <= 0 ? IDS_LONG : (value >= 3 ? IDS_SHORT : IDS_MEDIUM)); };
    }
    {
        SettingRow &row = page.Slider(Str(IDS_REPEAT_RATE), Str(IDS_REPEAT_RATE_DESC), L"keyboard_20_regular", 0, 31,
                                      speed, [](int value) { SetSpiValue(SPI_SETKEYBOARDSPEED, (UINT)value, NULL); });
        row.Label = [](int value) { return Str(value < 10 ? IDS_SLOW : (value > 22 ? IDS_FAST : IDS_MEDIUM)); };
    }
    page.Group(Str(IDS_TEXT_CURSOR));
    {
        SettingRow &row = page.Slider(Str(IDS_CURSOR_BLINK), Str(IDS_CURSOR_BLINK_DESC), NULL, 0, 10,
                                      blink == INFINITE ? 0 : max(1, 11 - (int)(blink / 100)),
                                      [](int value)
                                      {
                                          UINT time = value ? (UINT)(11 - value) * 100 : INFINITE;
                                          SetCaretBlinkTime(time);
                                          WriteString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"CursorBlinkRate",
                                                      value ? Printf(L"%u", time) : std::wstring(L"-1"));
                                      });
        row.Label = [](int value) { return value ? Printf(L"%d", value) : Str(IDS_NONE); };
    }
    page.Group(Str(IDS_RELATED));
    page.Navigate(Str(IDS_INPUT_LANGUAGES), Str(IDS_INPUT_LANGUAGES_DESC), L"local_language_20_regular",
                  PageId::Language);
}

void
BuildPrinters(PageBuilder &page)
{
    SettingsHost *host = page.Host;
    DWORD needed = 0, count = 0;
    WCHAR defaultName[512] = L"";
    DWORD defaultSize = _countof(defaultName);
    std::vector<BYTE> buffer;

    page.Group(Str(IDS_PRINTERS));
    if (page.Indexing)
        return;
    GetDefaultPrinterW(defaultName, &defaultSize);
    EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, NULL, 0, &needed, &count);
    if (needed)
    {
        buffer.resize(needed);
        if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, buffer.data(), needed, &needed,
                           &count))
            count = 0;
    }
    if (!count)
        page.Text(Str(IDS_NO_PRINTERS));
    for (DWORD i = 0; i < count; i++)
    {
        PRINTER_INFO_2W *printer = &((PRINTER_INFO_2W *)buffer.data())[i];
        std::wstring name = printer->pPrinterName;
        bool isDefault = !lstrcmpiW(printer->pPrinterName, defaultName);
        SettingRow &row = page.Button(name, Str(isDefault ? IDS_DEFAULT_PRINTER : (printer->Status ? IDS_PRINTER_BUSY :
                                                                                  IDS_PRINTER_READY)),
                                      L"print_20_regular", std::wstring(),
                                      [host, name, isDefault](int action)
                                      {
                                          if (!isDefault && action == 0)
                                          {
                                              if (!SetDefaultPrinterW(name.c_str()))
                                                  ShowError(host->Window(), GetLastError());
                                              return;
                                          }
                                          if (Confirm(host->Window(), Format(IDS_REMOVE_PRINTER_TITLE, name.c_str()),
                                                      Str(IDS_REMOVE_PRINTER_TEXT), Str(IDS_REMOVE)))
                                          {
                                              HANDLE handle = NULL;
                                              PRINTER_DEFAULTSW defaults = { NULL, NULL, PRINTER_ALL_ACCESS };
                                              if (!OpenPrinterW((LPWSTR)name.c_str(), &handle, &defaults) ||
                                                  !DeletePrinter(handle))
                                                  ShowError(host->Window(), GetLastError());
                                              if (handle)
                                                  ClosePrinter(handle);
                                          }
                                      });
        if (!isDefault)
            row.Actions.push_back(Str(IDS_SET_DEFAULT));
        row.Actions.push_back(Str(IDS_REMOVE));
    }
}
