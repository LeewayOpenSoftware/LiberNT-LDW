/*
 * Performance Data Helpers - Shared measurement code for PDH and perflib providers
 * Used by both PDH counter callbacks and perflib provider DLLs (PerfOS, PerfProc)
 */

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include "windef.h"
#include "winbase.h"
#include <winperf.h>
#include <psapi.h>
#include "winternl.h"

#define NDEBUG
#include <debug.h>

/* Get processor performance data (time-based) */
NTSTATUS perf_get_processor_time(
    ULONGLONG *kernel_time,
    ULONGLONG *user_time,
    ULONGLONG *idle_time)
{
    SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION *performance;
    SYSTEM_INFO info;
    ULONG count, returned = 0;
    NTSTATUS status;
    ULONGLONG total_kernel = 0, total_user = 0, total_idle = 0;
    int i;

    GetSystemInfo(&info);
    count = info.dwNumberOfProcessors;
    if (!count)
        return STATUS_INSUFFICIENT_RESOURCES;

    performance = malloc(count * sizeof(*performance));
    if (!performance)
        return STATUS_NO_MEMORY;

    status = NtQuerySystemInformation(
        SystemProcessorPerformanceInformation,
        performance,
        count * sizeof(*performance),
        &returned);

    if (status >= 0)
    {
        for (i = 0; i < count; i++)
        {
            total_kernel += performance[i].KernelTime.QuadPart;
            total_user += performance[i].UserTime.QuadPart;
            total_idle += performance[i].IdleTime.QuadPart;
        }
        *kernel_time = total_kernel;
        *user_time = total_user;
        *idle_time = total_idle;
    }

    free(performance);
    return status;
}

/* Get available memory in pages */
NTSTATUS perf_get_available_memory(ULONGLONG *available_pages)
{
    PERFORMANCE_INFORMATION perf_info;

    if (!GetPerformanceInfo(&perf_info, sizeof(perf_info)))
        return STATUS_UNSUCCESSFUL;

    *available_pages = perf_info.PhysicalAvailable;
    return STATUS_SUCCESS;
}

/* Get committed memory in pages */
NTSTATUS perf_get_committed_memory(ULONGLONG *committed_pages)
{
    PERFORMANCE_INFORMATION perf_info;

    if (!GetPerformanceInfo(&perf_info, sizeof(perf_info)))
        return STATUS_UNSUCCESSFUL;

    *committed_pages = perf_info.CommitTotal;
    return STATUS_SUCCESS;
}

/* Get total physical memory */
NTSTATUS perf_get_total_memory(ULONGLONG *total_pages)
{
    PERFORMANCE_INFORMATION perf_info;

    if (!GetPerformanceInfo(&perf_info, sizeof(perf_info)))
        return STATUS_UNSUCCESSFUL;

    *total_pages = perf_info.PhysicalTotal;
    return STATUS_SUCCESS;
}

/* Get process count */
NTSTATUS perf_get_process_count(ULONG *count)
{
    PERFORMANCE_INFORMATION perf_info;

    if (!GetPerformanceInfo(&perf_info, sizeof(perf_info)))
        return STATUS_UNSUCCESSFUL;

    *count = perf_info.ProcessCount;
    return STATUS_SUCCESS;
}

/* Get thread count */
NTSTATUS perf_get_thread_count(ULONG *count)
{
    PERFORMANCE_INFORMATION perf_info;

    if (!GetPerformanceInfo(&perf_info, sizeof(perf_info)))
        return STATUS_UNSUCCESSFUL;

    *count = perf_info.ThreadCount;
    return STATUS_SUCCESS;
}

/*
 * Does the perflib query string ask for one of our object ids?
 * "Global" (or an empty query) means everything; "Foreign"/"Costly" nothing of ours;
 * otherwise a list of decimal object ids separated by spaces.
 */
BOOL perf_query_wants(const WCHAR *query, const DWORD *ids, unsigned count)
{
    unsigned i;

    if (!query || !query[0] || !_wcsicmp(query, L"Global"))
        return TRUE;
    if (!_wcsicmp(query, L"Foreign") || !_wcsicmp(query, L"Costly"))
        return FALSE;

    while (*query)
    {
        DWORD value = 0;
        BOOL digits = FALSE;

        while (*query == L' ')
            query++;
        while (*query >= L'0' && *query <= L'9')
        {
            value = value * 10 + (*query++ - L'0');
            digits = TRUE;
        }
        if (!digits)
        {
            if (*query)
                query++;
            continue;
        }
        for (i = 0; i < count; i++)
            if (ids[i] == value)
                return TRUE;
    }
    return FALSE;
}

/* Append one line to the perf trace file so a VM run can prove which provider entry points ran */
void perf_trace(const char *what, DWORD a, DWORD b)
{
    char line[160];
    DWORD written;
    int len;
    HANDLE file;

    /* tracing is opt-in: it writes to disk, which a normal query must not do */
    if (GetFileAttributesW(L"C:\\perf_trace.on") == INVALID_FILE_ATTRIBUTES)
        return;
    file = CreateFileW(L"C:\\perf_trace.txt", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;
    len = _snprintf(line, sizeof(line), "%s a=%lu b=%lu pid=%lu\r\n", what, a, b, GetCurrentProcessId());
    if (len > 0)
        WriteFile(file, line, (DWORD)len, &written, NULL);
    CloseHandle(file);
}
