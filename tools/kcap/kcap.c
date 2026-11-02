/*
 * kcap.dxt -- XBDM debugger extension that records every kernel import call
 * a title makes on real hardware.
 *
 * XBDM loads every E:\dxt\*.dxt at boot. Once the running title's kernel
 * thunk table is resolved, each function entry in it is redirected to a small
 * per-slot stub. The stub logs ordinal, stack args, ecx/edx, caller, thread
 * and TSC into a ring buffer, swaps the caller's return address for a
 * trampoline that logs eax:edx and the number of argument bytes the callee
 * popped, then jumps to the real kernel export. Data exports (KeTickCount,
 * XboxHardwareInfo, ...) are never touched.
 *
 * Because only the title's thunk table is patched, calls made by the kernel,
 * XBDM and this extension are not hooked, so logging can never recurse.
 *
 * The PC side drains the ring over the debug channel (port 731):
 *   kcap!status            one-line state
 *   kcap!read [max=N]      binary: u32 byte count, then N 64-byte records
 *   kcap!slots             multiline: per-import ordinal, mode, call count
 *   kcap!mode ord=N|all m=off|count|log
 *   kcap!hook [force]      hook now (force also hooks xshell)
 *   kcap!pause | resume | clear
 *
 * Build with build.sh; tools/kcap/kcap.py is the client.
 */

typedef unsigned char UCHAR;
typedef unsigned short USHORT;
typedef unsigned long ULONG;
typedef long LONG;
typedef unsigned __int64 ULONGLONG;
typedef long HRESULT;
typedef ULONG DWORD;
typedef void *PVOID;
typedef char *LPSTR;
typedef const char *LPCSTR;

/* ---- xboxkrnl.exe ------------------------------------------------------- */

typedef struct { USHORT Length; USHORT MaximumLength; char *Buffer; } ANSI_STRING;
typedef union { struct { ULONG LowPart; LONG HighPart; } u; __int64 QuadPart; } LARGE_INTEGER;

#define NTAPI __stdcall
#define KIMPORT __declspec(dllimport)

KIMPORT PVOID NTAPI MmAllocateContiguousMemory(ULONG NumberOfBytes);
KIMPORT UCHAR NTAPI MmIsAddressValid(PVOID VirtualAddress);
KIMPORT void NTAPI KeInitializeDpc(PVOID Dpc, PVOID DeferredRoutine, PVOID DeferredContext);
KIMPORT void NTAPI KeInitializeTimerEx(PVOID Timer, int Type);
KIMPORT UCHAR NTAPI KeSetTimerEx(PVOID Timer, LARGE_INTEGER DueTime, LONG Period, PVOID Dpc);
KIMPORT ULONG __cdecl DbgPrint(const char *Format, ...);
KIMPORT extern ANSI_STRING XeImageFileName;

/* ---- xbdm.dll ----------------------------------------------------------- */

#define DMHRAPI __declspec(dllimport) HRESULT __stdcall

typedef struct _DM_CMDCONT *PDM_CMDCONT;
typedef HRESULT (__stdcall *PDM_CMDCONTPROC)(PDM_CMDCONT pdmcc, LPSTR szResponse, DWORD cchResponse);
typedef struct _DM_CMDCONT {
    PDM_CMDCONTPROC HandlingFunction;
    DWORD DataSize;
    PVOID Buffer;
    DWORD BufferSize;
    PVOID CustomData;
    DWORD BytesRemaining;
} DM_CMDCONT;
typedef HRESULT (__stdcall *PDM_CMDPROC)(LPCSTR szCommand, LPSTR szResponse, DWORD cchResponse, PDM_CMDCONT pdmcc);
typedef DWORD (__stdcall *PDM_NOTIFY_FUNCTION)(ULONG dwNotification, DWORD dwParam);
typedef struct _DMN_SESSION *PDMN_SESSION;

DMHRAPI DmRegisterCommandProcessor(LPCSTR szProcessor, PDM_CMDPROC pfn);
DMHRAPI DmOpenNotificationSession(DWORD dwFlags, PDMN_SESSION *pSession);
DMHRAPI DmNotify(PDMN_SESSION Session, DWORD dwNotification, PDM_NOTIFY_FUNCTION pfnHandler);

#define DM_EXEC 3
#define DM_MODLOAD 5
#define DM_CREATETHREAD 7
#define DM_NOTIFICATIONMASK 0xffffff
#define DM_PERSISTENT 1

#define XBDM_NOERR          ((HRESULT)0x02DB0000)
#define XBDM_MULTIRESPONSE  ((HRESULT)0x02DB0002)
#define XBDM_BINRESPONSE    ((HRESULT)0x02DB0003)
#define XBDM_INVALIDARG     ((HRESULT)0x82DB0017)
#define XBDM_ENDOFLIST      ((HRESULT)0x82DB0104)
#define XBDM_UNDEFINED      ((HRESULT)0x82DB0000)

/* ---- configuration ------------------------------------------------------ */

#define KCAP_RING_BYTES   (4u << 20)   /* 65536 records; halved until it fits */
#define KCAP_READ_MAX     1024         /* records per kcap!read */
#define KCAP_MAX_SLOTS    512
#define KCAP_MAX_THREADS  64
#define KCAP_MAX_DEPTH    32

#define MODE_OFF   0
#define MODE_COUNT 1
#define MODE_LOG   2

#define XBE_BASE           0x00010000
#define XBE_MAGIC          0x48454258  /* "XBEH" */
#define THUNK_DEBUG_XOR    0xEFB1F152
#define THUNK_RETAIL_XOR   0x5B6D40B6
#define KERNEL_BASE        0x80010000

/* Called constantly by every title: counted, not logged, unless asked. */
static const USHORT g_hot_ordinals[] = {
    51, 52, 53, 54, 55, 56, 57, 58,     /* Interlocked* */
    101, 122,                           /* KeEnter/LeaveCriticalRegion */
    103, 104,                           /* KeGetCurrentIrql/Thread */
    125, 126, 127, 128,                 /* KeQuery*Time/PerformanceCounter */
    129, 130, 160, 161,                 /* IRQL raise/lower */
    139, 142,                           /* KeSave/RestoreFloatingPointState */
    151,                                /* KeStallExecutionProcessor */
    163,                                /* KiUnlockDispatcherDatabase */
    250, 251,                           /* ObfDereference/ReferenceObject */
    268, 284, 298, 320,                 /* RtlCompare/Fill/Move/ZeroMemory */
    277, 294, 306,                      /* Rtl*CriticalSection */
};

/* Never return to the caller, so their return address is left alone. */
static const USHORT g_noreturn_ordinals[] = { 49, 95, 96, 258 };

/* Data exports, in case the kernel's section table cannot tell us. */
static const USHORT g_data_ordinals[] = {
    16, 22, 30, 31, 40, 41, 42, 64, 70, 71, 88, 89, 102, 120, 156, 157, 162,
    164, 240, 245, 249, 259, 321, 322, 323, 324, 325, 326, 353, 354, 355, 356,
    357,
};

/* ---- records ------------------------------------------------------------ */

#define REC_ENTER 1
#define REC_EXIT  2

typedef struct {
    ULONG seq;
    USHORT ordinal;
    UCHAR kind;
    UCHAR depth;
    ULONG thread;      /* KTHREAD* */
    ULONG caller;      /* return address in the title */
    ULONGLONG tsc;
    ULONG v[10];       /* enter: 8 stack args, ecx, edx
                          exit:  eax, edx, arg bytes popped, enter seq */
} KCAP_REC;

typedef char kcap_rec_is_64_bytes[sizeof(KCAP_REC) == 64 ? 1 : -1];

typedef struct {
    ULONG target;
    ULONG count;
    USHORT ordinal;
    UCHAR mode;
    UCHAR noreturn;
} SLOT;

typedef struct {
    ULONG slot_addr;   /* where the caller's return address lives */
    ULONG caller;
    ULONG seq;
    USHORT ordinal;
    USHORT pad;
} SHADOW;

typedef struct {
    ULONG thread;
    ULONG depth;
    SHADOW e[KCAP_MAX_DEPTH];
} TSTACK;

/* Register images built by the stubs below (pushfd; pushad). */
typedef struct {
    ULONG edi, esi, ebp, esp_, ebx, edx, ecx, eax, eflags;
    ULONG slot;
    ULONG ret;
    ULONG args[8];
} HOOK_FRAME;

typedef struct {
    ULONG edi, esi, ebp, esp_, ebx, edx, ecx, eax, eflags;
    ULONG ret;
} RET_FRAME;

static KCAP_REC *g_ring;
static ULONG g_ring_mask;
static volatile ULONG g_head, g_tail, g_drops, g_seq;
static volatile LONG g_logging = 1;

static SLOT g_slots[KCAP_MAX_SLOTS];
static ULONG g_nslots, g_nlogged;
static ULONG *g_thunks;
static ULONG g_hooked_key;
static volatile LONG g_hooked;
static const char *g_hook_state = "waiting";
static char g_title[64];

static TSTACK g_tstacks[KCAP_MAX_THREADS];

#pragma section(".kcapx", read, write, execute)
__declspec(allocate(".kcapx")) static UCHAR g_stubs[KCAP_MAX_SLOTS][16];

static UCHAR g_dpc[64], g_timer[64];
static UCHAR g_stage[4 + KCAP_READ_MAX * sizeof(KCAP_REC)];
static char g_line[160];

/* No CRT: the compiler may still emit these for struct copies. */
void *memset(void *d, int c, unsigned int n)
{
    UCHAR *p = d;
    while (n--) *p++ = (UCHAR)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned int n)
{
    UCHAR *p = d;
    const UCHAR *q = s;
    while (n--) *p++ = *q++;
    return d;
}

/* ---- low level ---------------------------------------------------------- */

static ULONG irq_save(void)
{
    ULONG flags;
    __asm {
        pushfd
        pop eax
        mov flags, eax
        cli
    }
    return flags;
}

static void irq_restore(ULONG flags)
{
    __asm {
        push flags
        popfd
    }
}

static ULONG cur_thread(void)
{
    ULONG t;
    __asm {
        mov eax, fs:[0x28]   /* KPCR.PrcbData.CurrentThread */
        mov t, eax
    }
    return t;
}

static ULONGLONG rdtsc(void)
{
    ULONG lo, hi;
    __asm {
        rdtsc
        mov lo, eax
        mov hi, edx
    }
    return ((ULONGLONG)hi << 32) | lo;
}

static void write_protected(ULONG *dst, ULONG value)
{
    __asm {
        mov eax, cr0
        push eax
        and eax, 0xFFFEFFFF  /* clear CR0.WP */
        mov cr0, eax
    }
    *dst = value;
    __asm {
        pop eax
        mov cr0, eax
    }
}

static int in_list(const USHORT *list, ULONG n, ULONG ordinal)
{
    ULONG i;
    for (i = 0; i < n; i++)
        if (list[i] == ordinal)
            return 1;
    return 0;
}

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

/* ---- ring --------------------------------------------------------------- */

/* Caller holds interrupts off. */
static KCAP_REC *ring_alloc(void)
{
    KCAP_REC *r;
    if (!g_ring || g_head - g_tail > g_ring_mask) {
        g_drops++;
        return 0;
    }
    r = &g_ring[g_head & g_ring_mask];
    g_head++;
    return r;
}

static TSTACK *tstack_get(ULONG thread, int create)
{
    TSTACK *free_ts = 0;
    ULONG i;
    for (i = 0; i < KCAP_MAX_THREADS; i++) {
        TSTACK *ts = &g_tstacks[i];
        if (ts->thread == thread)
            return ts;
        if (!free_ts && (ts->thread == 0 || ts->depth == 0))
            free_ts = ts