/**
 * Display timing the D3D8 HLE no longer gets from the XDK's vblank interrupt.
 *
 * HP: Philosopher's Stone (XDK 5659) plays its XMV movies through a texture
 * the title draws itself, not the NV2A overlay, so unlike the Chamber of
 * Secrets port nothing here converts overlay frames. What remains is the
 * device's vblank counter (+0x1DE8), which the XDK's interrupt advances and
 * which the HLE CreateDevice (scda/xdk/hle_d3d8.cpp) never connects:
 *
 *   D3DDevice_GetDisplayFieldStatus   the XMV player waits on its field count
 *   D3DDevice_GetOverlayUpdateStatus  the loading loop: "a vblank passed since
 *                                     the last overlay update" (always, here)
 *
 * Every entry point keeps the generated body as sub_X_gen, used when
 * RECOMP_HLE_D3D=0 (the XDK's own Direct3D on the emulated NV2A).
 * RECOMP_XMV_TRACE=1 logs the field status once a second.
 *
 * The movie player, sub_00164450(name, ., ., per-frame callback), plays the
 * main menu's moving background ("movies/frontend.thp" -> frontend.xmv) for as
 * long as the menu is up; hpps_MenuMovie() tells d3d12.cpp's Squeeze2D so that,
 * in 16:9, it shows that picture's middle 360 lines across the full width
 * instead of the 4:3 frame between bars (RECOMP_MENU_MOVIE_CROP=0: left 4:3).
 * The loading screen (sprites/load_hognight.tga, set up by sub_00148400) is
 * drawn by its own thread, sub_00148100, looping until the level is in;
 * hpps_LoadingScreen() gives its picture the same crop (RECOMP_LOADING_CROP=0:
 * left 4:3).
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
#include <math.h>

/* the register names (eax, esp, ...) the runtime macros expand to */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

#define ARG(i)  MEM32(g_esp + 4 + 4 * (i))

extern uint32_t xbox_Nv2aVblankCount(void);

static int d3d_hle(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_HLE_D3D");
        on = !(e && *e == '0');
    }
    return on;
}

static int trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *t = getenv("RECOMP_XMV_TRACE");
        on = t && *t && *t != '0';
    }
    return on;
}

/* The movie player's loop (sub_00164450 at 0x164630) is the XDK's own:
 * DirectSoundDoWork, XMVDecoder_GetNextFrame, and round again at once on
 * XMV_NOFRAME. GetNextFrame polls the field status first thing (its call
 * returns to 0x1C449E), so a poll that sees the field it saw last time cannot
 * have a new frame due: wait for the next field there (Sleep is 1 ms, the
 * timer thread set timeBeginPeriod(1)). Spinning instead held the guest core
 * flat out, and the kernel timer thread that counts the fields shares that
 * core: on Windows it got the CPU only between the spinner's quanta, so the
 * movie clock stalled and the intro movies and the menu ran at a few frames a
 * second (1.2 s frames in a user's log). RECOMP_XMV_WAIT=0: spin as before. */
#define XMV_FIELD_POLL_RET 0x001C449Eu

static int xmv_wait_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_XMV_WAIT");
        on = !(e && *e == '0');
    }
    return on;
}

static uint32_t xmv_field_wait(void)
{
    static uint32_t last = 0xFFFFFFFFu;
    uint32_t v = xbox_Nv2aVblankCount();
    int ms;
    /* bounded: with no vblank clock (RECOMP_VBLANK unset) the count never moves */
    for (ms = 0; v == last && ms < 20; ms++) {
        Sleep(1);
        v = xbox_Nv2aVblankCount();
    }
    last = v;
    return v;
}

/* D3DDevice_GetDisplayFieldStatus(pFieldStatus): { Field, VBlankCount }. The
 * runtime's 60 Hz count, progressive (field 3, as the XDK reports when the mode
 * is not interlaced). */
extern void sub_001934C0_gen(void);
void sub_001934C0(void)
{
    static DWORD t0;
    uint32_t p = ARG(0);
    if (d3d_hle() && p) {
        MEM32(p + 4) = MEM32(g_esp) == XMV_FIELD_POLL_RET && xmv_wait_on()
                           ? xmv_field_wait() : xbox_Nv2aVblankCount();
        MEM32(p) = 3;
        g_esp += 8;
    } else {
        sub_001934C0_gen();
    }
    if (trace_on() && p) {
        DWORD now = GetTickCount();
        if (now - t0 >= 1000) {
            fprintf(stderr, "[XMV] field status: field=%u vblank=%u\n", MEM32(p), MEM32(p + 4));
            t0 = now;
        }
    }
}

/* D3DDevice_GetOverlayUpdateStatus(): TRUE once a vblank has passed since the
 * last UpdateOverlay. The title never shows an overlay, so always. */
extern void sub_0019D910_gen(void);
void sub_0019D910(void)
{
    if (!d3d_hle()) {
        sub_0019D910_gen();
        return;
    }
    g_eax = 1;
    g_esp += 4;
}

/* xboxrecomp: true 16:9. The engine's camera setter, Camera_SetScale(cam, xs, ys):
 * the projection's x and y scales at cam+0xE4 / cam+0xE8 (read back by
 * 0x16A480 / 0x16A492). Every camera passes ys = xs * 4/3 (the world camera
 * from its horizontal field of view at 0x210FC4, the menu cameras 2.4 / 3.2),
 * so in 16:9 a 4:3 pair gets xs * 3/4: the same vertical view, wider at the
 * sides (Hor+). The 640x480 frame is shown 16:9 wide and d3d12.cpp's Squeeze2D
 * keeps the 2D at its 4:3 shape. */
extern int hpps_Widescreen(void);
extern uint32_t hpps_FovPercent(void);  /* scda/app/app.cpp: 80 to 130, 100 as the game */
extern void sub_0016A4A0_gen(void);
void sub_0016A4A0(void)
{
    uint32_t cam = ARG(0);
    float xs = MEMF(g_esp + 8), ys = MEMF(g_esp + 12);
    {   /* [CAM] once per camera and caller: which camera a screen uses (the inventory's, a cutscene's) */
        static uint32_t seen[64][2];
        static int n;
        uint32_t caller = MEM32(g_esp);
        int i = 0;
        while (i < n && !(seen[i][0] == cam && seen[i][1] == caller)) i++;
        if (i == n && n < 64) {
            seen[n][0] = cam, seen[n][1] = caller, n++;
            fprintf(stderr, "[CAM] cam %08X from %08X xs %g ys %g\n", cam, caller, xs, ys);
        }
    }
    /* field of view (F10 Graphics, % of the game's): the world camera's scales divided by it, the view widened (or
     * narrowed) the same each way; the engine culls with the same scales (read back by 0x16A480 / 0x16A492), so what
     * comes into view is drawn. The menus' cameras (theirs below, and the 2.4 / 3.2 pairs) keep their framing. This
     * title has no cutscene test: the slider holds in cutscenes too */
    int menu_cam = cam && (cam == MEM32(0x34F998) || cam == MEM32(0x34F944));
    int menu_scale = fabsf(xs - 2.4f) < 0.001f || fabsf(xs - 3.2f) < 0.001f;
    float fov_k = (menu_cam || menu_scale || !(xs > 0.0f)) ? 1.0f : (float)hpps_FovPercent() / 100.0f;
    if (!d3d_hle() || !hpps_Widescreen() || !(xs > 0.0f) || fabsf(ys / xs - 4.0f / 3.0f) > 0.01f) {
        if (fov_k == 1.0f || !(xs > 0.0f) || !(ys > 0.0f)) {
            sub_0016A4A0_gen();
            return;
        }
        MEMF(cam + 0xE4) = xs / fov_k;
        MEMF(cam + 0xE8) = ys / fov_k;
        g_esp += 4;
        return;
    }
    /* the menus' own cameras (the Remembrall inventory's, made after remember.hog loads, pointer at 0x34F998, and
     * its sibling at 0x34F944) stay 4:3: their 3D (the hourglass, the books) is laid out with the menu's 2D. Tagged
     * for d3d12.cpp's Squeeze2D by an x scale two float steps up (hpps_MenuScale), which nothing else draws with. */
    if (cam && (cam == MEM32(0x34F998) || cam == MEM32(0x34F944))) {
        MEMF(cam + 0xE4) = nextafterf(nextafterf(xs, INFINITY), INFINITY);
        MEMF(cam + 0xE8) = ys;
        g_esp += 4;
        return;
    }
    MEMF(cam + 0xE4) = xs * 0.75f / fov_k;
    MEMF(cam + 0xE8) = ys / fov_k;
    g_esp += 4;
}

/* the main menu's background movie is playing */
static volatile int g_menu_movie;

int hpps_MenuMovie(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_MENU_MOVIE_CROP");
        on = !(e && *e == '0');
    }
    return on && g_menu_movie;
}

extern void sub_00164450_gen(void);
void sub_00164450(void)
{
    uint32_t name = ARG(0);
    int menu = name && _stricmp((const char *)XBOX_PTR(name), "movies/frontend.thp") == 0;
    if (menu) g_menu_movie = 1;
    sub_00164450_gen();
    if (menu) g_menu_movie = 0;
}

/* the loading screen's thread is drawing it */
static volatile int g_loading;

int hpps_LoadingScreen(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_LOADING_CROP");
        on = !(e && *e == '0');
    }
    return on && g_loading;
}

/* loading screens so far (d3d12.cpp's RECOMP_CAP_LOADING captures the first frame of each that shows its
 * picture) */
static volatile int g_loads;
int hpps_LoadingCount(void) { return g_loads; }

/* the icons text puts in its lines ("[[CROSS ICON]]" ... "[[R2 ICON]]", table 0x211F28): the controller's sprites
 * (sub_00050060: "<sprite>_icon.tga"), their records at 0x331638 + 4 * i, each with its D3D texture at +0x20.
 * Some lines ("Press [B], [Y] or [X] button to test the wand.") draw them through a header of their own over the
 * same pixels (8045BC80 for the record's 8045B820, both Data 8045B880): matched by Data (D3D resource +4) too */
int hpps_TextIcon(uint32_t tex)
{
    if (!tex) return 0;
    const uint32_t data = MEM32(tex + 4);
    for (uint32_t i = 0; i < 12; i++) {
        uint32_t rec = MEM32(0x331638 + 4 * i);
        if (rec && (rec + 0x20 == tex || (data && MEM32(rec + 0x20 + 4) == data))) return 1;
    }
    return 0;
}

/* debug: the icons' textures now, for a [WIDE] line */
void hpps_TextIconDump(void)
{
    fprintf(stderr, "[WIDE]   text icons:");
    for (uint32_t i = 0; i < 12; i++) {
        uint32_t rec = MEM32(0x331638 + 4 * i);
        fprintf(stderr, " %08X", rec ? rec + 0x20 : 0);
    }
    fprintf(stderr, "\n");
}

extern void sub_00148100_gen(void);
void sub_00148100(void)
{
    g_loads++;
    g_loading = 1;
    sub_00148100_gen();
    g_loading = 0;
}
