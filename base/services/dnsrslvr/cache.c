/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS system libraries
 * FILE:        base/services/dnsrslvr/cache.c
 * PURPOSE:     DNS cache functions
 * PROGRAMMER:  Peter Hater
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

static RESOLVER_CACHE DnsCache;
static BOOL DnsCacheInitialized = FALSE;

#define DnsCacheLock()          do { EnterCriticalSection(&DnsCache.Lock); } while (0)
#define DnsCacheUnlock()        do { LeaveCriticalSection(&DnsCache.Lock); } while (0)

VOID
DnsIntCacheInitialize(VOID)
{
    DPRINT("DnsIntCacheInitialize()\n");

    /* Check if we're initialized */
    if (DnsCacheInitialized)
        return;

    /* Initialize the cache lock and namespace list */
    InitializeCriticalSection((LPCRITICAL_SECTION)&DnsCache.Lock);
    InitializeListHead(&DnsCache.RecordList);
    DnsCacheInitialized = TRUE;
}

VOID
DnsIntCacheFree(VOID)
{
    DPRINT("DnsIntCacheFree()\n");

    /* Check if we're initialized */
    if (!DnsCacheInitialized)
        return;

    if (!DnsCache.RecordList.Flink)
        return;

    DnsIntCacheFlush(CACHE_FLUSH_ALL);

    DeleteCriticalSection(&DnsCache.Lock);
    DnsCacheInitialized = FALSE;
}

VOID
DnsIntCacheRemoveEntryItem(PRESOLVER_CACHE_ENTRY CacheEntry)
{
    DPRINT("DnsIntCacheRemoveEntryItem(%p)\n", CacheEntry);

    /* Remove the entry from the list */
    RemoveEntryList(&CacheEntry->CacheLink);

    /* Free record */
    DnsRecordListFree(CacheEntry->Record, DnsFreeRecordList);

    /* Delete us */
    HeapFree(GetProcessHeap(), 0, CacheEntry);
}

DNS_STATUS
DnsIntCacheFlush(
    _In_ ULONG ulFlags)
{
    PLIST_ENTRY Entry, NextEntry;
    PRESOLVER_CACHE_ENTRY CacheEntry;

    DPRINT("DnsIntCacheFlush(%lu)\n", ulFlags);

    /* Lock the cache */
    DnsCacheLock();

    /* Loop every entry */
    Entry = DnsCache.RecordList.Flink;
    while (Entry != &DnsCache.RecordList)
    {
        NextEntry = Entry->Flink;

        /* Get this entry */
        CacheEntry = CONTAINING_RECORD(Entry, RESOLVER_CACHE_ENTRY, CacheLink);

        /* Remove it from list */
        if (((ulFlags & CACHE_FLUSH_HOSTS_FILE_ENTRIES) && (CacheEntry->bHostsFileEntry != FALSE)) ||
            ((ulFlags & CACHE_FLUSH_NON_HOSTS_FILE_ENTRIES) && (CacheEntry->bHostsFileEntry == FALSE)))
            DnsIntCacheRemoveEntryItem(CacheEntry);

        /* Move to the next entry */
        Entry = NextEntry;
    }

    /* Unlock the cache */
    DnsCacheUnlock();

    return ERROR_SUCCESS;
}


DNS_STATUS
DnsIntFlushCacheEntry(
    _In_ LPCWSTR pszName,
    _In_ WORD wType)
{
    PLIST_ENTRY Entry, NextEntry;
    PRESOLVER_CACHE_ENTRY CacheEntry;

    DPRINT("DnsIntFlushCacheEntry(%S %x)\n", pszName, wType);

    /* Lock the cache */
    DnsCacheLock();

    /* Loop every entry */
    Entry = DnsCache.RecordList.Flink;
    while (Entry != &DnsCache.RecordList)
    {
        NextEntry = Entry->Flink;

        /* Get this entry */
        CacheEntry = CONTAINING_RECORD(Entry, RESOLVER_CACHE_ENTRY, CacheLink);

        /* Remove it from the list */
        if ((_wcsicmp(CacheEntry->Name, pszName) == 0) &&
            (CacheEntry->bHostsFileEntry == FALSE))
        {
            if ((wType == DNS_TYPE_ANY) ||
                (CacheEntry->Type == wType))
            {
                DnsIntCacheRemoveEntryItem(CacheEntry);
            }
        }

        /* Move to the next entry */
        Entry = NextEntry;
    }

    /* Unlock the cache */
    DnsCacheUnlock();

    return ERROR_SUCCESS;
}


static
PRESOLVER_CACHE_ENTRY
DnsIntCacheFindEntry(
    _In_ LPCWSTR Name,
    _In_ WORD Type)
{
    ULONGLONG Now = GetTickCount64();
    PRESOLVER_CACHE_ENTRY CacheEntry;
    PLIST_ENTRY NextEntry = DnsCache.RecordList.Flink;

    while (NextEntry != &DnsCache.RecordList)
    {
        CacheEntry = CONTAINING_RECORD(NextEntry, RESOLVER_CACHE_ENTRY, CacheLink);
        NextEntry = NextEntry->Flink;

        if (!CacheEntry->bHostsFileEntry && CacheEntry->Expires <= Now)
        {
            DnsIntCacheRemoveEntryItem(CacheEntry);
            continue;
        }
        if (CacheEntry->Type == Type && _wcsicmp(CacheEntry->Name, Name) == 0)
            return CacheEntry;
    }

    return NULL;
}

DNS_STATUS
DnsIntCacheGetEntryByName(
    LPCWSTR Name,
    WORD wType,
    DWORD dwFlags,
    PDNS_RECORDW *Record)
{
    DNS_STATUS Status = DNS_INFO_NO_RECORDS;
    PRESOLVER_CACHE_ENTRY CacheEntry;

    DPRINT("DnsIntCacheGetEntryByName(%S %hu 0x%lx %p)\n",
           Name, wType, dwFlags, Record);

    *Record = NULL;

    DnsCacheLock();

    CacheEntry = DnsIntCacheFindEntry(Name, wType);
    if (CacheEntry)
    {
        *Record = DnsRecordSetCopyEx(CacheEntry->Record, DnsCharSetUnicode, DnsCharSetUnicode);
        Status = *Record ? ERROR_SUCCESS : ERROR_OUTOFMEMORY;

        /*
         * The entry matches the name without regard to case, but it carries the
         * spelling of whoever cached it first. Answer with the spelling that was
         * asked for, like a fresh lookup does. Equal without case means equal
         * length, so the name is overwritten in place.
         */
        if (*Record)
        {
            PDNS_RECORDW Answer;

            for (Answer = *Record; Answer != NULL; Answer = Answer->pNext)
            {
                if (Answer->pName != NULL && _wcsicmp(Answer->pName, Name) == 0)
                    wcscpy(Answer->pName, Name);
            }
        }
    }

    DnsCacheUnlock();

    return Status;
}

BOOL
DnsIntCacheRemoveEntryByName(LPCWSTR Name)
{
    BOOL Ret = FALSE;
    PRESOLVER_CACHE_ENTRY CacheEntry;
    PLIST_ENTRY NextEntry;

    DPRINT("DnsIntCacheRemoveEntryByName(%S)\n", Name);

    DnsCacheLock();

    NextEntry = DnsCache.RecordList.Flink;
    while (NextEntry != &DnsCache.RecordList)
    {
        CacheEntry = CONTAINING_RECORD(NextEntry, RESOLVER_CACHE_ENTRY, CacheLink);
        NextEntry = NextEntry->Flink;

        if (_wcsicmp(CacheEntry->Name, Name) == 0)
        {
            DnsIntCacheRemoveEntryItem(CacheEntry);
            Ret = TRUE;
        }
    }

    DnsCacheUnlock();

    return Ret;
}

VOID
DnsIntCacheAddEntry(
    _In_ LPCWSTR Name,
    _In_ WORD Type,
    _In_ PDNS_RECORDW Record,
    _In_ BOOL bHostsFileEntry)
{
    PRESOLVER_CACHE_ENTRY Entry;
    PDNS_RECORDW Copy, Tail;
    ULONGLONG Expires = MAXULONGLONG;
    DWORD Ttl = MAXDWORD;
    SIZE_T NameSize;

    DPRINT("DnsIntCacheAddEntry(%S %u %p %u)\n", Name, Type, Record, bHostsFileEntry);

    if (!bHostsFileEntry)
    {
        for (Tail = Record; Tail; Tail = Tail->pNext)
            Ttl = min(Ttl, Tail->dwTtl);
        if (!Ttl)
            return;
        Expires = GetTickCount64() + (ULONGLONG)min(Ttl, 86400) * 1000;
    }

    Copy = DnsRecordSetCopyEx(Record, DnsCharSetUnicode, DnsCharSetUnicode);
    if (!Copy)
        return;

    DnsCacheLock();

    Entry = DnsIntCacheFindEntry(Name, Type);
    if (Entry && Entry->bHostsFileEntry)
    {
        if (bHostsFileEntry)
        {
            for (Tail = Entry->Record; Tail->pNext; Tail = Tail->pNext);
            Tail->pNext = Copy;
        }
        else
        {
            DnsRecordListFree(Copy, DnsFreeRecordList);
        }
        DnsCacheUnlock();
        return;
    }
    if (Entry)
        DnsIntCacheRemoveEntryItem(Entry);

    NameSize = (wcslen(Name) + 1) * sizeof(WCHAR);
    Entry = HeapAlloc(GetProcessHeap(), 0, FIELD_OFFSET(RESOLVER_CACHE_ENTRY, Name) + NameSize);
    if (!Entry)
    {
        DnsRecordListFree(Copy, DnsFreeRecordList);
        DnsCacheUnlock();
        return;
    }

    Entry->bHostsFileEntry = bHostsFileEntry;
    Entry->Record = Copy;
    Entry->Type = Type;
    Entry->Expires = Expires;
    CopyMemory(Entry->Name, Name, NameSize);
    InsertTailList(&DnsCache.RecordList, &Entry->CacheLink);

    DnsCacheUnlock();
}

DNS_STATUS
DnsIntCacheGetEntries(
    _Out_ DNS_CACHE_ENTRY **ppCacheEntries)
{
    PRESOLVER_CACHE_ENTRY CacheEntry;
    PLIST_ENTRY NextEntry;
    PDNS_CACHE_ENTRY pLastEntry = NULL, pNewEntry;

    /* Lock the cache */
    DnsCacheLock();

    *ppCacheEntries = NULL;

    NextEntry = DnsCache.RecordList.Flink;
    while (NextEntry != &DnsCache.RecordList)
    {
        /* Get the Current Entry */
        CacheEntry = CONTAINING_RECORD(NextEntry, RESOLVER_CACHE_ENTRY, CacheLink);

        DPRINT("1 %S %lu\n", CacheEntry->Record->pName, CacheEntry->Record->wType);
        if (CacheEntry->Record->pNext)
        {
            DPRINT("2 %S %lu\n", CacheEntry->Record->pNext->pName, CacheEntry->Record->pNext->wType);
        }

        pNewEntry = midl_user_allocate(sizeof(DNS_CACHE_ENTRY));
        if (pNewEntry == NULL)
        {
            DnsCacheUnlock();
            return ERROR_OUTOFMEMORY;
        }

        pNewEntry->pNext = NULL;
        pNewEntry->pszName = midl_user_allocate((wcslen(CacheEntry->Name) + 1) * sizeof(WCHAR));
        if (pNewEntry->pszName == NULL)
        {
            midl_user_free(pNewEntry);
            DnsCacheUnlock();
            return ERROR_OUTOFMEMORY;
        }

        wcscpy(pNewEntry->pszName, CacheEntry->Name);
        pNewEntry->wType1 = CacheEntry->Type;
        pNewEntry->wType2 = 0;
        pNewEntry->wFlags = 0;

        if (pLastEntry == NULL)
            *ppCacheEntries = pNewEntry;
        else
            pLastEntry->pNext = pNewEntry;
        pLastEntry = pNewEntry;

        NextEntry = NextEntry->Flink;
    }

    /* Release the cache */
    DnsCacheUnlock();

    return ERROR_SUCCESS;
}
