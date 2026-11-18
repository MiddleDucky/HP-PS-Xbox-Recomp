/**
 * Free camera (F10 Enhancements, "Free camera"): the right stick's (and the mouse's) up and down tilt the play camera
 * up and down around Harry, as its left and right already turn it round him.
 *
 * What the console does (found for this):
 *   sub_00014ED0      the camera stick, once a frame from the chase camera's step sub_00017180, while the player
 *                     may turn the camera: [0x31F5D0] == 1, [0x32038E] set, [0x32038F] and [0x320378] clear and
 *                     Harry's state ([[0x3565B0]+0xDC]+0x2564) not 0x14. Its X action ([0x356BE0] into the input
 *                     table [[0x356520]+0x10], 24 bytes an action) turns the camera round its focus. Its Y action
 *                     ([0x356C20]) has no pitch: past +-0.8 it steps a zoom index [0x210FF4] (0..3) over the table
 *                     at 0x210FC8 (-0.4 -0.2 0.1 0.6) into [0x320368], which slides the camera along the area's
 *                     camera rail ([0x320280], near-low to far-high): closer or further, not up or down.
 *   sub_00016450      the chase camera's view: from the position 0x320358 looking at the focus 0x32026C, onto the
 *                     camera object [0x356CCC]; the frame's last camera step (sub_00011F40 runs sub_00017180 first)
 *   sub_000138E0(from, to)  the camera's collision probe: `to` pulled in front of a camera blocker on the way from
 *                     `from`; true when it moved it
 *
 * On: the Y action is read as at rest by the stick handler (no zoom steps; the zoom stays where it is), and the stick's
 * Y (hpps_last_pad: keyboard, mouse and the invert setting already in) moves a pitch of our own. For the view only,
 * the position is swung up or down round the focus by it, through the probe and kept above Harry's feet, and put back
 * after: the game's camera (its turning, springs and collision) never sees the change. When the player cannot turn
 * the camera (cutscenes, locked and scripted cameras) the pitch eases back to the game's own.
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

#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"
#include "input/xinput_xbox.h"

extern void sub_00014ED0_gen(void);
extern void sub_00016450_gen(void);
extern void sub_000138E0(void);
extern int hpps_FreeCamera(void);   /* scda/app/app.cpp */
extern float app_MouseBoostX(void);  /* scda/app/input.cpp: the mouse past the stick's full tilt (or the controller
                                     * camera speed while the right stick turns), 1..4 */
extern float app_MouseBoostY(void);
#define ACT_CAM_X  0x00356BE0u
extern XBOX_GAMEPAD hpps_last_pad;  /* hle_input.c */

#define CAM_POS    0x00320358u  /* x y z w */
#define CAM_FOCUS  0x0032026Cu
#define PLAYER     0x003565B0u
#define FRAME_TIME 0x0021B7BCu
#define ACTIONS    0x00356520u
#define ACT_CAM_Y  0x00356C20u

static float g_pitch;    /* radians added to the camera's elevation, + higher */
static float g_rate;     /* smoothed stick */
static int g_turnable;   /* the stick handler found the camera the player's this frame */

static int guest_ptr(uint32_t p) { return p >= 0x80000000u && p < 0x84000000u; }

static float frame_dt(void)
{
    float dt = MEMF(FRAME_TIME);
    return dt > 0 && dt < 0.25f ? dt : 1 / 60.0f;
}

/* the stick handler's own test for "the player turns the camera now" */
static int camera_turnable(void)
{
    uint32_t player = MEM32(PLAYER), ctrl;
    if (MEM32(0x0031F5D0u) != 1 || !MEM8(0x0032038Eu) || MEM8(0x0032038Fu) || MEM8(0x00320378u)) return 0;
    if (!guest_ptr(player)) return 0;
    ctrl = MEM32(player + 0xDC);
    return guest_ptr(ctrl) && MEM32(ctrl + 0x2564) != 20;
}

/* the camera's X action (turning round Harry) times the mouse's boost past the stick's full tilt, or the controller
 * camera speed, for the stick handler's run only (2026-11-16: mouse sensitivity 100 was slow, the stick's full turn
 * the most it could ask) */
static void boosted_stick(void)
{
    uint32_t table = MEM32(ACTIONS), x_at = 0;
    float x_was = 0, k = app_MouseBoostX();
    if (k > 1.0f && guest_ptr(table) && guest_ptr(MEM32(table + 16))) {
        x_at = MEM32(table + 16) + 24 * MEM32(ACT_CAM_X);
        x_was = MEMF(x_at);
        MEMF(x_at) = x_was * k;
    }
    sub_00014ED0_gen();
    if (x_at) MEMF(x_at) = x_was;
}

void sub_00014ED0(void)
{
    if (!hpps_FreeCamera()) {
        g_turnable = 0;
        boosted_stick();
        return;
    }
    g_turnable = camera_turnable();
    uint32_t table = MEM32(ACTIONS), y_at = 0;
    float y_was = 0;
    if (guest_ptr(table) && guest_ptr(MEM32(table + 16))) {
        y_at = MEM32(table + 16) + 24 * MEM32(ACT_CAM_Y);
        y_was = MEMF(y_at);
        MEMF(y_at) = 0;  /* no zoom steps */
    }
    if (g_turnable) {
        float y = hpps_last_pad.sThumbRY / 32767.0f;
        /* the stick's up looks up: the camera goes down */
        g_rate += (-y - g_rate) * 0.35f;
        g_pitch += g_rate * 1.8f * app_MouseBoostY() * frame_dt();
        if (g_pitch > 1.3f) g_pitch = 1.3f;
        if (g_pitch < -1.3f) g_pitch = -1.3f;
    }
    boosted_stick();
    if (y_at) MEMF(y_at) = y_was;
}

/* the camera position swung by g_pitch round the focus, through the collision probe */
static void pitched(float out[4])
{
    float f[3] = {MEMF(CAM_FOCUS), MEMF(CAM_FOCUS + 4), MEMF(CAM_FOCUS + 8)};
    float o[3] = {MEMF(CAM_POS) - f[0], MEMF(CAM_POS + 4) - f[1], MEMF(CAM_POS + 8) - f[2]};
    float h = sqrtf(o[0] * o[0] + o[2] * o[2]), r = sqrtf(h * h + o[1] * o[1]);
    out[0] = MEMF(CAM_POS), out[1] = MEMF(CAM_POS + 4), out[2] = MEMF(CAM_POS + 8), out[3] = MEMF(CAM_POS + 12);
    if (h < 1e-3f || r < 1e-3f) return;
    float e0 = atan2f(o[1], h), e = e0 + g_pitch;
    /* from 30 degrees under the focus to 80 over it (no further than the game's own, if it is already past) */
    float lo = -0.52f, hi = 1.40f;
    /* not below Harry's feet: his object's current world matrix ([player+0x28]: +0x40 or +0x80 by +0x2C) */
    uint32_t player = MEM32(PLAYER), obj = guest_ptr(player) ? MEM32(player + 0x28) : 0;
    if (guest_ptr(obj)) {
        float feet = MEMF(obj + 0x40 + 64 * (MEM32(obj + 0x2C) & 1) + 52);
        float s = (feet + 25.0f - f[1]) / r;
        if (s > -1 && s < 1 && asinf(s) > lo) lo = asinf(s);
    }
    if (lo > e0) lo = e0;
    if (hi < e0) hi = e0;
    if (e < lo) e = lo;
    if (e > hi) e = hi;
    float k = r * cosf(e) / h;
    out[0] = f[0] + o[0] * k, out[1] = f[1] + r * sinf(e), out[2] = f[2] + o[2] * k;

    /* the probe on a copy below the guest stack: sub_000138E0(from = the focus, to = the copy) */
    uint32_t s_esp = g_esp, s_ebx = g_ebx, s_esi = g_esi, s_edi = g_edi, s_ebp = g_ebp;
    uint32_t to = (g_esp - 64) & ~15u;
    for (int i = 0; i < 4; i++) MEMF(to + 4 * i) = out[i];
    g_esp = to;
    g_esp -= 4, MEM32(g_esp) = to;
    g_esp -= 4, MEM32(g_esp) = CAM_FOCUS;
    g_esp -= 4, MEM32(g_esp) = 0;  /* return address */
    sub_000138E0();
    for (int i = 0; i < 4; i++) out[i] = MEMF(to + 4 * i);
    g_esp = s_esp, g_ebx = s_ebx, g_esi = s_esi, g_edi = s_edi, g_ebp = s_ebp;
}

void sub_00016450(void)
{
    int on = hpps_FreeCamera();
    if (!on || !g_turnable) {
        /* back to the game's own: at once when off, eased otherwise */
        g_pitch = on ? g_pitch * expf(-6.0f * frame_dt()) : 0;
        g_rate = 0;
        if (fabsf(g_pitch) < 1e-3f) g_pitch = 0;
    }
    g_turnable = 0;
    if (g_pitch == 0) {
        sub_00016450_gen();
        return;
    }
    uint32_t keep[4];
    float p[4];
    for (int i = 0; i < 4; i++) keep[i] = MEM32(CAM_POS + 4 * i);
    pitched(p);
    for (int i = 0; i < 4; i++) MEMF(CAM_POS + 4 * i) = p[i];
    sub_00016450_gen();
    for (int i = 0; i < 4; i++) MEM32(CAM_POS + 4 * i) = keep[i];
}

/*
 * Free first-person look (F10 Enhancements "Free first-person look"; by request, 2026-11-16). The look on White is
 * sub_00019950, once a camera tick while it is up (the only look-only caller in the F7 dumps of 2026-11-16): it eases
 * its yaw and pitch toward the stick, yaw += 0.08 (stick X * 0.7 - yaw), pitch the same from stick Y, and turns the
 * view round Harry's head by them. So the stick's position is the angle: 40 degrees at most, back ahead when let go.
 * Here the stick turns our own yaw and pitch instead (2.5 and 1.8 radians a second at full tilt, the mouse's boost
 * too); yaw unbounded, pitch within 75 degrees. Each tick the game's yaw and pitch and its two stick actions are set
 * so that its easing lands exactly on ours (old = ours, stick = ours / 0.7), and the game builds the view as ever.
 * A look begun after a gap starts where the game's is (ahead).
 *
 * Smooth (2026-11-16, "not very smooth"): the game runs its camera on the 25 Hz tick (sub_00011FB0 228 calls in
 * 9.1 s of the F7 dumps) and nothing eases it between ticks, so a fast turn moved the view in 40 ms steps. The look
 * also runs, with the same step, before each frame's drawing: sub_00140330 (PAL; 1309 calls in the same 9.1 s, once
 * a frame) while the camera is the look's ([0x31F5D0] == 2, sub_00011FB0's switch) and the tick ran it in the last
 * 100 ms. The look takes no arguments and reads only globals (Harry's head, the stick actions), so running it more
 * often changes nothing else; the yaw and pitch now move every frame.
 */
extern int hpps_FreeLook(void);    /* scda/app/app.cpp */
extern int hpps_LookToggle(void);  /* scda/app/app.cpp */
extern void sub_00019950_gen(void);
extern void sub_00140330_gen(void);

#define LOOK_YAW    0x0032084Cu
#define LOOK_PITCH  0x00320848u
#define ACT_LOOK_X  0x00356C94u
#define ACT_LOOK_Y  0x00356C28u

static ULONGLONG g_look_ms;  /* when the tick last ran the look (the White switch lets go once it stops) */

/* our yaw and pitch on by the time since the last step, then the game's look with its easing landing on them */
static void free_look_step(void)
{
    static LARGE_INTEGER freq, last;
    static float yaw, pitch, vyaw, vpitch;  /* angles, and their eased turning speeds (radians a second) */
    LARGE_INTEGER now;
    uint32_t table = MEM32(ACTIONS);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    double dt = last.QuadPart ? (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart : 1.0;
    last = now;
    if (dt > 0.3) {  /* a new look: from where the game's is */
        yaw = MEMF(LOOK_YAW);
        pitch = MEMF(LOOK_PITCH);
        vyaw = vpitch = 0;
        dt = 0;
    }
    uint32_t ax = MEM32(table + 16) + 24 * MEM32(ACT_LOOK_X), ay = MEM32(table + 16) + 24 * MEM32(ACT_LOOK_Y);
    float sx = MEMF(ax), sy = MEMF(ay);
    /* the turning speed eases toward the stick's (0.1 s to most of the way), starting and stopping, as the third
     * person camera does: the stick's speed taken at once turned stiffly and stopped dead (2026-11-17) */
    const float ease = dt > 0 ? 1.0f - expf(-(float)dt / 0.1f) : 0.0f;
    vyaw += (sx * 2.5f * app_MouseBoostX() - vyaw) * ease;
    vpitch += (sy * 1.8f * app_MouseBoostY() - vpitch) * ease;
    yaw += vyaw * (float)dt;
    pitch += vpitch * (float)dt;
    if (yaw > 3.14159265f) yaw -= 6.28318531f;
    if (yaw < -3.14159265f) yaw += 6.28318531f;
    if (pitch > 1.3f) pitch = 1.3f, vpitch = vpitch > 0 ? 0 : vpitch;
    if (pitch < -1.3f) pitch = -1.3f, vpitch = vpitch < 0 ? 0 : vpitch;
    MEMF(LOOK_YAW) = yaw, MEMF(LOOK_PITCH) = pitch;
    MEMF(ax) = yaw / 0.7f, MEMF(ay) = pitch / 0.7f;
    sub_00019950_gen();
    MEMF(ax) = sx, MEMF(ay) = sy;
}

static int free_look_ready(void)
{
    uint32_t table = MEM32(ACTIONS);
    return hpps_FreeLook() && guest_ptr(table) && guest_ptr(MEM32(table + 16));
}

void sub_00019950(void)
{
    g_look_ms = GetTickCount64();
    if (!free_look_ready()) {
        sub_00019950_gen();
        return;
    }
    free_look_step();
}

/* each frame's drawing: the look stepped first while it is up (its own return address, registers as before) */
void sub_00140330(void)
{
    if (free_look_ready() && MEM32(0x0031F5D0u) == 2 && GetTickCount64() - g_look_ms < 100) {
        uint32_t s_esp = g_esp, s_eax = g_eax, s_ebx = g_ebx, s_ecx = g_ecx, s_edx = g_edx, s_esi = g_esi,
                 s_edi = g_edi, s_ebp = g_ebp;
        g_esp -= 4;
        MEM32(g_esp) = 0;
        free_look_step();
        g_esp = s_esp, g_eax = s_eax, g_ebx = s_ebx, g_ecx = s_ecx, g_edx = s_edx, g_esi = s_esi, g_edi = s_edi;
        g_ebp = s_ebp;
    }
    sub_00140330_gen();
}

/*
 * White as a switch (F10 Enhancements "White switches the look on and off"; by request, 2026-11-16): the console looks
 * only while White is held. Each press flips a switch, and while it is on the game reads White as held. A press still
 * reaches the game as itself (menus keep White as before). The switch lets go by itself when the game's look has not
 * run for half a second (a cutscene, a menu or anything else ended it). hle_input.c, port 0, every read.
 */
uint8_t hpps_LookWhite(uint8_t white)
{
    static int was, on;
    static ULONGLONG since;
    if (!hpps_LookToggle()) {
        on = 0;
        return white;
    }
    ULONGLONG now = GetTickCount64();
    int down = white > 30;
    if (down && !was) on = !on, since = now;
    was = down;
    if (on && now - since > 500 && now - g_look_ms > 500) on = 0;
    return on || down ? 255 : 0;
}
