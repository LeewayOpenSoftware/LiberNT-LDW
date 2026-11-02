/*
 * PerfOS Provider - System, Memory, Processor performance objects
 * Implements Windows-compatible perflib provider interface
 * Objects: System (2), Memory (4), Processor (238)
 * Uses shared measurement helpers from perfdata.c for real kernel data
 */

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <stdlib.h>
#include <string.h>
#include "windef.h"
#include "winbase.h"
#include <winperf.h>
#include "winternl.h"

#define NDEBUG

BOOL perf_query_wants(const WCHAR *query, const DWORD *ids, unsigned count);
void perf_trace(const char *what, DWORD a, DWORD b);
#include <debug.h>

/* Forward declarations - shared measurement helpers from perfdata.c */
extern NTSTATUS perf_get_processor_time(ULONGLONG *kernel_time, ULONGLONG *user_time, ULONGLONG *idle_time);
extern NTSTATUS perf_get_available_memory(ULONGLONG *available_pages);
extern NTSTATUS perf_get_committed_memory(ULONGLONG *committed_pages);
extern NTSTATUS perf_get_total_memory(ULONGLONG *total_pages);

static DWORD g_dwOpenCount = 0;

DWORD APIENTRY OpenPerfOS(LPWSTR lpDeviceNames)
{
    g_dwOpenCount++;
    perf_trace("Open", g_dwOpenCount, 0);
    return ERROR_SUCCESS;
}

DWORD APIENTRY ClosePerfOS(void)
{
    if (g_dwOpenCount > 0)
        g_dwOpenCount--;
    return ERROR_SUCCESS;
}

/* Helper: Initialize a counter definition structure */
static void init_counter_def(
    PERF_COUNTER_DEFINITION *def,
    DWORD counter_id,
    DWORD counter_type,
    DWORD counter_size,
    DWORD counter_offset)
{
    def->ByteLength = sizeof(PERF_COUNTER_DEFINITION);
    def->CounterNameTitleIndex = counter_id;
    def->CounterNameTitle = 0;
    def->CounterHelpTitleIndex = counter_id + 1;
    def->CounterHelpTitle = 0;
    def->DefaultScale = 0;
    def->DetailLevel = PERF_DETAIL_NOVICE;
    def->CounterType = counter_type;
    def->CounterSize = counter_size;
    def->CounterOffset = counter_offset;
}

DWORD APIENTRY CollectPerfOSData(LPWSTR lpValueName, LPVOID *lppData, LPDWORD lpcbData, LPDWORD lpNumObjectTypes)
{
    LARGE_INTEGER perf_time, perf_freq;
    PERF_OBJECT_TYPE *pobj;
    PERF_COUNTER_DEFINITION *pcnt;
    PERF_COUNTER_BLOCK *pcblk;
    ULONGLONG kernel_time, user_time, idle_time;
    ULONGLONG avail_mem, committed_mem, total_mem;
    DWORD offset, obj_offset;
    BYTE *p;
    ULONGLONG *pullData;
    DWORD *pdwData;

    if (!lppData || !lpcbData || !lpNumObjectTypes)
        return ERROR_INVALID_PARAMETER;

    perf_trace("Collect called buffer", *lpcbData, lpValueName ? lpValueName[0] : 0);
    {
        static const DWORD ours[] = { 2, 4, 238 };

        if (!perf_query_wants(lpValueName, ours, 3))
        {
            *lpcbData = 0;
            *lpNumObjectTypes = 0;
            return ERROR_SUCCESS;
        }
    }

    /* Require minimum buffer size */
    if (!*lppData || *lpcbData < 4096)
        return ERROR_MORE_DATA;

    /* Collect all kernel data first */
    if (!NT_SUCCESS(perf_get_processor_time(&kernel_time, &user_time, &idle_time)) ||
        !NT_SUCCESS(perf_get_available_memory(&avail_mem)) ||
        !NT_SUCCESS(perf_get_committed_memory(&committed_mem)) ||
        !NT_SUCCESS(perf_get_total_memory(&total_mem)))
        return ERROR_NOT_READY;

    /* perflib contract: write PERF_OBJECT_TYPE blocks at *lppData; the PERF_DATA_BLOCK header belongs to the caller */
    p = (BYTE *)*lppData;
    NtQueryPerformanceCounter(&perf_time, &perf_freq);

    offset = 0;

    /* ===== SYSTEM OBJECT (2) ===== */
    obj_offset = offset;
    pobj = (PERF_OBJECT_TYPE *)(p + offset);

    pobj->TotalByteLength = 0;
    pobj->DefinitionLength = 0;
    pobj->HeaderLength = sizeof(PERF_OBJECT_TYPE);
    pobj->ObjectNameTitleIndex = 2;
    pobj->ObjectNameTitle = 0;
    pobj->ObjectHelpTitleIndex = 3;
    pobj->ObjectHelpTitle = 0;
    pobj->DetailLevel = PERF_DETAIL_NOVICE;
    pobj->NumCounters = 1;
    pobj->DefaultCounter = -1;
    pobj->NumInstances = PERF_NO_INSTANCES;
    pobj->CodePage = 0;
    pobj->PerfTime.QuadPart = perf_time.QuadPart;
    pobj->PerfFreq.QuadPart = perf_freq.QuadPart;

    offset += sizeof(PERF_OBJECT_TYPE);

    /* System counter: System Up Time (counter ID 674) */
    pcnt = (PERF_COUNTER_DEFINITION *)(p + offset);
    init_counter_def(pcnt, 674, PERF_ELAPSED_TIME, sizeof(ULONGLONG), sizeof(PERF_COUNTER_BLOCK));

    offset += sizeof(PERF_COUNTER_DEFINITION);
    pobj->DefinitionLength = sizeof(PERF_OBJECT_TYPE) + sizeof(PERF_COUNTER_DEFINITION);

    /* System counter block with data */
    pcblk = (PERF_COUNTER_BLOCK *)(p + offset);
    pcblk->ByteLength = sizeof(PERF_COUNTER_BLOCK) + sizeof(ULONGLONG);

    pullData = (ULONGLONG *)(pcblk + 1);
    /* PERF_ELAPSED_TIME: the counter holds the start time; a consumer computes (PerfTime - data) / PerfFreq */
    *pullData = perf_time.QuadPart - (ULONGLONG)((double)GetTickCount64() * (double)perf_freq.QuadPart / 1000.0);

    offset += pcblk->ByteLength;
    offset = (offset + 7) & ~7u;
    pobj->TotalByteLength = offset - obj_offset;

    /* ===== MEMORY OBJECT (4) ===== */
    obj_offset = offset;
    pobj = (PERF_OBJECT_TYPE *)(p + offset);

    pobj->TotalByteLength = 0;
    pobj->DefinitionLength = 0;
    pobj->HeaderLength = sizeof(PERF_OBJECT_TYPE);
    pobj->ObjectNameTitleIndex = 4;
    pobj->ObjectNameTitle = 0;
    pobj->ObjectHelpTitleIndex = 5;
    pobj->ObjectHelpTitle = 0;
    pobj->DetailLevel = PERF_DETAIL_NOVICE;
    pobj->NumCounters = 2;
    pobj->DefaultCounter = -1;
    pobj->NumInstances = PERF_NO_INSTANCES;
    pobj->CodePage = 0;
    pobj->PerfTime.QuadPart = perf_time.QuadPart;
    pobj->PerfFreq.QuadPart = perf_freq.QuadPart;

    offset += sizeof(PERF_OBJECT_TYPE);

    /* Counter 1: Available MBytes (counter ID 6490) */
    pcnt = (PERF_COUNTER_DEFINITION *)(p + offset);
    init_counter_def(pcnt, 6490, PERF_COUNTER_RAWCOUNT, sizeof(DWORD), sizeof(PERF_COUNTER_BLOCK));

    offset += sizeof(PERF_COUNTER_DEFINITION);

    /* Counter 2: Committed Bytes (counter ID 6492) */
    pcnt = (PERF_COUNTER_DEFINITION *)(p + offset);
    init_counter_def(pcnt, 6492, PERF_COUNTER_LARGE_RAWCOUNT, sizeof(ULONGLONG),
                     sizeof(PERF_COUNTER_BLOCK) + sizeof(DWORD));

    offset += sizeof(PERF_COUNTER_DEFINITION);
    pobj->DefinitionLength = sizeof(PERF_OBJECT_TYPE) + 2 * sizeof(PERF_COUNTER_DEFINITION);

    /* Memory counter block with data */
    pcblk = (PERF_COUNTER_BLOCK *)(p + offset);
    pcblk->ByteLength = sizeof(PERF_COUNTER_BLOCK) + sizeof(DWORD) + sizeof(ULONGLONG);

    pdwData = (DWORD *)(pcblk + 1);
    *pdwData = (DWORD)(avail_mem * 4096 / (1024 * 1024));  /* Convert pages to MBytes */

    pullData = (ULONGLONG *)(pdwData + 1);
    *pullData = committed_mem * 4096;  /* Convert pages to bytes */

    offset += pcblk->ByteLength;
    offset = (offset + 7) & ~7u;
    pobj->TotalByteLength = offset - obj_offset;

    /* ===== PROCESSOR OBJECT (238) ===== */
    obj_offset = offset;
    pobj = (PERF_OBJECT_TYPE *)(p + offset);

    pobj->TotalByteLength = 0;
    pobj->DefinitionLength = 0;
    pobj->HeaderLength = sizeof(PERF_OBJECT_TYPE);
    pobj->ObjectNameTitleIndex = 238;
    pobj->ObjectNameTitle = 0;
    pobj->ObjectHelpTitleIndex = 239;
    pobj->ObjectHelpTitle = 0;
    pobj->DetailLevel = PERF_DETAIL_NOVICE;
    pobj->NumCounters = 1;
    pobj->DefaultCounter = -1;
    pobj->NumInstances = PERF_NO_INSTANCES;
    pobj->CodePage = 0;
    pobj->PerfTime.QuadPart = perf_time.QuadPart;
    pobj->PerfFreq.QuadPart = perf_freq.QuadPart;

    offset += sizeof(PERF_OBJECT_TYPE);

    /* Counter: % Processor Time (counter ID 6) */
    pcnt = (PERF_COUNTER_DEFINITION *)(p + offset);
    init_counter_def(pcnt, 6, PERF_100NSEC_TIMER_INV, sizeof(ULONGLONG), sizeof(PERF_COUNTER_BLOCK));

    offset += sizeof(PERF_COUNTER_DEFINITION);
    pobj->DefinitionLength = sizeof(PERF_OBJECT_TYPE) + sizeof(PERF_COUNTER_DEFINITION);

    /* Processor counter block with data */
    pcblk = (PERF_COUNTER_BLOCK *)(p + offset);
    pcblk->ByteLength = sizeof(PERF_COUNTER_BLOCK) + sizeof(ULONGLONG);

    pullData = (ULONGLONG *)(pcblk + 1);
    /* PERF_100NSEC_TIMER_INV: the counter holds idle time; % busy = 100 * (1 - delta idle / delta time) */
    *pullData = idle_time;

    offset += pcblk->ByteLength;
    offset = (offset + 7) & ~7u;
    pobj->TotalByteLength = offset - obj_offset;

    /* Report the data written and advance the caller's pointer past it */
    *lppData = p + offset;
    *lpNumObjectTypes = 3;
    *lpcbData = offset;
    perf_trace("Collect done objects", 3, offset);

    return ERROR_SUCCESS;
}

/* DLL Entry Point */
BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved)
{
    (void)hInstance;
    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH:
        case DLL_PROCESS_DETACH:
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            break;
    }
    return TRUE;
}
