/*
 * host_unwind.h - a host (x64) stack walk that cannot fault, for samplers and crash reports.
 *
 * The [STALL] sampler, the profiler and the stack dumps walk other threads' stacks while those threads are suspended
 * anywhere: inside ___chkstk_ms (no unwind data, its return address not on top of the stack), half way through a
 * prologue, in code a frame register does not describe. A guess there gives a wrong RSP or frame register, and
 * RtlVirtualUnwind reads through it unchecked: under Wine/Proton it read address 0 in ntdll and took the whole game
 * down from the sampling thread (CoS 0.65 s into hpengx.xmv, PS at the save check; 2026-11-02). Nothing here reads
 * stack memory outside the thread's own stack: each step needs RSP inside [StackLimit, StackBase) and, when the
 * function's unwind data restores RSP from a frame register, that register inside it too. The walk stops instead.
 */
#ifndef XBOX_HOST_UNWIND_H
#define XBOX_HOST_UNWIND_H

#ifdef _WIN64
#include <stdint.h>
#include <windows.h>

/* the thread's stack, committed part: StackLimit to StackBase from its TEB (a thread of this process) */
static inline int xbox_thread_stack_bounds(HANDLE thread, DWORD64 *lo, DWORD64 *hi)
{
    typedef LONG(WINAPI * query_fn)(HANDLE, int, PVOID, ULONG, PULONG);
    static query_fn query;
    struct {
        LONG exit_status;
        PVOID teb;
        ULONG_PTR client_id[2];
        ULONG_PTR affinity;
        LONG priority, base_priority;
    } info;
    const NT_TIB *tib;
    if (!query) query = (query_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    if (!query || query(thread, 0 /* ThreadBasicInformation */, &info, sizeof info, NULL) < 0 || !info.teb)
        return 0;
    tib = (const NT_TIB *)info.teb;
    *lo = (DWORD64)(uintptr_t)tib->StackLimit;
    *hi = (DWORD64)(uintptr_t)tib->StackBase;
    return *hi > *lo;
}

/* the calling thread's own stack */
static inline void xbox_own_stack_bounds(DWORD64 *lo, DWORD64 *hi)
{
    const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
    *lo = (DWORD64)(uintptr_t)tib->StackLimit;
    *hi = (DWORD64)(uintptr_t)tib->StackBase;
}

/* RIP, then each caller by its unwind data, at most max; c is changed. A frame without unwind data is taken as a
 * leaf (return address on top of the stack) only first. Allocates and locks nothing: the thread looked at may hold
 * the heap lock. */
static inline int xbox_host_unwind(CONTEXT *c, DWORD64 lo, DWORD64 hi, DWORD64 *out, int max)
{
    int n = 0;
    while (n < max && c->Rip) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION f;
        out[n++] = c->Rip;
        if (c->Rsp < lo || c->Rsp + 8 > hi) break;
        f = RtlLookupFunctionEntry(c->Rip, &base, NULL);
        if (f) {
            PRUNTIME_FUNCTION g = f;
            const BYTE *info;
            unsigned frame_reg;
            void *data;
            DWORD64 frame;
            if (g->UnwindData & 1) g = (PRUNTIME_FUNCTION)(uintptr_t)(base + (g->UnwindData & ~1u));
            info = (const BYTE *)(uintptr_t)(base + g->UnwindData);
            frame_reg = info[3] & 0x0F; /* UNWIND_INFO.FrameRegister: RAX..R15 as CONTEXT lays them out */
            if (frame_reg) {
                DWORD64 v = (&c->Rax)[frame_reg];
                if (v < lo || v > hi) break;
            }
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, f, c, &data, &frame, NULL);
        } else if (n == 1) {
            c->Rip = *(const DWORD64 *)(uintptr_t)c->Rsp;
            c->Rsp += 8;
        } else {
            break;
        }
    }
    return n;
}

#endif /* _WIN64 */
#endif /* XBOX_HOST_UNWIND_H */
