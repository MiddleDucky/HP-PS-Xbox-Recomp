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
 * +0x2564) with a turn of 0x32036C radians and then adds 0.05 to the turn, once per call and with no time step.
 * The player and camera update, sub_00017180, calls it once a frame (from one of two branches), so the circle spun
 * as fast as frames came: unlocked many times too fast, and still turning behind the pause menu. It now turns by
 * 0.05 per 1/59.94 s of real time, the speed it has at 60 fps (3 rad/s), whatever the frame rate. Each call site
 * keeps its own clock, so the two branches never share a frame's time. A long gap (the circle just shown again) is
 * one step, as before.
 */
#define RETICLE_TURN 0x0032036Cu

extern void sub_00012EB0_gen(void);
void sub_00012EB0(void)
{
    static LARGE_INTEGER freq;
    static struct { uint32_t ret; LARGE_INTEGER last; } site[2];
    const uint32_t ret = MEM32(g_esp);
    const float before = MEMF(RETICLE_TURN);
    LARGE_INTEGER now;
    double dt, k;
    int i;

    sub_00012EB0_gen();
    const float step = MEMF(RETICLE_TURN) - before;
    if (step == 0.0f) return;  /* no target: nothing turned */

    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    for (i = 0; i < 2 && site[i].ret && site[i].ret != ret; i++) {}
    if (i == 2) i = 1;
    dt = site[i].ret == ret && site[i].last.QuadPart
             ? (double)(now.QuadPart - site[i].last.QuadPart) / (double)freq.QuadPart
             : 0.0;
    site[i].ret = ret;
    site[i].last = now;
    if (dt <= 0.0 || dt > 0.25) return;  /* first turn after a gap: the game's one step */
    k = dt * 60000.0 / 1001.0;           /* in 60 fps frames */
    if (k > 4.0) k = 4.0;
    MEMF(RETICLE_TURN) = (float)(before + step * k);
}

extern void sub_000BA050_gen(void);
void sub_000BA050(void)
{
    if (step_trace_on())
        fprintf(stderr, "[QUAKE] %8.2f s from %08X for %g strength %g level %u flag %u\n", GetTickCount64() / 1000.0,
                MEM32(g_esp), ARGF(0), ARGF(1), MEM32(0x356CC4), MEM8(0x33ED38));
    sub_000BA050_gen();
}
