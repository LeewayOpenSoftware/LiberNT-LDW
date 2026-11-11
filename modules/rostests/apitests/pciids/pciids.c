/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the PCI hardware/compatible ID ladder
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define UNICODE
#define _UNICODE

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <winreg.h>
#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>
#include <cfgmgr32.h>

#define MAX_IDS 16
#define ID_BUFFER_CCH 4096

static
BOOL
ParseHexField(PCWSTR *Cursor, PCWSTR Key, PULONG Value, ULONG Width)
{
    ULONG i, Digit, Result = 0;
    PCWSTR p = *Cursor;

    if (wcsncmp(p, Key, wcslen(Key)) != 0)
        return FALSE;
    p += wcslen(Key);

    for (i = 0; i < Width; i++)
    {
        if (p[i] >= L'0' && p[i] <= L'9')
            Digit = p[i] - L'0';
        else if (p[i] >= L'A' && p[i] <= L'F')
            Digit = p[i] - L'A' + 10;
        else
            return FALSE;
        Result = (Result << 4) | Digit;
    }

    *Value = Result;
    *Cursor = p + Width;
    return TRUE;
}

static
BOOL
ParsePciId(PCWSTR Id,
           PULONG Ven, PULONG Dev,
           PULONG Subsys, PULONG Rev,
           PULONG Cc, PULONG Sc, PULONG Pi,
           PBOOL HasSubsys, PBOOL HasCc)
{
    PCWSTR p = Id;

    *HasSubsys = FALSE;
    *HasCc = FALSE;
    *Subsys = 0;
    *Rev = 0;
    *Cc = 0;
    *Sc = 0;
    *Pi = 0;

    if (wcsncmp(p, L"PCI\\VEN_", 8) != 0)
        return FALSE;
    p += 4;

    if (!ParseHexField(&p, L"VEN_", Ven, 4))
        return FALSE;
    if (p[0] != L'&')
        return FALSE;
    p++;
    if (!ParseHexField(&p, L"DEV_", Dev, 4))
        return FALSE;

    while (p[0] == L'&')
    {
        p++;
        if (wcsncmp(p, L"SUBSYS_", 7) == 0)
        {
            if (!ParseHexField(&p, L"SUBSYS_", Subsys, 8))
                return FALSE;
            *HasSubsys = TRUE;
        }
        else if (wcsncmp(p, L"REV_", 4) == 0)
        {
            if (!ParseHexField(&p, L"REV_", Rev, 2))
                return FALSE;
        }
        else if (wcsncmp(p, L"CC_", 3) == 0)
        {
            ULONG Raw;
            if (!ParseHexField(&p, L"CC_", &Raw, 4))
                return FALSE;
            *Cc = (Raw >> 8) & 0xFF;
            *Sc = Raw & 0xFF;
            *HasCc = TRUE;
            if (p[0] == L'&')
            {
                p++;
                if (!ParseHexField(&p, L"REV_", Rev, 2))
                    return FALSE;
            }
        }
        else
        {
            return FALSE;
        }
    }

    return p[0] == UNICODE_NULL;
}

static
VOID
BuildExpectedIds(PCWSTR Expected[MAX_IDS], PULONG Count,
                 ULONG Ven, ULONG Dev, ULONG Subsys, ULONG Rev,
                 ULONG Cc, ULONG Sc, ULONG Pi)
{
    WCHAR Buffer[64];
    ULONG n = 0;

    _snwprintf(Buffer, RTL_NUMBER_OF(Buffer),
               L"PCI\\VEN_%04X&DEV_%04X&SUBSYS_%08X&REV_%02X", Ven, Dev, Subsys, Rev);
    Expected[n++] = _wcsdup(Buffer);
    _snwprintf(Buffer, RTL_NUMBER_OF(Buffer),
               L"PCI\\VEN_%04X&DEV_%04X&SUBSYS_%08X", Ven, Dev, Subsys);
    Expected[n++] = _wcsdup(Buffer);
    _snwprintf(Buffer, RTL_NUMBER_OF(Buffer),
               L"PCI\\VEN_%04X&DEV_%04X&CC_%02X%02X%02X", Ven, Dev, Cc, Sc, Pi);
    Expected[n++] = _wcsdup(Buffer);
    _snwprintf(Buffer, RTL_NUMBER_OF(Buffer),
               L"PCI\\VEN_%04X&DEV_%04X&CC_%02X%02X", Ven, Dev, Cc, Sc);
    Expected[n++] = _wcsdup(Buffer);
    _snwprintf(Buffer, RTL_NUMBER_OF(Buffer),
               L"PCI\\VEN_%04X&DEV_%04X&REV_%02X", Ven, Dev, Rev);
    Expected[n++] = _wcsdup(Buffer);
    _snwprintf(Buffer, RTL_NUMBER_OF(Buffer),
               L"PCI\\VEN_%04X&DEV_%04X", Ven, Dev);
    Expected[n++] = _wcsdup(Buffer);
    _snwprintf(Buffer, RTL_NUMBER_OF(Buffer),
               L"PCI\\VEN_%04X&CC_%02X%02X%02X", Ven, Cc, Sc, Pi);
    Expected[n++] = _wcsdup(Buffer);
    _snwprintf(Buffer, RTL_NUMBER_OF(Buffer),
               L"PCI\\VEN_%04X&CC_%02X%02X", Ven, Cc, Sc);
    Expected[n++] = _wcsdup(Buffer);
    _snwprintf(Buffer, RTL_NUMBER_OF(Buffer), L"PCI\\VEN_%04X", Ven);
    Expected[n++] = _wcsdup(Buffer);
    _snwprintf(Buffer, RTL_NUMBER_OF(Buffer),
               L"PCI\\CC_%02X%02X%02X", Cc, Sc, Pi);
    Expected[n++] = _wcsdup(Buffer);
    _snwprintf(Buffer, RTL_NUMBER_OF(Buffer), L"PCI\\CC_%02X%02X", Cc, Sc);
    Expected[n++] = _wcsdup(Buffer);

    *Count = n;
}

static
ULONG
LoadIdList(DEVINST DevInst, ULONG Property, WCHAR Buffer[ID_BUFFER_CCH])
{
    ULONG Length = sizeof(WCHAR) * ID_BUFFER_CCH;
    CONFIGRET cr;

    Buffer[0] = UNICODE_NULL;
    cr = CM_Get_DevNode_Registry_PropertyW(DevInst, Property, NULL,
                                           Buffer, &Length, 0);
    if (cr != CR_SUCCESS)
        return 0;
    return Length / sizeof(WCHAR);
}

static
VOID
CheckLadderOrder(PCWSTR Ids, PCWSTR Expected[], ULONG ExpectedCount,
                 PCWSTR What, PCWSTR DevInstId)
{
    PCWSTR Cursor = Ids;
    ULONG Found = 0, i;

    while (*Cursor && Found < ExpectedCount)
    {
        if (wcscmp(Cursor, Expected[Found]) == 0)
            Found++;
        Cursor += wcslen(Cursor) + 1;
    }

    for (i = Found; i < ExpectedCount; i++)
        ok(0, "%ls: %ls ladder is missing '%ls'\n", DevInstId, What, Expected[i]);

    if (Found == ExpectedCount)
        trace("%ls: %ls ladder matches the documented order\n", DevInstId, What);
}

START_TEST(PciIds)
{
    WCHAR IdList[16384];
    WCHAR HwBuffer[ID_BUFFER_CCH], CompatBuffer[ID_BUFFER_CCH];
    ULONG ListLength = RTL_NUMBER_OF(IdList);
    ULONG Ven, Dev, Subsys, Rev, Cc, Sc, Pi;
    BOOL HasSubsys, HasCc, SeenCc;
    PCWSTR Expected[MAX_IDS];
    ULONG ExpectedCount, Checked = 0;
    CONFIGRET cr;
    PWSTR Cursor;
    DEVINST DevInst;
    ULONG i;

    cr = CM_Get_Device_ID_ListW(L"PCI", IdList, ListLength, CM_GETIDLIST_FILTER_ENUMERATOR);
    ok(cr == CR_SUCCESS, "CM_Get_Device_ID_ListW returned %lu\n", cr);
    if (cr != CR_SUCCESS)
        return;

    Cursor = IdList;
    while (*Cursor)
    {
        cr = CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)Cursor, CM_LOCATE_DEVNODE_NORMAL);
        if (cr == CR_SUCCESS && LoadIdList(DevInst, CM_DRP_HARDWAREID, HwBuffer) > 0)
        {
            if (ParsePciId(HwBuffer, &Ven, &Dev, &Subsys, &Rev, &Cc, &Sc, &Pi, &HasSubsys, &HasCc) &&
                HasSubsys)
            {
                PCWSTR HwCursor;

                /* The class code lives in the CC hardware ID. */
                SeenCc = FALSE;
                HwCursor = HwBuffer;
                while (*HwCursor)
                {
                    if (ParsePciId(HwCursor, &Ven, &Dev, &Subsys, &Rev, &Cc, &Sc, &Pi,
                                   &HasSubsys, &HasCc) && HasCc)
                    {
                        SeenCc = TRUE;
                        break;
                    }
                    HwCursor += wcslen(HwCursor) + 1;
                }

                if (SeenCc)
                {
                    BuildExpectedIds(Expected, &ExpectedCount, Ven, Dev, Subsys, Rev, Cc, Sc, Pi);

                    /* Hardware IDs: the SUBSYS and CC forms, in order. */
                    CheckLadderOrder(HwBuffer, Expected, 4, L"hardware IDs", Cursor);

                    /* Compatible IDs: DEV&REV, DEV, VEN&CC, VEN&CC(2), VEN, CC, CC(2). */
                    if (LoadIdList(DevInst, CM_DRP_COMPATIBLEIDS, CompatBuffer) > 0)
                        CheckLadderOrder(CompatBuffer, &Expected[4], ExpectedCount - 4,
                                         L"compatible IDs", Cursor);

                    for (i = 0; i < ExpectedCount; i++)
                        free((void *)Expected[i]);

                    Checked++;
                }
            }
        }
        Cursor += wcslen(Cursor) + 1;
    }

    ok(Checked > 0, "no PCI device with a subsystem-enumerated hardware ID was found\n");
}
