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

void sub_00014ED0(void)
{
    if (!hpps_FreeCamera()) {
        g_turnable = 0;
        sub_00014ED0_gen();
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
        g_pitch += g_rate * 1.8f * frame_dt();
        if (g_pitch > 1.3f) g_pitch = 1.3f;
        if (g_pitch < -1.3f) g_pitch = -1.3f;
    }
    sub_00014ED0_gen();
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
