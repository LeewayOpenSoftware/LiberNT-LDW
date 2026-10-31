/*
 * PROJECT:     LiberNT ntdll API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     LdrFindResourceEx_U: what was measured on Windows 11 26100 (PROBE-RESULTS.md, group B)
 *              and the honest rejection of the combinations that were not
 */

#include "precomp.h"

#define STATUS_MEASURED_BAD_ARGUMENT ((NTSTATUS)0xC00000F1)

static
NTSTATUS
FindEx(ULONG Flags, PVOID Module, ULONG_PTR *Path, ULONG Count, PIMAGE_RESOURCE_DATA_ENTRY *Entry)
{
    *Entry = NULL;
    return LdrFindResourceEx_U(Flags, Module, Path, Count, Entry);
}

static
void
Test_SameAsFindResource(void)
{
    HMODULE Module = GetModuleHandleW(NULL);
    LDR_RESOURCE_INFO Info;
    ULONG_PTR Path[3];
    PIMAGE_RESOURCE_DATA_ENTRY Entry1 = NULL, Entry2 = NULL;
    NTSTATUS Status1, Status2;

    /* The resource of the older test; present or not, both functions must agree */
    Info.Type = 2; /* RT_BITMAP */
    Info.Name = (ULONG_PTR)L"NORMAL_FRAMECAPTION_BMP";
    Info.Language = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    Path[0] = Info.Type;
    Path[1] = Info.Name;
    Path[2] = Info.Language;

    Status1 = LdrFindResource_U(Module, &Info, 3, &Entry1);
    Status2 = FindEx(0, Module, Path, 3, &Entry2);
    ok_ntstatus(Status2, Status1);
    ok(Entry2 == Entry1, "Entry %p, LdrFindResource_U gave %p\n", Entry2, Entry1);
    ok(Status1 != STATUS_SUCCESS || Entry2 != NULL, "Success without an entry\n");

    /* A name and a type that do not exist: the same errors as LdrFindResource_U */
    Info.Name = 0x7FFE;
    Path[1] = 0x7FFE;
    Status1 = LdrFindResource_U(Module, &Info, 3, &Entry1);
    Status2 = FindEx(0, Module, Path, 3, &Entry2);
    ok_ntstatus(Status2, Status1);
    ok(!NT_SUCCESS(Status2), "A missing name was found\n");
    ok(Entry2 == NULL, "Entry %p for a missing name\n", Entry2);

    Info.Type = 0x7FFE;
    Path[0] = 0x7FFE;
    Info.Name = 1;
    Path[1] = 1;
    Status1 = LdrFindResource_U(Module, &Info, 3, &Entry1);
    Status2 = FindEx(0, Module, Path, 3, &Entry2);
    ok_ntstatus(Status2, Status1);
    ok(!NT_SUCCESS(Status2), "A missing type was found\n");
}

static
void
Test_MeasuredRejections(void)
{
    HMODULE Module = GetModuleHandleW(NULL);
    ULONG_PTR Path[3] = { 2, 1, 0 };
    PIMAGE_RESOURCE_DATA_ENTRY Entry;

    /* Measured: Count 0, 2 and 5 with flags 0, and the alternate-type flag with a Count of 3 */
    ok_ntstatus(FindEx(0, Module, Path, 0, &Entry), STATUS_MEASURED_BAD_ARGUMENT);
    ok_ntstatus(FindEx(0, Module, Path, 2, &Entry), STATUS_MEASURED_BAD_ARGUMENT);
    ok_ntstatus(FindEx(0, Module, Path, 5, &Entry), STATUS_MEASURED_BAD_ARGUMENT);
    ok_ntstatus(FindEx(1, Module, Path, 3, &Entry), STATUS_MEASURED_BAD_ARGUMENT);
    ok(Entry == NULL, "Entry %p after a rejection\n", Entry);
}

/*
 * NOT measured, or measured only as 'succeeds' without the result: LiberNT rejects instead of
 * inventing a success. These expectations are LiberNT's, Windows 11 accepts the short path of flag 2.
 */
static
void
Test_HonestRejections(void)
{
    HMODULE Module = GetModuleHandleW(NULL);
    ULONG_PTR Path[4] = { 2, 1, 0, 0 };
    PIMAGE_RESOURCE_DATA_ENTRY Entry;

    ok_ntstatus(FindEx(2, Module, Path, 2, &Entry), STATUS_NOT_IMPLEMENTED);
    ok_ntstatus(FindEx(2, Module, Path, 3, &Entry), STATUS_NOT_IMPLEMENTED);
    ok_ntstatus(FindEx(0x10, Module, Path, 3, &Entry), STATUS_NOT_IMPLEMENTED);
    ok_ntstatus(FindEx(0, Module, Path, 1, &Entry), STATUS_NOT_IMPLEMENTED);
    ok_ntstatus(FindEx(0, Module, Path, 4, &Entry), STATUS_NOT_IMPLEMENTED);
    ok_ntstatus(FindEx(1, Module, Path, 4, &Entry), STATUS_NOT_IMPLEMENTED);
    ok(Entry == NULL, "Entry %p after a rejection\n", Entry);
}

static
void
Test_NullPath(void)
{
    HMODULE Module = GetModuleHandleW(NULL);
    PIMAGE_RESOURCE_DATA_ENTRY Entry = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    /* Measured: a NULL path raises an access violation, it is not turned into a status */
    _SEH2_TRY
    {
        Status = LdrFindResourceEx_U(0, Module, NULL, 3, &Entry);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    ok_ntstatus(Status, STATUS_ACCESS_VIOLATION);
}

START_TEST(LdrFindResourceEx_U)
{
    Test_SameAsFindResource();
    Test_MeasuredRejections();
    Test_HonestRejections();
    Test_NullPath();
}
