/*
 * Performance Data Helper Declarations
 */

#ifndef _PDH_PERFDATA_H_
#define _PDH_PERFDATA_H_

#include <ntstatus.h>
#include <windef.h>

/* Processor performance data */
NTSTATUS perf_get_processor_time(
    ULONGLONG *kernel_time,
    ULONGLONG *user_time,
    ULONGLONG *idle_time);

/* Memory information */
NTSTATUS perf_get_available_memory(ULONGLONG *available_pages);
NTSTATUS perf_get_committed_memory(ULONGLONG *committed_pages);
NTSTATUS perf_get_total_memory(ULONGLONG *total_pages);

/* Process and thread counts */
NTSTATUS perf_get_process_count(ULONG *count);
NTSTATUS perf_get_thread_count(ULONG *count);

/* perflib provider helpers */
BOOL perf_query_wants(const WCHAR *query, const DWORD *ids, unsigned count);
void perf_trace(const char *what, DWORD a, DWORD b);

#endif /* _PDH_PERFDATA_H_ */
