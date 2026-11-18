/**
 * Trace wrappers around XAPI's controller entry points (RECOMP_PAD_TRACE=1).
 *
 * The emulated USB pad enumerates and is polled, yet the intro does not skip
 * on a press. These show what the title asks XAPI for and what it gets back:
 * which ports it opens, what XInputGetState returns, and when it sees a pad
 * arrive or leave. Nothing here changes behaviour; each wrapper runs the
 * generated XAPI body (sub_X_gen) and only logs.
 *
 * Built into the recompile with a third --exclude-manual naming this file.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* the register names (eax, esp, ...) the runtime macros expand to */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

#define ARG(i)  MEM32(g_esp + 4 + 4 * (i))

static int trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *t = getenv("RECOMP_PAD_TRACE");
        on = t && *t && *t != '0' ? atoi(t) : 0;  /* 2: also every left stick reading */
    }
    return on;
}

/* ── Direct controller access (RECOMP_HLE_XINPUT, default on) ────────────
 *
 * Ported from SCDA-XBERecomp's src/xdk/xapi.cpp: XInitDevices, XInputOpen,
 * XInputClose, XInputGetCapabilities and XInputGetState answer from the PC's
 * pad directly instead of going through the emulated OHCI controller and
 * XAPI's USB driver. That route delivered the stick late (Harry kept walking
 * the old way for a moment, or forward/back when the stick went sideways).
 * XGetDevices / XGetDeviceChanges stay XAPI's own code: they only read the
 * device type record that XInitDevices and the poller below keep current.
 * RECOMP_HLE_XINPUT=0 runs the XAPI bodies over the emulated USB as before. */
#include "input/xinput_xbox.h"

extern int app_KbmOn(void);  /* scda/app/input.cpp: keyboard and mouse play as controller 1 */
extern void app_InputPad0Present(int present);

/* scda/app/input.cpp's sneak toggle: Harry's pace as the game last picked it, [[0x3565B0] + 0xDC] + 0x2574 (0 stand,
 * 1 sneak, 2 walk, 3 run, set by the movement code at 0x8567A from the stick's push); -1 when there is no Harry */
int hpps_HarryPace(void)
{
    uint32_t player = MEM32(0x3565B0), ctrl;
    if (player < 0x80000000u || player >= 0x84000000u) return -1;
    ctrl = MEM32(player + 0xDC);
    if (ctrl < 0x80000000u || ctrl >= 0x84000000u) return -1;
    return (int)MEM32(ctrl + 0x2574);
}

/* a pad read, port 0 standing in at rest for keyboard and mouse when no controller is there (always: with no
 * controller on port 0 keyboard and mouse play, so unplugging it hands over instead of pausing the game) */
static DWORD pad_read(DWORD port, XBOX_INPUT_STATE *s)
{
    DWORD rc = xbox_InputGetState(port, s);
    if (port == 0) app_InputPad0Present(rc == 0);
    if (rc != 0 && port == 0 && app_KbmOn()) {
        memset(s, 0, sizeof(*s));
        rc = 0;
    }
    return rc;
}

#define XPP_GAMEPAD_TYPE 0x001F422Cu  /* g_DeviceType_Gamepad: Current, Change, Previous connected */
#define XPP_TYPES_BEGIN  0x001F4204u  /* g_DeviceTypeInfoTableBegin .. End: the linked device types */
#define XPP_TYPES_END    0x001F420Cu
#define HANDLE_BASE      0x5ADE0010u  /* opaque handles, never dereferenced by the title */
#define MAX_HANDLES      32

static int hle_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_HLE_XINPUT");
        on = !(e && *e == '0');
        fprintf(stderr, "[INPUT] XInput %s\n", on ? "direct (SCDA style)" : "through the emulated USB");
    }
    return on;
}

static CRITICAL_SECTION g_pad_lock;
/* One record per XInputOpen, as SCDA's g_handles: a title can hold two handles
 * on one port (the menu's and the game's) and close either. A handle per port
 * let the menu's close kill the game's, which then read "not connected". */
static struct { int used; uint32_t port, packet; XBOX_GAMEPAD last; } g_h[MAX_HANDLES];

static void update_ports(void)
{
    uint32_t cur = 0, old;
    /* read each pad: on Windows xbox_InputIsConnected is only a cache the reads update */
    for (DWORD p = 0; p < 4; p++) {
        XBOX_INPUT_STATE s;
        if (pad_read(p, &s) == 0) cur |= 1u << p;
    }
    EnterCriticalSection(&g_pad_lock);
    old = MEM32(XPP_GAMEPAD_TYPE);
    if (cur != old) {
        MEM32(XPP_GAMEPAD_TYPE + 4) |= cur ^ old;
        MEM32(XPP_GAMEPAD_TYPE) = cur;
        fprintf(stderr, "[INPUT] controller ports now %X (was %X)\n", cur, old);
    }
    LeaveCriticalSection(&g_pad_lock);
}

static DWORD WINAPI port_poller(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(250);
        update_ports();
    }
    return 0;
}

static int handle_index(uint32_t h)
{
    uint32_t i = (h - HANDLE_BASE) / 16;
    if (h < HANDLE_BASE || (h - HANDLE_BASE) % 16 || i >= MAX_HANDLES || !g_h[i].used) return -1;
    return (int)i;
}

static int handle_port(uint32_t h)
{
    int i = handle_index(h);
    return i < 0 ? -1 : (int)g_h[i].port;
}

/* XInitDevices(dwPreallocTypeCount, PreallocTypes) */
extern void sub_001F4543_gen(void);
void sub_001F4543(void)
{
    static int poller;
    if (!hle_on()) {
        sub_001F4543_gen();
        return;
    }
    InitializeCriticalSection(&g_pad_lock);
    xbox_InputInit();
    /* every linked device type (Current, Change, Previous connected) starts empty */
    for (uint32_t t = XPP_TYPES_BEGIN; t < XPP_TYPES_END; t += 4) {
        uint32_t info = MEM32(t), type = info ? MEM32(info + 4) : 0;  /* { class, type record, ... } */
        if (type)
            for (int i = 0; i < 3; i++) MEM32(type + 4 * i) = 0;
    }
    for (int i = 0; i < 3; i++) MEM32(XPP_GAMEPAD_TYPE + 4 * i) = 0;
    /* No update_ports() here, unlike SCDA: on the console the pads enumerate over
     * USB after XInitDevices returns, so a title sees each one arrive as an
     * insertion from XGetDeviceChanges. HP:CoS set its per-port "connected" flag
     * (0x51E0BC + port * 0x38) only on that insertion; a pad already present at
     * XInitDevices was consumed by its boot-time XGetDevices, the flag stayed 0 and
     * gameplay asked to connect a controller. The poller reports them 250 ms in. */
    if (!poller) {
        poller = 1;
        CloseHandle(CreateThread(NULL, 0, port_poller, NULL, 0, NULL));
    }
    g_esp += 4 + 8;
}

/* XInputOpen(DeviceType, dwPort, dwSlot, pPollingParameters) -> HANDLE */
extern void sub_001F4FE2_gen(void);
void sub_001F4FE2(void)
{
    uint32_t type = ARG(0), port = ARG(1), slot = ARG(2);
    if (hle_on()) {
        g_eax = 0;
        if (type == XPP_GAMEPAD_TYPE && port < 4) {
            for (int i = 0; i < MAX_HANDLES; i++) {
                if (g_h[i].used) continue;
                memset(&g_h[i], 0, sizeof(g_h[i]));
                g_h[i].used = 1;
                g_h[i].port = port;
                g_eax = HANDLE_BASE + 16u * (uint32_t)i;
                break;
            }
        }
        g_esp += 4 + 16;
    } else {
        sub_001F4FE2_gen();
    }
    if (trace_on())
        fprintf(stderr, "[PADTRACE] XInputOpen(type %08X, port %u, slot %u) = %08X\n",
                type, port, slot, g_eax);
}

/* XInputClose(hDevice) */
extern void sub_001F5038_gen(void);
void sub_001F5038(void)
{
    int p;
    if (!hle_on()) {
        sub_001F5038_gen();
        return;
    }
    p = handle_index(ARG(0));
    if (p >= 0) g_h[p].used = 0;
    if (trace_on())
        fprintf(stderr, "[PADTRACE] XInputClose(%08X)\n", ARG(0));
    g_esp += 4 + 4;
}

/* XInputGetCapabilities(hDevice, pCapabilities) -> DWORD. XINPUT_CAPABILITIES:
 * BYTE SubType, WORD Reserved (at 2), XINPUT_GAMEPAD In (at 4), XINPUT_RUMBLE Out (at 22). */
extern void sub_001F5044_gen(void);
void sub_001F5044(void)
{
    uint32_t caps = ARG(1);
    int p;
    if (!hle_on()) {
        sub_001F5044_gen();
        return;
    }
    p = handle_port(ARG(0));
    if (p < 0) {
        g_eax = 6;                          /* ERROR_INVALID_HANDLE */
    } else if (!xbox_InputIsConnected((DWORD)p) && !(p == 0 && app_KbmOn())) {
        g_eax = 1167;                       /* ERROR_DEVICE_NOT_CONNECTED */
    } else {
        MEM8(caps) = 1;                     /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
        MEM8(caps + 1) = 0;
        MEM16(caps + 2) = 0;
        MEM16(caps + 4) = 0x00FF;           /* every digital button */
        for (int i = 0; i < 8; i++) MEM8(caps + 6 + i) = 0xFF;
        for (int i = 0; i < 4; i++) MEM16(caps + 14 + 2 * i) = 0xFFFF;
        MEM16(caps + 22) = 0xFFFF;          /* both motors */
        MEM16(caps + 24) = 0xFFFF;
        g_eax = 0;
    }
    g_esp += 4 + 8;
}

extern int hpps_MenuOpen(void);  /* scda/app/menu.cpp */
extern void app_InputPad(int port, XBOX_GAMEPAD *g);  /* scda/app/input.cpp */
extern uint8_t hpps_LookWhite(uint8_t white);  /* hle_camera.c: White as the first-person look's switch */

/* Stick range compensation (RECOMP_STICK_RANGE, default 0.65; 1 = off).
 *
 * The game runs only when the stick reads past 0.5 after its own 0.35
 * deadzone (0x19050, test at 0x62885): about 22100 raw. A Controller S gives
 * 32767 at full tilt, but some PC pads report a short range in one direction
 * (one reported -21900 held straight back, 32767 forward), so Harry walked
 * backwards. Deflection at the given fraction of full now counts as full,
 * scaled radially so the direction is kept. */
static float stick_range(void)
{
    static float r = -1.0f;
    if (r < 0.0f) {
        const char *e = getenv("RECOMP_STICK_RANGE");
        r = e && *e ? (float)atof(e) : 0.65f;
        if (r <= 0.05f || r > 1.0f) r = 1.0f;
    }
    return r;
}

static void stick_compensate(SHORT *x, SHORT *y)
{
    float r = stick_range();
    if (r >= 1.0f) return;
    float fx = *x / 32767.0f, fy = *y / 32767.0f;
    float m = sqrtf(fx * fx + fy * fy);
    if (m <= 0.0f) return;
    float s = 1.0f / r;
    if (m * s > 1.0f) s = 1.0f / m;  /* keep the result on the unit circle */
    fx *= s; fy *= s;
    if (fx > 1.0f) fx = 1.0f; else if (fx < -1.0f) fx = -1.0f;
    if (fy > 1.0f) fy = 1.0f; else if (fy < -1.0f) fy = -1.0f;
    *x = (SHORT)lrintf(fx * 32767.0f);
    *y = (SHORT)lrintf(fy * 32767.0f);
}

/* the pad as the game last read it: the debug trainer's noclip flies Harry with it (hle_trainer.c) */
XBOX_GAMEPAD hpps_last_pad;

static void get_state_direct(void)
{
    uint32_t st = ARG(1);
    int h = handle_index(ARG(0));
    XBOX_INPUT_STATE s;
    if (h < 0) {
        g_eax = 6;
    } else if (pad_read((DWORD)g_h[h].port, &s) != 0) {
        g_eax = 1167;
    } else {
        /* the F10 options panel has the pad while it is open: the game sees it at rest */
        if (hpps_MenuOpen()) memset(&s.Gamepad, 0, sizeof(s.Gamepad));
        /* the controls settings: remapping, deadzones, camera inversion, keyboard and mouse on port 0 */
        app_InputPad((int)g_h[h].port, &s.Gamepad);
        stick_compensate(&s.Gamepad.sThumbLX, &s.Gamepad.sThumbLY);
        stick_compensate(&s.Gamepad.sThumbRX, &s.Gamepad.sThumbRY);
        /* White as a switch for the first-person look (hle_camera.c) */
        if (g_h[h].port == 0) s.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] = hpps_LookWhite(s.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE]);
        hpps_last_pad = s.Gamepad;
        /* the packet number moves only when something changed, as XAPI's does */
        if (memcmp(&s.Gamepad, &g_h[h].last, sizeof(s.Gamepad))) g_h[h].packet++;
        g_h[h].last = s.Gamepad;
        MEM32(st) = g_h[h].packet;
        MEM16(st + 4) = s.Gamepad.wButtons;
        for (int i = 0; i < 8; i++) MEM8(st + 6 + i) = s.Gamepad.bAnalogButtons[i];
        MEM16(st + 14) = (uint16_t)s.Gamepad.sThumbLX;
        MEM16(st + 16) = (uint16_t)s.Gamepad.sThumbLY;
        MEM16(st + 18) = (uint16_t)s.Gamepad.sThumbRX;
        MEM16(st + 20) = (uint16_t)s.Gamepad.sThumbRY;
        g_eax = 0;
    }
    g_esp += 4 + 8;
}

/* XInputGetState(hDevice, pState) -> DWORD. XINPUT_STATE: dwPacketNumber,
 * then XINPUT_GAMEPAD { wButtons, bAnalogButtons[8], sThumb* }. Logs every
 * change of result, buttons or the A/B/X/Y pressure, and a heartbeat. */
extern void sub_001F521C_gen(void);
void sub_001F521C(void)
{
    static uint32_t last_rc = 0xFFFFFFFF, last_btn = 0xFFFFFFFF, last_abxy = 0xFFFFFFFF, calls;
    static DWORD t0;
    uint32_t dev = ARG(0), st = ARG(1);
    if (hle_on())
        get_state_direct();
    else
        sub_001F521C_gen();
    if (!trace_on())
        return;
    calls++;
    {
        uint32_t rc = g_eax, btn = st ? MEM16(st + 4) : 0, abxy = st ? MEM32(st + 6) : 0;
        DWORD now = GetTickCount();
        if (rc != last_rc || btn != last_btn || abxy != last_abxy)
            fprintf(stderr, "[PADTRACE] XInputGetState(%08X) = %u pkt=%u buttons=%04X ABXY=%08X\n",
                    dev, rc, st ? MEM32(st) : 0, btn, abxy);
        last_rc = rc; last_btn = btn; last_abxy = abxy;
        /* the left stick as the game gets it, every call, with the time: a flick that reads centred for several
         * frames or repeats a stale value shows input arriving late */
        if (st && trace_on() > 1) {
            LARGE_INTEGER q, f;
            QueryPerformanceCounter(&q);
            QueryPerformanceFrequency(&f);
            fprintf(stderr, "[STICK] %.1f %d %d pkt=%u\n", q.QuadPart * 1000.0 / f.QuadPart,
                    (int16_t)MEM16(st + 14), (int16_t)MEM16(st + 16), MEM32(st));
        }
        if (now - t0 >= 5000) {
            if (hle_on())   /* our handles are not XAPI device records */
                fprintf(stderr, "[PADTRACE] %u XInputGetState calls in %lu ms; handle %08X\n",
                        calls, (unsigned long)(now - t0), dev);
            else
                fprintf(stderr, "[PADTRACE] %u XInputGetState calls in %lu ms; handle %08X pkt=%u "
                        "data %08X %08X %08X\n", calls, (unsigned long)(now - t0), dev,
                        MEM32(dev + 8), MEM32(dev + 0x14), MEM32(dev + 0x18), MEM32(dev + 0x1C));
            calls = 0;
            t0 = now;
        }
    }
}

/* XGetDeviceChanges(DeviceType, pdwInsertions, pdwRemovals) -> BOOL */
extern void sub_001F530B_gen(void);
void sub_001F530B(void)
{
    uint32_t type = ARG(0), ins = ARG(1), rem = ARG(2);
    sub_001F530B_gen();
    if (trace_on() && g_eax)
        fprintf(stderr, "[PADTRACE] XGetDeviceChanges(type %08X): insertions %08X removals %08X\n",
                type, ins ? MEM32(ins) : 0, rem ? MEM32(rem) : 0);
}

/* XGetDevices(DeviceType) -> bitmask of connected ports */
extern void sub_001F52E9_gen(void);
void sub_001F52E9(void)
{
    uint32_t type = ARG(0);
    sub_001F52E9_gen();
    if (trace_on())
        fprintf(stderr, "[PADTRACE] XGetDevices(type %08X) = %08X\n", type, g_eax);
}

/* XInputSetState(hDevice, pFeedback) -> DWORD.
 *
 * Replaced, not traced: XAPI sends the rumble report down the pad's interrupt
 * OUT pipe and completes it later, setting pFeedback->Header.dwStatus and the
 * header's event. That completion never arrives from the emulated OHCI, and
 * the title waits on the event with INFINITE (sub_00018F20, its rumble helper):
 * the game froze the first time Harry was hit (washing-machine duel). Here the
 * rumble goes to the PC's pad and the request completes at once.
 *
 * XINPUT_FEEDBACK: header { dwStatus, hEvent, Reserved[58] } at 0, then
 * XINPUT_RUMBLE { wLeftMotorSpeed, wRightMotorSpeed } at 0x42. */
extern void xbox_GuestSetEvent(uint32_t handle);

/* RECOMP_RUMBLE_TRACE=1: every rumble request with the time, the title's caller and what the PC's XInput said */
static int rumble_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *t = getenv("RECOMP_RUMBLE_TRACE");
        on = t && *t && *t != '0';
    }
    return on;
}

/* On the console a request sets the motors' speeds until the next one; the title holds a rumble by sending the same
 * speeds every frame or two, which changes nothing. Only a change goes on to the PC's pad, as the title sent it. */
extern void app_InputRumble(uint16_t *left, uint16_t *right);  /* scda/app/input.cpp: vibration settings */

static DWORD host_rumble(DWORD port, XBOX_VIBRATION v)
{
    static XBOX_VIBRATION last[4];
    static int sent[4];
    app_InputRumble(&v.wLeftMotorSpeed, &v.wRightMotorSpeed);
    if (port >= 4) return xbox_InputSetState(port, &v);
    if (sent[port] && v.wLeftMotorSpeed == last[port].wLeftMotorSpeed &&
        v.wRightMotorSpeed == last[port].wRightMotorSpeed)
        return ERROR_SUCCESS;
    last[port] = v;
    sent[port] = 1;
    return xbox_InputSetState(port, &v);
}

void sub_001F528F(void)
{
    uint32_t fb = ARG(1);
    uint32_t caller = MEM32(g_esp);
    DWORD host = 0;
    XBOX_VIBRATION v = {0, 0};
    int port = hle_on() ? handle_port(ARG(0)) : 0;
    uint32_t event;

    if (fb) {
        v.wLeftMotorSpeed = MEM16(fb + 0x42);
        v.wRightMotorSpeed = MEM16(fb + 0x44);
        host = host_rumble(port < 0 ? 0 : (DWORD)port, v);
        event = MEM32(fb + 4);
        MEM32(fb) = 0;              /* ERROR_SUCCESS */
        if (event)
            xbox_GuestSetEvent(event);
    }
    if (trace_on())
        fprintf(stderr, "[PADTRACE] XInputSetState(%08X) rumble %04X %04X\n", ARG(0), v.wLeftMotorSpeed, v.wRightMotorSpeed);
    if (rumble_trace_on())
        fprintf(stderr, "[RUMBLE] %10.3f s from %08X left %04X right %04X host %lu\n", GetTickCount64() / 1000.0, caller,
                v.wLeftMotorSpeed, v.wRightMotorSpeed, (unsigned long)host);
    g_eax = 997;                    /* ERROR_IO_PENDING, as XAPI returns */
    g_esp += 4 + 8;
}
