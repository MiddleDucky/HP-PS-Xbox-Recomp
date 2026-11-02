/**
 * A sampling profiler, in process (RECOMP_PROFILE=1).
 *
 * Wine runs under the host's ptrace restrictions, so perf/gdb cannot attach
 * from outside. This thread suspends every other thread of the process about
 * 500 times a second, records its RIP, and every 10 seconds writes the
 * hottest addresses to profile.txt (in the working directory) -- resolved
 * offline against the executable's symbols (x86_64-w64-mingw32-nm -n).
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xbox/host_unwind.h>
#include <tlhelp32.h>
#include <mmsystem.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROF_SLOTS (1 << 16)

static struct { uint64_t pc; uint32_t n; } s_hist[PROF_SLOTS];
static uint32_t s_total;

static void prof_add(uint64_t pc)
{
    uint32_t h = (uint32_t)((pc * 0x9E3779B97F4A7C15ull) >> 48) & (PROF_SLOTS - 1);
    for (int i = 0; i < PROF_SLOTS; i++, h = (h + 1) & (PROF_SLOTS - 1)) {
        if (s_hist[h].pc == pc) { s_hist[h].n++; break; }
        if (!s_hist[h].pc) { s_hist[h].pc = pc; s_hist[h].n = 1; break; }
    }
    s_total++;
}

static int cmp_n(const void *a, const void *b)
{
    uint32_t x = ((const uint32_t *)((const char *)a + 8))[0];
    uint32_t y = ((const uint32_t *)((const char *)b + 8))[0];
    return x < y ? 1 : x > y ? -1 : 0;
}

static void prof_dump(void)
{
    static struct { uint64_t pc; uint32_t n; } top[PROF_SLOTS];
    int k = 0;
    FILE *f;
    for (int i = 0; i < PROF_SLOTS; i++)
        if (s_hist[i].pc) { top[k].pc = s_hist[i].pc; top[k].n = s_hist[i].n; k++; }
    qsort(top, (size_t)k, sizeof(top[0]), cmp_n);
    f = fopen("profile.txt", "w");
    if (!f) return;
    fprintf(f, "# %u samples\n", s_total);
    for (int i = 0; i < k && i < 3000; i++)
        fprintf(f, "%llx %u\n", (unsigned long long)top[i].pc, top[i].n);
    fclose(f);
}

static DWORD WINAPI prof_thread(LPVOID unused)
{
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId(), last = GetTickCount();
    (void)unused;
    timeBeginPeriod(1);
    for (;;) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te;
        if (snap == INVALID_HANDLE_VALUE) { Sleep(100); continue; }
        te.dwSize = sizeof(te);
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            HANDLE th;
            CONTEXT ctx;
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                continue;
            th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!th) continue;
            if (SuspendThread(th) != (DWORD)-1) {
                memset(&ctx, 0, sizeof(ctx));
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(th, &ctx))
                    prof_add(ctx.Rip);
                ResumeThread(th);
            }
            CloseHandle(th);
        }
        CloseHandle(snap);
        if (GetTickCount() - last >= 10000) {
            prof_dump();
            last = GetTickCount();
        }
        Sleep(2);
    }
    return 0;
}

extern ptrdiff_t g_xbox_mem_offset;
extern void xbox_DumpHeldLocks(FILE* f);

static uint32_t guest32(uint32_t va)
{
    const uint32_t *p = (const uint32_t *)((uintptr_t)va + g_xbox_mem_offset);
    return (va && !IsBadReadPtr(p, 4)) ? *p : 0;
}

/* The title's async file I/O workers (XDK-style request queues at 0x54AD5C, 32 contexts of 0xB8 bytes; worker
 * 0x1125A0): each context's pending queue (+0x14) and done list (+0x4c). A queue is count, flags, head, tail;
 * an item next, id, op, flags, then status and priority bytes. */
static void dump_queue(FILE *f, const char *name, uint32_t q)
{
    uint32_t it = guest32(q + 8);
    fprintf(f, "  %s count %u flags %X head %08X tail %08X:", name, guest32(q), guest32(q + 4), it, guest32(q + 12));
    for (int n = 0; it && n < 12; n++, it = guest32(it)) {
        uint32_t w4 = guest32(it + 16);
        fprintf(f, " [id %08X op %u fl %X st %d pr %u]", guest32(it + 4), guest32(it + 8), guest32(it + 12),
                (int)(int8_t)(w4 & 0xFF), (w4 >> 8) & 0xFF);
    }
    fputc('\n', f);
}

static void dump_io_workers(FILE *f)
{
    uint32_t table = guest32(0x54AD5C);
    for (uint32_t i = 0; table && i < 32; i++) {
        uint32_t c = table + i * 0xB8;
        if (!guest32(c) && !guest32(c + 0x88)) continue;
        fprintf(f, "io ctx %u at %08X: run %u cur %08X event %08X threshold %d b0 %u\n", i, c, guest32(c),
                guest32(c + 0x10), guest32(c + 0x88), (int)guest32(c + 0xB4), guest32(c + 0xB0));
        dump_queue(f, "pending", c + 0x14);
        dump_queue(f, "done", c + 0x4C);
    }
}

/* F11 writes every thread's native call stack to stacks.txt: a hung title's
 * threads all sit in waits, which the sampler above cannot tell apart. The
 * recompiled functions carry unwind data, so the stacks name sub_XXXXXXXX
 * frames once resolved with x86_64-w64-mingw32-nm -n. */
static void stack_dump_to(const char *path)
{
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    FILE *f;
    if (snap == INVALID_HANDLE_VALUE) return;
    f = fopen(path, "a");
    if (!f) { CloseHandle(snap); return; }
    fprintf(f, "# dump at tick %lu\n", (unsigned long)GetTickCount());
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        HANDLE th;
        CONTEXT ctx;
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
            continue;
        th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                        te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(th, &ctx)) {
                /* xbox/host_unwind.h: inside the thread's stack only (an unchecked walk read address 0 in Wine's
                 * ntdll and crashed the game) */
                DWORD64 pcs[48], lo, hi;
                int npc = xbox_thread_stack_bounds(th, &lo, &hi) ? xbox_host_unwind(&ctx, lo, hi, pcs, 48) : 0;
                fprintf(f, "thread %lu:", (unsigned long)te.th32ThreadID);
                for (int i = 0; i < npc; i++) fprintf(f, " %llx", (unsigned long long)pcs[i]);
                fputc('\n', f);
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    xbox_DumpHeldLocks(f);
    dump_io_workers(f);
    fclose(f);
}

static void stack_dump(void) { stack_dump_to("stacks.txt"); }

/* the debug trainer's report (hle_trainer.c): the same stacks into its folder */
void hpps_StackDumpTo(const char *path) { stack_dump_to(path); }

#ifdef RECOMP_CALL_PROFILE
/* HPPS_CALL_PROFILE build: every direct call counts its target. F7 writes the counts since the last F7 to
 * calls_N.txt (address and count, most called first) and starts over, so two dumps taken in two situations diff
 * into the functions one of them runs and the other does not. Guest code is below 4 MB: a flat table. */
#define CALL_SLOTS 0x400000u
static volatile LONG g_calls[CALL_SLOTS];
static DWORD g_calls_t0;

void recomp_call_profile_hit(uint32_t va)
{
    if (va < CALL_SLOTS) g_calls[va]++;
}

static int cmp_desc(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x >> 32) < (y >> 32) ? 1 : (x >> 32) > (y >> 32) ? -1 : 0;
}

static void calls_dump(void)
{
    static int n;
    char path[64];
    DWORD now = GetTickCount();
    size_t used = 0;
    uint64_t *rows = malloc(sizeof(uint64_t) * 65536);
    if (!rows) return;
    for (uint32_t va = 0; va < CALL_SLOTS && used < 65536; va++) {
        LONG c = InterlockedExchange(&g_calls[va], 0);
        if (c) rows[used++] = ((uint64_t)(uint32_t)c << 32) | va;
    }
    qsort(rows, used, sizeof(rows[0]), cmp_desc);
    snprintf(path, sizeof path, "calls_%d.txt", ++n);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "# %zu functions called over %lu ms\n", used, (unsigned long)(now - g_calls_t0));
        for (size_t i = 0; i < used; i++)
            fprintf(f, "%08X %u\n", (uint32_t)rows[i], (uint32_t)(rows[i] >> 32));
        fclose(f);
    }
    fprintf(stderr, "[PROFILE] call counts (%zu functions, %lu ms) written to %s\n", used,
            (unsigned long)(now - g_calls_t0), path);
    g_calls_t0 = now;
    free(rows);
}
#endif

static DWORD WINAPI stack_thread(LPVOID unused)
{
    (void)unused;
#ifdef RECOMP_CALL_PROFILE
    g_calls_t0 = GetTickCount();
#endif
    for (;;) {
#ifdef RECOMP_CALL_PROFILE
        if (GetAsyncKeyState(VK_F7) & 1) calls_dump();
#endif
        if (GetAsyncKeyState(VK_F11) & 1) {
            stack_dump();
            fprintf(stderr, "[PROFILE] thread stacks appended to stacks.txt\n");
        }
        Sleep(50);
    }
    return 0;
}

void recomp_profiler_start(void)
{
    const char *e = getenv("RECOMP_PROFILE");
    CloseHandle(CreateThread(NULL, 0, stack_thread, NULL, 0, NULL));
    if (!e || !*e || *e == '0')
        return;
    fprintf(stderr, "[PROFILE] sampling all threads; writing profile.txt every 10 s\n");
    CloseHandle(CreateThread(NULL, 0, prof_thread, NULL, 0, NULL));
}
