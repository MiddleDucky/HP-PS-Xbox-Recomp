/**
 * The title's animation start, sub_00142DB0(set, anim, first, last, rate, blend):
 * plays frames first..last of animation `anim` on an animation set at `rate`
 * frames a second (15 for most), blending in over `blend`. Stored on the set's
 * state (+0x18): +0x24 first, +0x26 last, +0x28 the current frame, +0x2A the
 * animation, +0x2C the rate.
 *
 * RECOMP_ANIM_TRACE=1 logs [ANIM] once per call site and arguments, with the
 * seconds since boot: which call plays which animation (finding the one to
 * retime).
 *
 * Opening a door is slow on the console too (about 7 s). Its state machine
 * sub_000D86D0 (the door's animations, then Harry's back to normal) mostly waits
 * for its animations to end, and Harry's push starts at 0xD7332 (sub_000D7040):
 * those animations play RECOMP_DOOR_SPEED times as fast (default 1.8, 1 = the
 * console's).
 *
 * RECOMP_FOOTSTEP_TRACE=1 logs [STEP] for each call of the script command Footstep (sub_0004AE80 ->
 * sub_000DDA80(entity)): the entity's name, the surface [0x356704] (1..12 plays, else silent) and whether that
 * surface has a sound table ([0x342E08 + 0x1C * surface]).
 *
 * The same switch logs [QUAKE] for each troll impact, sub_000BA050(seconds offset, strength): the troll's club or
 * charge hitting the ground or a tree (callers 0xBCDF3, 0xBD161) shakes the camera and, while [0x33ED38] is set,
 * makes Harry stumble ([0x356594] = end time against the clock [0x356598]).
 *
 * Built into the recompile with --exclude-manual naming this file.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

#define ARG(i)  MEM32(g_esp + 4 + 4 * (i))
#define ARGF(i) MEMF(g_esp + 4 + 4 * (i))

static int trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *t = getenv("RECOMP_ANIM_TRACE");
        on = t && *t && *t != '0';
    }
    return on;
}

static void trace(void)
{
    enum { kSeen = 4096 };
    static uint32_t seen[kSeen][5];
    static int n;
    uint32_t caller = MEM32(g_esp), anim = ARG(1), first = ARG(2), last = ARG(3), rate = ARG(4);
    for (int i = 0; i < n; i++) {
        if (seen[i][0] == caller && seen[i][1] == anim && seen[i][2] == first && seen[i][3] == last &&
            seen[i][4] == rate)
            return;
    }
    if (n < kSeen) {
        seen[n][0] = caller, seen[n][1] = anim, seen[n][2] = first, seen[n][3] = last, seen[n][4] = rate;
        n++;
    }
    /* the set's animation name (inline at +0x1C) and its data (+0x14; 0: none loaded, nothing to play) */
    uint32_t set = ARG(0);
    int named = set >= 0x80000000u && set < 0x84000000u;
    fprintf(stderr, "[ANIM] %8.2f s from %08X set %08X '%.24s' data %08X anim %u frames %u-%u rate %g blend %g\n",
            GetTickCount64() / 1000.0, caller, set, named ? (const char *)XBOX_PTR(set + 0x1C) : "?",
            named ? MEM32(set + 0x14) : 0, anim, first, last, ARGF(4), ARGF(5));
}

static float door_speed(void)
{
    static float k = -1;
    if (k < 0) {
        const char *e = getenv("RECOMP_DOOR_SPEED");
        k = e && *e ? (float)atof(e) : 1.8f;
        if (!(k > 0)) k = 1;
    }
    return k;
}

extern void sub_00142DB0_gen(void);
void sub_00142DB0(void)
{
    uint32_t caller = MEM32(g_esp);
    if (trace_on()) trace();
    /* a door opening (the caller's argument slot is its own: cdecl, popped by it after) */
    if ((caller >= 0x000D86D0u && caller < 0x000D9260u) || caller == 0x000D7332u) ARGF(4) *= door_speed();
    sub_00142DB0_gen();
}

static int step_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *t = getenv("RECOMP_FOOTSTEP_TRACE");
        on = t && *t && *t != '0';
    }
    return on;
}

static int guest_ptr(uint32_t p) { return p >= 0x80000000u && p < 0x84000000u; }

extern void sub_000DDA80_gen(void);
void sub_000DDA80(void)
{
    static int lines;
    if (step_trace_on() && lines < 4000) {
        uint32_t ent = ARG(0), s = MEM32(0x356704), name = guest_ptr(ent) ? MEM32(ent + 0x0C) : 0;
        uint32_t table = s > 0 && s < 13 ? MEM32(0x342E08 + 0x1C * s) : 0;
        lines++;
        fprintf(stderr, "[STEP] %8.2f s entity %08X '%.32s' surface %u table %08X\n", GetTickCount64() / 1000.0, ent,
                guest_ptr(name) ? (const char *)XBOX_PTR(name) : "?", s, table);
    }
    sub_000DDA80_gen();
}

/*
 * The Remembrall's fog (issue #16): sub_00036AF0 moves its 160 cloud sprites, each position (x, y at 0x32E97C,
 * 8 bytes apart) by its velocity 0x500 further on, once per call and with no time step, wrapping at 640 x 480
 * (sub_00036A10 draws them). Its drawing, sub_00045A30, calls it once a frame, so the clouds drifted as fast as frames
 * came: twice as fast at 60 fps as at 30, and many times faster unlocked. That call (returning to 0x45A5E) moves them
 * by the time the frame took instead, measured in the title's frames (hpps_TitleFrameSeconds): the velocities are
 * scaled for the call and put back after. The set-up, sub_00040B40, pre-steps the clouds in a loop; those calls are
 * steps, not frames, and stay as they are. At the game's own frame rate nothing changes.
 */
extern double hpps_TitleFrameSeconds(void); /* scda/xdk/hle_d3d8.cpp */

#define FOG_COUNT 160
#define FOG_VEL   0x0032EE7Cu  /* x, y per cloud */

extern void sub_00036AF0_gen(void);
void sub_00036AF0(void)
{
    static LARGE_INTEGER freq, last;
    LARGE_INTEGER now;
    double frame = hpps_TitleFrameSeconds(), dt, k;
    float vel[FOG_COUNT * 2];

    if (MEM32(g_esp) != 0x00045A5Eu) {
        sub_00036AF0_gen();
        return;
    }
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    dt = last.QuadPart ? (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart : 0.0;
    last = now;
    /* the game's own rate, or the Remembrall just opened (a long gap since the last frame of fog): one step */
    if (frame <= 0.0 || dt <= 0.0 || dt > 0.25) {
        sub_00036AF0_gen();
        return;
    }
    k = dt / frame;
    if (k > 4.0) k = 4.0;
    for (int i = 0; i < FOG_COUNT * 2; i++) {
        vel[i] = MEMF(FOG_VEL + 4 * i);
        MEMF(FOG_VEL + 4 * i) = (float)(vel[i] * k);
    }
    sub_00036AF0_gen();
    for (int i = 0; i < FOG_COUNT * 2; i++) MEMF(FOG_VEL + 4 * i) = vel[i];
}

/*
 * The aiming reticle (issue #22): sub_00012EB0 places the circle round the locked target (the player controller's
 * +0x2564) with a turn of 0x32036C radians and then adds 0.05 to the turn, once per call and with no time step,
 * unless it leaves early at 0x12F56 (a test against [0x1FABB8]: no turn that call). The run of 2026-11-16 19:30
 * measured it at 60 fps: 25 calls a second from 0x17492, the 25 Hz tick, so 1.25 rad/s, the console's speed at any
 * frame rate. The scaling of 1c23cd2 (one turn per 1/59.94 s, from a per-frame reading) made it 3 rad/s, 2.4 times
 * too fast, and is gone: the game turns it as it always did. Unlocked, a 4 s aim logged no turn at all, so the
 * speed there is still to be measured: [RETICLE] once a second while aiming counts calls per site, the calls that
 * turned and those that left early, and the game's rad/s, beside the frame counter [0x3562E8].
 */
#define RETICLE_TURN 0x0032036Cu

extern void sub_00012EB0_gen(void);
void sub_00012EB0(void)
{
    static LARGE_INTEGER freq, t0;
    static uint32_t site_ret[2], ctr0;
    static unsigned calls[2], turned[2], why[4];  /* left early: a flag, Harry's state, no target, too far */
    static double rad;
    const uint32_t ret = MEM32(g_esp);
    const float before = MEMF(RETICLE_TURN);
    LARGE_INTEGER now;
    int i;

    /* the function's own early exits, read before it runs (0x12EB0 .. 0x12F47) */
    const uint32_t ctl = MEM32(MEM32(0x003565B0u) + 0xDC);
    const uint32_t hstate = ctl ? MEM32(ctl + 0x2564) : 0;
    const int flag = MEM8(0x0032038Cu) || MEM8(0x0032038Du) || MEM8(0x0032038Fu) || MEM8(0x00320392u) ||
                     MEM8(0x00356588u) || MEM8(0x003203C8u);
    const int state = hstate == 0x12 || hstate == 0x2A || hstate == 0x14 || hstate == 0x26;
    const int no_target = MEM32(0x00320398u) == 0;

    sub_00012EB0_gen();
    const float step = MEMF(RETICLE_TURN) - before;
    if (step == 0.0f) why[flag ? 0 : state ? 1 : no_target ? 2 : 3]++;  /* else past 100 units: the lock let go */

    for (i = 0; i < 2 && site_ret[i] && site_ret[i] != ret; i++) {}
    if (i == 2) i = 1;
    site_ret[i] = ret;
    calls[i]++;
    if (step != 0.0f) turned[i]++, rad += step;

    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (!t0.QuadPart) { t0 = now; ctr0 = MEM32(0x003562E8u); }
    const double el = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    if (el > 3.0) {  /* not aiming for a while: start a fresh second */
        t0 = now;
        ctr0 = MEM32(0x003562E8u);
        calls[0] = calls[1] = turned[0] = turned[1] = why[0] = why[1] = why[2] = why[3] = 0;
        rad = 0.0;
        calls[i]++;
        if (step != 0.0f) turned[i]++, rad += step;
        else why[flag ? 0 : state ? 1 : no_target ? 2 : 3]++;
    } else if (el >= 1.0) {
        static int trace = -1;  /* RECOMP_RETICLE_TRACE=1: the line below once a second while aiming */
        if (trace < 0) {
            const char *e = getenv("RECOMP_RETICLE_TRACE");
            trace = e && *e && *e != '0';
        }
        if (trace)
            fprintf(stderr, "[RETICLE] %.2f s: site %05X x%u (turned %u, left early %u), site %05X x%u (turned %u, "
                        "left early %u) | early: flag %u, Harry's state %u (now %X), no target %u, past 100 units %u | "
                        "game %.2f rad/s | frames [0x3562E8] +%u\n", el, site_ret[0], calls[0], turned[0],
                calls[0] - turned[0], site_ret[1], calls[1], turned[1], calls[1] - turned[1], why[0], why[1], hstate,
                why[2], why[3], rad / el, MEM32(0x003562E8u) - ctr0);
        t0 = now;
        ctr0 = MEM32(0x003562E8u);
        calls[0] = calls[1] = turned[0] = turned[1] = why[0] = why[1] = why[2] = why[3] = 0;
        rad = 0.0;
    }
}

/*
 * The Lumos moths round the wand (issue #23): sub_000F70E0 runs once a frame while Lumos is on (+0x17C set) and
 * moves each moth (+0x150 of them, 0x30 bytes each at +0x154) with no time step:
 *     v = 0.95 v + 0.05 d;  p += v          (v at +0x00, unit heading d at +0x0C, p at +0x20)
 * Its target timer (+0x1C) and wing phase (+0x2C) already use the frame time [0x351534]. Unlocked, the moths flew
 * as fast as frames came. Before the game's step this sets v and p so that the step lands where n steps of 1/59.94 s
 * would, n being the real time since this object's last call in 60 fps frames: with u = d (= 0.05 d / (1 - 0.95)),
 *     v' = u + 0.95^n (v - u),   p' = p + n u + (v - u) 0.95 (1 - 0.95^n) / 0.05
 * which is the game's own step when n = 1. A second call in the same frame gets n = 0 and leaves the moths still.
 * A long gap (Lumos just cast) is one step, as before.
 */
#define MOTH_ON 0x17Cu
#define MOTH_COUNT 0x150u
#define MOTH_ARRAY 0x154u
#define MOTH_SIZE 0x30u

extern void sub_000F70E0_gen(void);
void sub_000F70E0(void)
{
    static LARGE_INTEGER freq;
    static struct { uint32_t obj; LARGE_INTEGER last; } clk[4];
    static int next;
    const uint32_t obj = MEM32(g_esp + 4);
    const int update = MEM8(g_esp + 0xC) != 0;
    LARGE_INTEGER now;
    double dt = -1.0, n, an;
    uint32_t count, arr, i;
    int c;

    if (update && obj && MEM32(obj + MOTH_ON) && (count = MEM32(obj + MOTH_COUNT)) != 0 &&
        (arr = MEM32(obj + MOTH_ARRAY)) != 0) {
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        for (c = 0; c < 4 && clk[c].obj != obj; c++) {}
        if (c == 4) { c = next; next = (next + 1) & 3; clk[c].last.QuadPart = 0; }
        else dt = (double)(now.QuadPart - clk[c].last.QuadPart) / (double)freq.QuadPart;
        clk[c].obj = obj;
        clk[c].last = now;
        if (dt >= 0.0 && dt <= 0.25 && count <= 256) {
            n = dt * 60000.0 / 1001.0;  /* in 60 fps frames */
            an = pow(0.95, n);
            for (i = 0; i < count; i++) {
                const uint32_t r = arr + i * MOTH_SIZE;
                for (int ax = 0; ax < 3; ax++) {
                    const double v = MEMF(r + 4 * ax), u = MEMF(r + 0x0C + 4 * ax), p = MEMF(r + 0x20 + 4 * ax);
                    const double v1 = u + an * (v - u);
                    const double p1 = p + n * u + (v - u) * 0.95 * (1.0 - an) / 0.05;
                    MEMF(r + 4 * ax) = (float)((v1 - 0.05 * u) / 0.95);  /* the game's step makes this v1 */
                    MEMF(r + 0x20 + 4 * ax) = (float)(p1 - v1);           /* and adds v1 to it */
                }
            }
        }
    }
    sub_000F70E0_gen();
}

extern void sub_000BA050_gen(void);
void sub_000BA050(void)
{
    if (step_trace_on())
        fprintf(stderr, "[QUAKE] %8.2f s from %08X for %g strength %g level %u flag %u\n", GetTickCount64() / 1000.0,
                MEM32(g_esp), ARGF(0), ARGF(1), MEM32(0x356CC4), MEM8(0x33ED38));
    sub_000BA050_gen();
}

/*
 * The aiming circle's spin (issue #22), measured where it is drawn: the renderer (d3d12.cpp Squeeze2D) knows it as a
 * single turned quad and passes its turn (the first corner's angle round the middle) on every such draw, on the game's
 * thread. [CIRCLE] once a second while it is drawn: draws, the turn per draw and per second, and who drew it (the
 * return addresses up the guest's frame chain, then code addresses found on the stack), to find the step to retime.
 */
void hpps_CircleDraw(float angle)
{
    static ULONGLONG since;
    static unsigned draws;
    static float last, turned, most;
    static int have;
    static char who[256];
    ULONGLONG now = GetTickCount64();
    if (have) {
        float d = angle - last;
        while (d > 3.14159265f) d -= 6.28318531f;
        while (d < -3.14159265f) d += 6.28318531f;
        turned += d;
        if (fabsf(d) > fabsf(most)) most = d;
    }
    last = angle, have = 1;
    if (!draws) {
        int n = 0;
        uint32_t ebp = g_ebp;
        n += snprintf(who + n, sizeof who - n, "frames");
        for (int k = 0; k < 6 && ebp >= 0x00010000u && ebp < 0x04000000u && n < 120; k++, ebp = MEM32(ebp))
            n += snprintf(who + n, sizeof who - n, " %08X", MEM32(ebp + 4));
        n += snprintf(who + n, sizeof who - n, " | stack");
        for (uint32_t p = g_esp, k = 0; k < 64 && n < 240; k++, p += 4) {
            uint32_t v = MEM32(p);
            if (v >= 0x00011000u && v < 0x001C0000u) n += snprintf(who + n, sizeof who - n, " %08X", v);
        }
    }
    draws++;
    if (!since) since = now;
    if (now - since >= 1000) {
        float s = (now - since) / 1000.0f;
        fprintf(stderr, "[CIRCLE] %u draws in %.2f s: turned %.0f deg/s, %.1f deg a draw (most %.1f) | %s\n", draws, s,
                turned * 57.29578f / s, draws > 1 ? turned * 57.29578f / (draws - 1) : 0.0f, most * 57.29578f, who);
        since = now, draws = 0, turned = 0, most = 0;
    }
}

/*
 * The aiming circle's spin (issue #22). The circle is one turned quad drawn by sub_001535E0 (called through a pointer
 * once a frame while a target is locked; found by [CIRCLE] and the F7 dumps of 2026-11-16, it alone calls the four
 * functions that only ran while aiming). Each draw it turns [0x21BF5C] (degrees, wrapped at 360) by
 * ticks a second [0x21B7B8] x the tick's length [0x21B7BC] x 10 [0x1FAC6C]: 25 x 0.04 x 10 = 10 degrees, a step
 * a step per draw: 600 degrees a second at 60 fps and 1440 unlocked (measured). The console drew it at NTSC's
 * 59.94 frames a second (the user's choice, 2026-11-17, as the Lumos moths: 250 a second, a step per 25 Hz tick,
 * looked too slow), so the step here is the game's scaled to the time since the last draw in NTSC frames: 599
 * degrees a second at any frame rate. The angle is set so the game's own add lands on it (and put back if the call
 * left before turning it). A fix of the game's own frame-rate mistake: always on.
 */
extern void sub_001535E0_gen(void);

#define CIRCLE_ANGLE   0x0021BF5Cu
#define CIRCLE_TICKS   0x0021B7B8u
#define CIRCLE_TICKLEN 0x0021B7BCu
#define CIRCLE_K       0x001FAC6Cu

void sub_001535E0(void)
{
    static LARGE_INTEGER freq, last;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    double dt = last.QuadPart ? (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart : 0.0;
    last = now;
    const float tick = MEMF(CIRCLE_TICKLEN);
    const float game_step = (float)MEM32(CIRCLE_TICKS) * tick * MEMF(CIRCLE_K);
    if (!(tick > 0.0f) || !(game_step > 0.0f) || game_step > 90.0f) {
        sub_001535E0_gen();
        return;
    }
    if (dt > 0.25) dt = 1.0 / 59.94;  /* the first draw of a lock-on, or after a pause: one console frame's worth */
    const float before = MEMF(CIRCLE_ANGLE);
    const float pre = before + game_step * (float)(dt * 59.94) - game_step;
    MEMF(CIRCLE_ANGLE) = pre;
    sub_001535E0_gen();
    if (MEMF(CIRCLE_ANGLE) == pre) MEMF(CIRCLE_ANGLE) = before;
}
