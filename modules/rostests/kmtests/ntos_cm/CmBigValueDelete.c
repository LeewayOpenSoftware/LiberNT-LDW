/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Delete a large (multi-block) registry value through the public API
 *              end to end, the path that bugchecked the dx2400 at cmapi.c:1015.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * A value whose data spans several hive blocks is stored in a multi-block cell.
 * Deleting it runs CmDeleteValueKey, which marks the key node, value list and
 * data cells dirty and asserts they are dirty. HvMarkCellDirty used to derive
 * the dirty range from the cell index alone, so it marked no block for a cell
 * whose header is block-aligned and only the first block of a multi-block cell;
 * on the checked kernel ASSERT(HvIsCellDirty(Hive, Cell)) then raised STOP 0x1E.
 * On a stable hive this exercises the real dirty/flush path; the value must be
 * gone after the delete and a flush. Runs under the SYSTEM-context test driver.
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#define BIG_VALUE_SIZE (256 * 1024)
#define TAG_TEST 'VBmC'

START_TEST(CmBigValueDelete)
{
    UNICODE_STRING KeyName, ValueName;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE KeyHandle = NULL;
    NTSTATUS Status;
    PUCHAR Data;
    PKEY_VALUE_PARTIAL_INFORMATION Partial = NULL;
    ULONG ResultLength = 0, i;
    ULONG PartialSize = FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + BIG_VALUE_SIZE;

    Data = ExAllocatePoolWithTag(PagedPool, BIG_VALUE_SIZE, TAG_TEST);
    Partial = ExAllocatePoolWithTag(PagedPool, PartialSize, TAG_TEST);
    if (!Data || !Partial)
    {
        ok(FALSE, "allocation failed\n");
        goto Cleanup;
    }
    for (i = 0; i < BIG_VALUE_SIZE; i++)
        Data[i] = (UCHAR)(i & 0xFF);

    /* A stable hive (SOFTWARE) so the dirty/flush path runs. */
    RtlInitUnicodeString(&KeyName, L"\\Registry\\Machine\\SOFTWARE\\KmtestCmBigValue");
    InitializeObjectAttributes(&ObjectAttributes, &KeyName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateKey(&KeyHandle, KEY_ALL_ACCESS, &ObjectAttributes, 0, NULL,
                         REG_OPTION_NON_VOLATILE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    RtlInitUnicodeString(&ValueName, L"BigValue");
    Status = ZwSetValueKey(KeyHandle, &ValueName, 0, REG_BINARY, Data, BIG_VALUE_SIZE);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = ZwFlushKey(KeyHandle);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* The delete that bugchecked on the pre-fix checked kernel. */
    Status = ZwDeleteValueKey(KeyHandle, &ValueName);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = ZwFlushKey(KeyHandle);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* The value is gone and stays gone. */
    Status = ZwQueryValueKey(KeyHandle, &ValueName, KeyValuePartialInformation,
                             Partial, PartialSize, &ResultLength);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_NOT_FOUND);

    Status = ZwDeleteKey(KeyHandle);
    ok_eq_hex(Status, STATUS_SUCCESS);

Cleanup:
    if (KeyHandle)
        ZwClose(KeyHandle);
    if (Data)
        ExFreePoolWithTag(Data, TAG_TEST);
    if (Partial)
        ExFreePoolWithTag(Partial, TAG_TEST);
}
