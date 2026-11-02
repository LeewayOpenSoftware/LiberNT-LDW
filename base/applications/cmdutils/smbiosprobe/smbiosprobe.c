/*
 * smbiosprobe - SMBIOS vs WMI probe (LiberNT observability, objective C, local only).
 *
 * Reads the raw SMBIOS table with GetSystemFirmwareTable('RSMB'), decodes types 0, 1, 3 and 17 with an
 * independent parser, queries the same facts through WMI (Win32_BIOS, Win32_ComputerSystem,
 * Win32_ComputerSystemProduct, Win32_SystemEnclosure, Win32_PhysicalMemory) and prints both side by side.
 * A WMI value that is not in the firmware table is a defect (invented data); a firmware value the WMI class
 * does not show is a gap. Read-only.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#define INITGUID
#include <initguid.h>
#include <wbemcli.h>
#include <stdio.h>
#include <stddef.h>

#pragma pack(push, 1)
struct hdr { BYTE type, length; WORD handle; };
#pragma pack(pop)

static BYTE *g_buf;
static DWORD g_len;     /* SMBIOS table bytes (after the 8-byte RawSMBIOSData prologue) */
static BYTE *g_tab;

static const BYTE *next_struct(const BYTE *p)
{
    const BYTE *q = p + ((const struct hdr *)p)->length;

    while (q + 1 < g_tab + g_len && (q[0] || q[1])) q++;
    return q + 2;
}

static const BYTE *find(BYTE type, int index)
{
    const BYTE *p = g_tab;

    while (p + sizeof(struct hdr) <= g_tab + g_len)
    {
        const struct hdr *h = (const struct hdr *)p;

        if (h->type == 127 || h->length < sizeof(*h)) break;
        if (h->type == type && index-- == 0) return p;
        p = next_struct(p);
    }
    return NULL;
}

static const char *str(const BYTE *s, int off)
{
    const struct hdr *h = (const struct hdr *)s;
    const char *p = (const char *)s + h->length;
    int id;

    if (off + 1 > h->length || !(id = s[off])) return "";
    while (--id && *p) p += strlen(p) + 1;
    return *p ? p : "";
}

static void show_wmi(IWbemServices *svc, const WCHAR *query, const WCHAR *const *props)
{
    IEnumWbemClassObject *e = NULL;
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(query);
    HRESULT hr = IWbemServices_ExecQuery(svc, lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &e);
    int rows = 0;

    SysFreeString(q);
    SysFreeString(lang);
    if (FAILED(hr))
    {
        wprintf(L"  WMI query failed 0x%08lx: %s\n", hr, query);
        return;
    }
    for (;;)
    {
        IWbemClassObject *obj = NULL;
        ULONG got = 0;
        int i;

        hr = IEnumWbemClassObject_Next(e, WBEM_INFINITE, 1, &obj, &got);
        if (hr != WBEM_S_NO_ERROR || !got) break;
        wprintf(L"  WMI row %d:\n", rows++);
        for (i = 0; props[i]; i++)
        {
            VARIANT v;

            VariantInit(&v);
            if (SUCCEEDED(IWbemClassObject_Get(obj, props[i], 0, &v, NULL, NULL)))
            {
                VARIANT s;

                VariantInit(&s);
                if (V_VT(&v) == VT_NULL || V_VT(&v) == VT_EMPTY)
                    wprintf(L"    %-24s = (null)\n", props[i]);
                else if (SUCCEEDED(VariantChangeType(&s, &v, 0, VT_BSTR)) && V_BSTR(&s))
                    wprintf(L"    %-24s = \"%s\"\n", props[i], V_BSTR(&s));
                else
                    wprintf(L"    %-24s = (vt %u)\n", props[i], V_VT(&v));
                VariantClear(&s);
            }
            else
                wprintf(L"    %-24s = (property not present)\n", props[i]);
            VariantClear(&v);
        }
        IWbemClassObject_Release(obj);
    }
    wprintf(L"  WMI rows = %d\n", rows);
    IEnumWbemClassObject_Release(e);
}

int wmain(void)
{
    IWbemLocator *loc = NULL;
    IWbemServices *svc = NULL;
    BSTR ns;
    HRESULT hr;
    const BYTE *p;
    int i;

    g_len = GetSystemFirmwareTable('RSMB', 0, NULL, 0);
    wprintf(L"SMBIOS: GetSystemFirmwareTable(RSMB) length = %lu\n", g_len);
    if (g_len > 8)
    {
        g_buf = HeapAlloc(GetProcessHeap(), 0, g_len);
        if (g_buf && GetSystemFirmwareTable('RSMB', 0, g_buf, g_len) == g_len)
        {
            DWORD tlen = *(DWORD *)(g_buf + 4);

            g_tab = g_buf + 8;
            g_len = tlen < g_len - 8 ? tlen : g_len - 8;
            wprintf(L"SMBIOS: version %u.%u, table length %lu\n", g_buf[1], g_buf[2], g_len);
        }
        else
        {
            g_tab = NULL;
        }
    }
    if (!g_tab)
        wprintf(L"SMBIOS: no table available from the OS (all WMI firmware fields must then be empty)\n");

    if (g_tab)
    {
        int n = 0;

        for (p = g_tab; p + sizeof(struct hdr) <= g_tab + g_len; p = next_struct(p))
        {
            const struct hdr *h = (const struct hdr *)p;

            if (h->length < sizeof(*h)) break;
            wprintf(L"SMBIOS: struct #%d type %u length %u\n", n++, h->type, h->length);
            if (h->type == 127) break;
        }
        if ((p = find(0, 0)))
            wprintf(L"\nSMBIOS type 0 BIOS: vendor=\"%S\" version=\"%S\" date=\"%S\"\n", str(p, 4), str(p, 5), str(p, 8));
        else
            wprintf(L"\nSMBIOS type 0 BIOS: absent\n");
        if ((p = find(1, 0)))
            wprintf(L"SMBIOS type 1 System: manufacturer=\"%S\" product=\"%S\" version=\"%S\" serial=\"%S\"\n", str(p, 4),
                    str(p, 5), str(p, 6), str(p, 7));
        else
            wprintf(L"SMBIOS type 1 System: absent\n");
        if ((p = find(3, 0)))
            wprintf(L"SMBIOS type 3 Chassis: manufacturer=\"%S\" chassis type=%u version=\"%S\" serial=\"%S\"\n", str(p, 4),
                    p[5] & 0x7f, str(p, 6), str(p, 7));
        else
            wprintf(L"SMBIOS type 3 Chassis: absent\n");
        for (i = 0; (p = find(17, i)); i++)
        {
            const struct hdr *h = (const struct hdr *)p;
            WORD size = h->length >= 0x0e ? *(WORD *)(p + 0x0c) : 0;
            WORD speed = h->length >= 0x17 ? *(WORD *)(p + 0x15) : 0;
            WORD cfg = h->length >= 0x22 ? *(WORD *)(p + 0x20) : 0;

            wprintf(L"SMBIOS type 17 #%d: size=0x%04x form=%u type=%u speed=%u configured=%u locator=\"%S\" bank=\"%S\" "
                    L"manufacturer=\"%S\" part=\"%S\" serial=\"%S\" (struct length %u)\n",
                    i, size, h->length >= 0x0f ? p[0x0e] : 0, h->length >= 0x13 ? p[0x12] : 0, speed, cfg,
                    h->length >= 0x11 ? str(p, 0x10) : "", h->length >= 0x12 ? str(p, 0x11) : "",
                    h->length >= 0x18 ? str(p, 0x17) : "", h->length >= 0x1b ? str(p, 0x1a) : "",
                    h->length >= 0x19 ? str(p, 0x18) : "", h->length);
        }
        if (i == 0)
            wprintf(L"SMBIOS type 17 Memory Device: none\n");
    }

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    hr = CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void **)&loc);
    if (FAILED(hr))
    {
        wprintf(L"CoCreateInstance(WbemLocator) 0x%08lx\n", hr);
        return 2;
    }
    ns = SysAllocString(L"ROOT\\CIMV2");
    hr = IWbemLocator_ConnectServer(loc, ns, NULL, NULL, NULL, 0, NULL, NULL, &svc);
    SysFreeString(ns);
    if (FAILED(hr))
    {
        wprintf(L"ConnectServer 0x%08lx\n", hr);
        return 3;
    }
    {
        static const WCHAR *const bios[] = { L"Manufacturer", L"Name", L"Description", L"Version", L"SMBIOSBIOSVersion",
                                              L"SerialNumber", L"ReleaseDate", L"SMBIOSMajorVersion", L"SMBIOSMinorVersion", NULL };
        static const WCHAR *const cs[] = { L"Manufacturer", L"Model", NULL };
        static const WCHAR *const csp[] = { L"Vendor", L"Name", L"Version", L"IdentifyingNumber", L"UUID", NULL };
        static const WCHAR *const enc[] = { L"Manufacturer", L"SerialNumber", L"Tag", L"ChassisTypes", NULL };
        static const WCHAR *const mem[] = { L"BankLabel", L"DeviceLocator", L"Manufacturer", L"PartNumber", L"SerialNumber",
                                             L"Capacity", L"Speed", L"ConfiguredClockSpeed", L"FormFactor", L"MemoryType", NULL };

        wprintf(L"\n== Win32_BIOS ==\n");
        show_wmi(svc, L"SELECT * FROM Win32_BIOS", bios);
        wprintf(L"\n== Win32_ComputerSystem ==\n");
        show_wmi(svc, L"SELECT * FROM Win32_ComputerSystem", cs);
        wprintf(L"\n== Win32_ComputerSystemProduct ==\n");
        show_wmi(svc, L"SELECT * FROM Win32_ComputerSystemProduct", csp);
        wprintf(L"\n== Win32_SystemEnclosure ==\n");
        show_wmi(svc, L"SELECT * FROM Win32_SystemEnclosure", enc);
        wprintf(L"\n== Win32_PhysicalMemory ==\n");
        show_wmi(svc, L"SELECT * FROM Win32_PhysicalMemory", mem);
    }
    IWbemServices_Release(svc);
    IWbemLocator_Release(loc);
    CoUninitialize();
    wprintf(L"\nsmbiosprobe done\n");
    return 0;
}
