/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         x64 trap handlers
 * PROGRAMMERS:     Timo Kreuzer (timo.kreuzer@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#include <reactos/smpdbg.h>

#define NDEBUG
#include <debug.h>

VOID
KiRetireDpcListInDpcStack(
    PKPRCB Prcb,
    PVOID DpcStack);

NTSTATUS
KiConvertToGuiThread(
    VOID);

_Requires_lock_not_held_(Prcb->PrcbLock)
VOID
NTAPI
KiDpcInterruptHandler(VOID)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    PKTHREAD NewThread, OldThread;
    KIRQL OldIrql;

    if (SmpDbgEnabled)
        SmpDbgDispatchInterrupt(Prcb->Number);

    /* Raise to DISPATCH_LEVEL */
    OldIrql = KfRaiseIrql(DISPATCH_LEVEL);

    /* Send an EOI */
    KiSendEOI();

    /* Check for pending timers, pending DPCs, or pending ready threads */
    if ((Prcb->DpcData[0].DpcQueueDepth) ||
        (Prcb->TimerRequest) ||
        (Prcb->DeferredReadyListHead.Next))
    {
        /* Retire DPCs while under the DPC stack */
        KiRetireDpcListInDpcStack(Prcb, Prcb->DpcStack);
    }

    /* Enable interrupts */
    _enable();

    /* Check for quantum end */
    if (Prcb->QuantumEnd)
    {
        /* Handle quantum end */
        Prcb->QuantumEnd = FALSE;
        KiQuantumEnd();
    }
    else if (Prcb->NextThread ||
             ((Prcb->CurrentThread == Prcb->IdleThread) && Prcb->ReadySummary))
    {
        /* Acquire the PRCB lock */
        KiAcquirePrcbLock(Prcb);

        /* Capture current thread data */
        OldThread = Prcb->CurrentThread;

        /* Convert a ready-queued idle placement into standby state. */
        if ((Prcb->NextThread == NULL) &&
            (OldThread == Prcb->IdleThread) &&
            (Prcb->ReadySummary != 0))
        {
            NewThread = KiSelectReadyThread(0, Prcb);
            if (NewThread != NULL)
            {
                NewThread->State = Standby;
                Prcb->NextThread = NewThread;
            }
        }

        if ((Prcb->NextThread == NULL) ||
            KiConsumeSelfNextThread(Prcb, OldThread))
        {
            KiReleasePrcbLock(Prcb);
        }
        else
        {
#ifdef CONFIG_SMP
            KfRaiseIrql(SYNCH_LEVEL);
#endif

            NewThread = Prcb->NextThread;
            Prcb->NextThread = NULL;
#ifdef CONFIG_SMP
            InterlockedBitTestAndResetAffinity(&KiIdleSummary, Prcb->Number);
#endif
            Prcb->CurrentThread = NewThread;

            NewThread->State = Running;
            OldThread->WaitReason = WrDispatchInt;

            KxQueueReadyThread(OldThread, Prcb);
            KiSwapContext(APC_LEVEL, OldThread);
        }
    }

    /* Disable interrupts and go back to old irql */
    _disable();
    KeLowerIrql(OldIrql);
}

_Requires_lock_not_held_(Prcb->PrcbLock)
VOID
NTAPI
KiIpiInterruptHandler(VOID)
{
    KIRQL OldIrql;

    OldIrql = KfRaiseIrql(IPI_LEVEL);
    KiSendEOI();

    if (SmpDbgEnabled)
        SmpDbgIpi(KeGetCurrentProcessorNumber());
    KiIpiProcessRequests();

    KeLowerIrql(OldIrql);
}

VOID
KiNmiInterruptHandler(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ PKEXCEPTION_FRAME ExceptionFrame)
{
    BOOLEAN ManualSwapGs = FALSE;

    /* Check if the NMI came from kernel mode */
    if ((TrapFrame->SegCs & MODE_MASK) == 0)
    {
        /* Check if GS base is already kernel mode. This is needed, because
           we might be interrupted during an interrupt/exception from user-mode
           before the swapgs instruction. */
        if ((LONG64)__readmsr(MSR_GS_BASE) >= 0)
        {
            /* Swap GS to kernel */
            __swapgs();
            ManualSwapGs = TRUE;
        }
    }

    /* Check if this is a freeze */
    if (KiProcessorFreezeHandler(TrapFrame, ExceptionFrame))
    {
        /* NMI was handled */
        goto Exit;
    }

    /* Handle the NMI */
    KiHandleNmi();

Exit:
    /* Check if we need to swap GS back */
    if (ManualSwapGs)
    {
        /* Swap GS back to user */
        __swapgs();
    }
}

#define MAX_SYSCALL_PARAMS 16

NTSTATUS
NtSyscallFailure(void)
{
    /* This is the failure function */
    return (NTSTATUS)KeGetCurrentThread()->TrapFrame->Rax;
}

NTSTATUS
KiConvertToGuiThreadFailure(VOID)
{
    ULONG ServiceNumber = (ULONG)KeGetCurrentThread()->TrapFrame->Rax & SERVICE_NUMBER_MASK;

    return KiGetGuiServiceFailureStatus(ServiceNumber);
}

VOID
KiInstrumentTrapFrame(
    _Inout_ PKTRAP_FRAME TrapFrame)
{
    PKTHREAD Thread = KeGetCurrentThread();
    PVOID Callback = Thread->ApcState.Process->InstrumentationCallback;

    if (Callback == NULL)
        return;

    TrapFrame->R10 = TrapFrame->Rip;
    TrapFrame->Rip = (ULONG64)Callback;
    InterlockedAnd8((PCHAR)&Thread->Header.DebugActive, (CHAR)~DEBUG_ACTIVE_INSTRUMENTED);
}

VOID
KiPicoSystemCallHandler(
    _Inout_ PKTRAP_FRAME TrapFrame,
    _Inout_ PKEXCEPTION_FRAME ExceptionFrame)
{
    PKTHREAD Thread = KeGetCurrentThread();

    __addgsdword(FIELD_OFFSET(KIPCR, Prcb.KeSystemCalls), 1);
    Thread->PreviousMode = TrapFrame->PreviousMode = UserMode;
    TrapFrame->PreviousIrql = PASSIVE_LEVEL;
    TrapFrame->ExceptionFrame = 0;
    TrapFrame->ExceptionActive = KEXCEPTION_ACTIVE_SERVICE_FRAME;
    _enable();
    PspPicoSystemCallDispatch(TrapFrame, ExceptionFrame);
    if (Thread->ApcState.UserApcPending)
    {
        KIRQL OldIrql;

        KeRaiseIrql(APC_LEVEL, &OldIrql);
        KiDeliverApc(UserMode, ExceptionFrame, TrapFrame);
        KeLowerIrql(OldIrql);
    }
    _disable();
}

/* ====================================================================
 * Gecko win32k per-syscall timing diag (B2: inter-entry, no asm).
 * DIAGNOSTIC ONLY (trace-loader-gecko2), never for main. Latches the first
 * Gecko process that issues a win32k syscall and its first GUI thread (the
 * UI/main thread). Per-syscall all-thread COUNT, plus main-thread inclusive
 * TIME approximated as the span from a win32k entry to the next syscall entry
 * of any kind on that thread (so OS waits are excluded). Dumped top-30 at the
 * first top-level window (via win32k) and at process exit.
 * ==================================================================== */
#define KIW32_MAX 2048
static PEPROCESS KiW32TraceProcess = NULL;
PKTHREAD volatile KiW32TraceThread = NULL;   /* read by KiUpdateRunTime (USERSAMP) */
static LONG64    KiW32Freq = 0;
static LONG      KiW32PendId = -1;
static LONG64    KiW32PendTsc = 0;
static ULONG     KiW32Count[KIW32_MAX];
static LONG64    KiW32Time[KIW32_MAX];
static PVOID     KiW32Addr[KIW32_MAX];
static LONG64    KiW32CallsAll = 0;
static LONG64    KiW32CallsMain = 0;
static LONG      KiW32DumpedWindow = 0;
static LONG      KiW32DumpedExit = 0;

static BOOLEAN
KiW32IsGeckoImage(PEPROCESS Process)
{
    const CHAR *Image = PsGetProcessImageFileName(Process);
    if (!Image) return FALSE;
    return (!_strnicmp(Image, "firefox", 7) ||
            !_strnicmp(Image, "basilisk", 8) ||
            !_strnicmp(Image, "plugin-conta", 12)) ? TRUE : FALSE;
}

/* ====================================================================
 * USERSAMP: user-mode sampling of the latched Gecko main thread.
 * DIAGNOSTIC ONLY (trace-loader-gecko2), never for main. KiUpdateRunTime
 * calls KiW32UserSample on every clock tick whose current thread is
 * KiW32TraceThread (one pointer compare for all other threads). A user-mode
 * tick bumps an open-addressed histogram keyed by the RIP page (integer
 * only, interrupt-safe); a kernel-mode tick only bumps a counter. The
 * histogram is resolved against PEB->Ldr by KiUsDump at the W32DIAG dumps.
 * ==================================================================== */
#define KIUS_SLOTS 2048
#define KIUS_MODS  512
#define KIUS_TOP   40
static ULONG64 KiUsPage[KIUS_SLOTS];
static ULONG   KiUsCount[KIUS_SLOTS];
static LONG64  KiUsTicksUser = 0;
static LONG64  KiUsTicksKernel = 0;
static LONG64  KiUsDropped = 0;
/* STATE/WAIT sampler: scheduling state of the latched thread, read every tick. */
static ULONG   KiUsState[8];
static ULONG   KiUsWait[40];
static LONG64  KiUsWaitIrp = 0;
static LONG64  KiUsStateTicks = 0;
static ULONG   KiUsOffRun = 0;
static ULONG   KiUsMaxOff = 0;
static LONG    KiUsStateStop = 0;   /* stop after the dump: the traced thread may exit next */

VOID NTAPI
KiW32UserSample(IN PKTRAP_FRAME TrapFrame, IN BOOLEAN UserMode)
{
    ULONG64 Page, Idx;
    ULONG n;

    if (!UserMode)
    {
        KiUsTicksKernel++;
        return;
    }
    KiUsTicksUser++;

    Page = TrapFrame->Rip >> 12;
    Idx = (Page * 0x9E3779B97F4A7C15ULL) >> (64 - 11);
    for (n = 0; n < KIUS_SLOTS; n++)
    {
        if (KiUsCount[Idx] == 0)
        {
            KiUsPage[Idx] = Page;
            KiUsCount[Idx] = 1;
            return;
        }
        if (KiUsPage[Idx] == Page)
        {
            KiUsCount[Idx]++;
            return;
        }
        Idx = (Idx + 1) & (KIUS_SLOTS - 1);
    }
    KiUsDropped++;
}

/* Read the latched thread's state every tick (even when not current) so
 * off-CPU time is attributed to Ready (scheduling) vs Waiting (I/O/IPC). */
VOID NTAPI
KiW32StateSample(VOID)
{
    PKTHREAD T = KiW32TraceThread;
    UCHAR st;

    if (T == NULL || KiUsStateStop)
        return;
    KiUsStateTicks++;
    st = T->State;
    if (st < 8)
        KiUsState[st]++;
    if (st == Running)
    {
        KiUsOffRun = 0;
    }
    else
    {
        if (++KiUsOffRun > KiUsMaxOff)
            KiUsMaxOff = KiUsOffRun;
        if (st == Waiting)
        {
            UCHAR wr = T->WaitReason;
            if (wr < 40)
                KiUsWait[wr]++;
            if (!IsListEmpty(&((PETHREAD)T)->IrpList))
                KiUsWaitIrp++;
        }
    }
}

static VOID
KiUsDump(PCSTR Reason)
{
    PEPROCESS Process = KiW32TraceProcess;
    KAPC_STATE ApcState;
    BOOLEAN Attached = FALSE;
    PPEB Peb;
    static ULONG64 ModBase[KIUS_MODS], ModEnd[KIUS_MODS], ModCount[KIUS_MODS];
    static PUNICODE_STRING ModName[KIUS_MODS];
    static UCHAR Used[KIUS_SLOTS], ModUsed[KIUS_MODS];
    ULONG NumMods = 0, i, k, m;
    ULONG64 Anon = 0, Total = 0;

    if (!Process)
        return;

    DbgPrint("USERSAMP dump reason=%s ticksUser=%I64d ticksKernel=%I64d dropped=%I64d\n",
             Reason, KiUsTicksUser, KiUsTicksKernel, KiUsDropped);
    DbgPrint("USERSAMP state ticks=%I64d init=%lu ready=%lu running=%lu standby=%lu term=%lu waiting=%lu transition=%lu deferredready=%lu maxoff=%lu waitirp=%I64d\n",
             KiUsStateTicks, KiUsState[0], KiUsState[1], KiUsState[2], KiUsState[3],
             KiUsState[4], KiUsState[5], KiUsState[6], KiUsState[7], KiUsMaxOff, KiUsWaitIrp);
    for (i = 0; i < 40; i++)
        if (KiUsWait[i])
            DbgPrint("USERSAMP waitreason %lu count=%lu\n", i, KiUsWait[i]);
    KiUsStateStop = 1;

    if (PsGetCurrentProcess() != Process)
    {
        KeStackAttachProcess(&Process->Pcb, &ApcState);
        Attached = TRUE;
    }

    _SEH2_TRY
    {
        PLIST_ENTRY Head, Entry;

        Peb = Process->Peb;
        if (!Peb || !Peb->Ldr)
            _SEH2_LEAVE;
        Head = &Peb->Ldr->InLoadOrderModuleList;
        for (Entry = Head->Flink; Entry != Head && NumMods < KIUS_MODS; Entry = Entry->Flink)
        {
            PLDR_DATA_TABLE_ENTRY Mod = CONTAINING_RECORD(Entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
            ModBase[NumMods] = (ULONG64)Mod->DllBase;
            ModEnd[NumMods] = (ULONG64)Mod->DllBase + Mod->SizeOfImage;
            ModName[NumMods] = &Mod->BaseDllName;
            ModCount[NumMods] = 0;
            ModUsed[NumMods] = 0;
            NumMods++;
        }

        /* Aggregate the histogram per module */
        for (i = 0; i < KIUS_SLOTS; i++)
        {
            ULONG64 Addr;
            if (!KiUsCount[i]) continue;
            Total += KiUsCount[i];
            Addr = KiUsPage[i] << 12;
            for (m = 0; m < NumMods; m++)
                if (Addr + 0xFFF >= ModBase[m] && Addr < ModEnd[m]) break;
            if (m < NumMods) ModCount[m] += KiUsCount[i];
            else Anon += KiUsCount[i];
        }

        DbgPrint("USERSAMP samples=%I64u anon=%I64u modules=%lu\n", Total, Anon, NumMods);
        for (k = 0; k < 20; k++)
        {
            ULONG64 best = 0; LONG bm = -1;
            for (m = 0; m < NumMods; m++)
                if (!ModUsed[m] && ModCount[m] > best) { best = ModCount[m]; bm = (LONG)m; }
            if (bm < 0) break;
            ModUsed[bm] = 1;
            DbgPrint("USERSAMP module %wZ count=%I64u\n", ModName[bm], best);
        }

        /* Hottest RIP pages as module+offset (page-granular) */
        RtlZeroMemory(Used, sizeof(Used));
        for (k = 0; k < KIUS_TOP; k++)
        {
            ULONG best = 0; LONG bi = -1;
            ULONG64 Addr;
            for (i = 0; i < KIUS_SLOTS; i++)
                if (!Used[i] && KiUsCount[i] > best) { best = KiUsCount[i]; bi = (LONG)i; }
            if (bi < 0) break;
            Used[bi] = 1;
            Addr = KiUsPage[bi] << 12;
            for (m = 0; m < NumMods; m++)
                if (Addr + 0xFFF >= ModBase[m] && Addr < ModEnd[m]) break;
            if (m < NumMods)
                DbgPrint("USERSAMP page %p = %wZ+0x%I64x count=%lu\n",
                         (PVOID)Addr, ModName[m],
                         Addr >= ModBase[m] ? Addr - ModBase[m] : 0, best);
            else
                DbgPrint("USERSAMP page %p = (anon) count=%lu\n", (PVOID)Addr, best);
        }

        /* Module map so any bucket can be resolved offline */
        for (m = 0; m < NumMods; m++)
            DbgPrint("USERSAMP map %p size=%I64x %wZ\n",
                     (PVOID)ModBase[m], ModEnd[m] - ModBase[m], ModName[m]);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        DbgPrint("USERSAMP dump aborted (exception 0x%lx)\n", _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    if (Attached)
        KeUnstackDetachProcess(&ApcState);
    DbgPrint("USERSAMP end reason=%s\n", Reason);
}

static VOID
KiW32TraceDump(PCSTR Reason)
{
    ULONG i, k;
    const CHAR *Image = KiW32TraceProcess ? PsGetProcessImageFileName(KiW32TraceProcess) : "?";
    static UCHAR Printed[KIW32_MAX];

    RtlZeroMemory(Printed, sizeof(Printed));
    DbgPrint("W32DIAG dump reason=%s image=%s qpcfreq=%I64u callsAll=%I64u callsMain=%I64u\n",
             Reason, Image, KiW32Freq, KiW32CallsAll, KiW32CallsMain);
    for (k = 0; k < 30; k++)
    {
        LONG64 best = 0; LONG bi = -1;
        for (i = 0; i < KIW32_MAX; i++)
            if (!Printed[i] && KiW32Time[i] > best) { best = KiW32Time[i]; bi = (LONG)i; }
        if (bi < 0) break;
        Printed[bi] = 1;
        {
            ULONG64 us = KiW32Freq ? (ULONG64)(KiW32Time[bi] * 1000000 / KiW32Freq) : 0;
            DbgPrint("W32DIAG #%-4ld addr=%p count=%lu time=%I64u us avg=%I64u us\n",
                     bi, KiW32Addr[bi], KiW32Count[bi], us,
                     KiW32Count[bi] ? us / KiW32Count[bi] : us);
        }
    }
    DbgPrint("W32DIAG end reason=%s\n", Reason);
    KiUsDump(Reason);
}

/* Called from win32k: Reason 1 = first top-level window, 2 = process exit. */
VOID NTAPI
KeGeckoW32TraceEvent(PVOID Process, ULONG Reason)
{
    if (KiW32TraceProcess == NULL || Process != (PVOID)KiW32TraceProcess)
        return;
    if (Reason == 1)
    {
        if (InterlockedCompareExchange(&KiW32DumpedWindow, 1, 0) == 0)
            KiW32TraceDump("first-window");
    }
    else
    {
        if (InterlockedCompareExchange(&KiW32DumpedExit, 1, 0) == 0)
            KiW32TraceDump("exit");
    }
}

PVOID
KiSystemCallHandler(
    PKTRAP_FRAME TrapFrame)
{
    PKSERVICE_TABLE_DESCRIPTOR DescriptorTable;
    PKTHREAD Thread;
    PULONG64 KernelParams, UserParams;
    ULONG ServiceNumber, TableIndex, Count;
    ULONG64 UserRsp;

    /* Increase system call count */
    __addgsdword(FIELD_OFFSET(KIPCR, Prcb.KeSystemCalls), 1);

    /* Get the current thread */
    Thread = KeGetCurrentThread();

    /* Set previous mode */
    Thread->PreviousMode = TrapFrame->PreviousMode = UserMode;

    /* NtContinue and NtRaiseException leave through KiExceptionExit,
     * which checks this field instead of taking the SYSRET path. */
    TrapFrame->PreviousIrql = PASSIVE_LEVEL;

    /* We don't have an exception frame yet */
    TrapFrame->ExceptionFrame = 0;
    TrapFrame->ExceptionActive = KEXCEPTION_ACTIVE_SERVICE_FRAME;

    if (Thread->ApcState.Process->InstrumentationCallback != NULL)
    {
        if (!(Thread->Header.DebugActive & DEBUG_ACTIVE_INSTRUMENTED))
            Thread->Header.DebugActive |= DEBUG_ACTIVE_INSTRUMENTED;
    }
    else if (Thread->Header.DebugActive & DEBUG_ACTIVE_INSTRUMENTED)
    {
        Thread->Header.DebugActive &= ~DEBUG_ACTIVE_INSTRUMENTED;
    }

    /* Get the user Stack pointer */
    UserRsp = TrapFrame->Rsp;

    /* Enable interrupts */
    _enable();

    /* If the usermode rsp was not a usermode address, prepare an exception */
    if (UserRsp > MmUserProbeAddress) UserRsp = MmUserProbeAddress;

    /* Get the address of the usermode and kernelmode parameters */
    UserParams = (PULONG64)UserRsp + 1;
    KernelParams = (PULONG64)TrapFrame - MAX_SYSCALL_PARAMS;

    /* Get the system call number from the trap frame and decode it */
    ServiceNumber = (ULONG)TrapFrame->Rax;
    TableIndex = (ServiceNumber >> TABLE_OFFSET_BITS) & ((1 << TABLE_NUMBER_BITS) - 1);
    ServiceNumber &= SERVICE_NUMBER_MASK;

    /* Check for win32k system calls */
    if (TableIndex == WIN32K_SERVICE_INDEX)
    {
        ULONG GdiBatchCount = 0;

        /* Read the GDI batch count from the TEB */
        _SEH2_TRY
        {
            GdiBatchCount = NtCurrentTeb()->GdiBatchCount;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            GdiBatchCount = 0;
        }
        _SEH2_END;

        /* Flush batch, if there are entries */
        if (GdiBatchCount != 0)
        {
            KeGdiFlushUserBatch();
        }
    }

    /* Get descriptor table */
#if defined(_WIN64) && (NTDDI_VERSION >= NTDDI_LONGHORN)
    /* TODO(NT6.1): KTHREAD->ServiceTable does not exist on amd64 from Vista onwards
     * (see ndk/ketypes.h). This branch derives the descriptor table from the global
     * KeServiceDescriptorTable[Shadow] using the GuiThread flag. The lookup compiles
     * but has not been validated against real NT6.x semantics — revisit when wiring
     * up the full Win7 syscall path. */
    {
        PVOID _ServiceTable = (Thread->GuiThread) ?
            (PVOID)KeServiceDescriptorTableShadow : (PVOID)KeServiceDescriptorTable;
        DescriptorTable = &((PKSERVICE_TABLE_DESCRIPTOR)_ServiceTable)[TableIndex];
    }
#else
    DescriptorTable = &((PKSERVICE_TABLE_DESCRIPTOR)Thread->ServiceTable)[TableIndex];
#endif

    if ((TableIndex == WIN32K_SERVICE_INDEX) &&
        PsGetCurrentProcess()->MitigationFlagsValues.DisallowWin32kSystemCalls)
    {
        TrapFrame->Rax = STATUS_INVALID_SYSTEM_SERVICE;
        return (PVOID)NtSyscallFailure;
    }

    /* Validate the system call number */
    if (ServiceNumber >= DescriptorTable->Limit)
    {
        /* Check if this is a GUI call and this is not a GUI thread yet */
        if ((TableIndex == WIN32K_SERVICE_INDEX) &&
#if defined(_WIN64) && (NTDDI_VERSION >= NTDDI_LONGHORN)
            /* TODO(NT6.1): see above — uses GuiThread instead of ServiceTable compare */
            (!Thread->GuiThread))
#else
            (Thread->ServiceTable == KeServiceDescriptorTable))
#endif
        {
            /* Convert this thread to a GUI thread.
               It is invalid to change the stack in the middle of a C function,
               therefore we return KiConvertToGuiThread to the system call entry
               point, which then calls the asm function KiConvertToGuiThread,
               which allocates a new stack, switches to it, calls
               PsConvertToGuiThread and resumes in the middle of
               KiSystemCallEntry64 to restart the system call handling.
               If converting fails, the system call returns a failure code. */
            return (PVOID)KiConvertToGuiThread;
        }

        /* Fail the call */
        TrapFrame->Rax = STATUS_INVALID_SYSTEM_SERVICE;
        return (PVOID)NtSyscallFailure;
    }

    /* Get stack bytes and calculate argument count */
    Count = DescriptorTable->Number[ServiceNumber] / 8;

    _SEH2_TRY
    {
        switch (Count)
        {
            case 16: KernelParams[15] = UserParams[15];
            case 15: KernelParams[14] = UserParams[14];
            case 14: KernelParams[13] = UserParams[13];
            case 13: KernelParams[12] = UserParams[12];
            case 12: KernelParams[11] = UserParams[11];
            case 11: KernelParams[10] = UserParams[10];
            case 10: KernelParams[9] = UserParams[9];
            case 9: KernelParams[8] = UserParams[8];
            case 8: KernelParams[7] = UserParams[7];
            case 7: KernelParams[6] = UserParams[6];
            case 6: KernelParams[5] = UserParams[5];
            case 5: KernelParams[4] = UserParams[4];
            case 4:
            case 3:
            case 2:
            case 1:
            case 0:
                break;

            default:
                ASSERT(FALSE);
                break;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        TrapFrame->Rax = _SEH2_GetExceptionCode();
        return (PVOID)NtSyscallFailure;
    }
    _SEH2_END;

    /* ---- Gecko win32k per-syscall timing diag (B2; diag build, no behaviour change) ---- */
    if (KiW32TraceProcess == NULL)
    {
        if (TableIndex == WIN32K_SERVICE_INDEX && KiW32IsGeckoImage(PsGetCurrentProcess()))
        {
            LARGE_INTEGER Freq;
            (VOID)KeQueryPerformanceCounter(&Freq);
            KiW32Freq = Freq.QuadPart;
            KiW32TraceThread = Thread;
            KiW32PendId = -1;
            KiW32TraceProcess = PsGetCurrentProcess();   /* publish last */
        }
    }
    if (PsGetCurrentProcess() == KiW32TraceProcess)
    {
        if (TableIndex == WIN32K_SERVICE_INDEX && ServiceNumber < KIW32_MAX)
        {
            KiW32Count[ServiceNumber]++;
            KiW32Addr[ServiceNumber] = (PVOID)DescriptorTable->Base[ServiceNumber];
            KiW32CallsAll++;
        }
        if (Thread == KiW32TraceThread)
        {
            LARGE_INTEGER Now = KeQueryPerformanceCounter(NULL);
            if (KiW32PendId >= 0)
                KiW32Time[KiW32PendId] += Now.QuadPart - KiW32PendTsc;
            if (TableIndex == WIN32K_SERVICE_INDEX && ServiceNumber < KIW32_MAX)
            {
                KiW32PendId = (LONG)ServiceNumber;
                KiW32PendTsc = Now.QuadPart;
                KiW32CallsMain++;
            }
            else
            {
                KiW32PendId = -1;   /* non-win32k syscall ends the timed span */
            }
        }
    }

    return (PVOID)DescriptorTable->Base[ServiceNumber];
}


// FIXME: we need to
VOID
KiSystemService(IN PKTHREAD Thread,
                IN PKTRAP_FRAME TrapFrame,
                IN ULONG Instruction)
{
    UNIMPLEMENTED;
    __debugbreak();
}
