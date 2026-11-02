/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>
#include "video_player.h"

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * Two buffers and an index, swapped after the copy completes, so the window
 * thread is never reading the one being filled. */
static uint32_t     *s_present[2];
static volatile LONG s_present_idx = -1;   /* -1 until the first flip */

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

static void ov_on_flip(void);

/* The GPU path's flip: the frame stays on the GPU, so there is nothing to
 * copy -- only the overlay's hide-on-flip to honour. */
void xbox_FramebufferWindowFlipped(void)
{
    ov_on_flip();
}

/* A presenter replaces the GDI blit when the frame lives on the GPU
 * (src/kernel/nv2a_gpu_d3d11.c). Called on the window thread, with the
 * overlay locked for the duration. */
static xbox_fb_presenter s_presenter;

void xbox_FramebufferWindowSetPresenter(xbox_fb_presenter fn)
{
    s_presenter = fn;
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height,
                                             4);
        if (!s_present[next])
            return;
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    /* Published only once it is whole. */
    InterlockedExchange(&s_present_idx, next);
    ov_on_flip();
}

/* The video overlay: the newest movie frame and where it goes. Written by the
 * guest's UpdateOverlay, read by the window thread, under one lock -- a frame
 * is 1.2 MB and is copied once per update, so holding it is cheap. */
static CRITICAL_SECTION s_ov_lock;
static volatile LONG    s_ov_lock_init;
static uint32_t        *s_ov;
static uint32_t         s_ov_w, s_ov_h, s_ov_cap;
static int              s_ov_on, s_ov_x, s_ov_y, s_ov_dw, s_ov_dh;
/* Switched off, but still showing its last frame until the title flips.
 *
 * A title plays its movies back to back without drawing in between, so the
 * framebuffer still holds whatever it last rendered -- here the legal screen,
 * which flashed up for a moment between one movie and the next. On the
 * console the gap is the movie's own last (black) frame for as long as the
 * next one takes to open; keeping the overlay up until the title presents
 * something new reproduces that, and a menu that does draw replaces it at
 * its first flip. */
static int              s_ov_hide_on_flip;
static uint32_t         s_ov_version;      /* bumped by every new frame */

static void ov_lock_init(void)
{
    if (InterlockedCompareExchange(&s_ov_lock_init, 1, 0) == 0) {
        InitializeCriticalSection(&s_ov_lock);
        InterlockedExchange(&s_ov_lock_init, 2);
    }
    while (s_ov_lock_init != 2)
        Sleep(0);
}

void xbox_FramebufferWindowOverlay(const uint32_t *xrgb, uint32_t w, uint32_t h,
                                   int dst_x, int dst_y, int dst_w, int dst_h)
{
    if (!xrgb || !w || !h)
        return;
    ov_lock_init();
    EnterCriticalSection(&s_ov_lock);
    if (w * h > s_ov_cap) {
        free(s_ov);
        s_ov = (uint32_t *)malloc((size_t)w * h * 4);
        s_ov_cap = s_ov ? w * h : 0;
    }
    if (s_ov) {
        memcpy(s_ov, xrgb, (size_t)w * h * 4);
        s_ov_w = w; s_ov_h = h;
        s_ov_x = dst_x; s_ov_y = dst_y;
        s_ov_dw = dst_w > 0 ? dst_w : (int)s_fb_width;
        s_ov_dh = dst_h > 0 ? dst_h : (int)s_fb_height;
        s_ov_on = 1;
        s_ov_hide_on_flip = 0;
        s_ov_version++;
    }
    LeaveCriticalSection(&s_ov_lock);
}

void xbox_FramebufferWindowOverlayOff(void)
{
    ov_lock_init();
    EnterCriticalSection(&s_ov_lock);
    if (s_ov_on)
        s_ov_hide_on_flip = 1;
    LeaveCriticalSection(&s_ov_lock);
}

static void ov_on_flip(void)
{
    if (!s_ov_lock_init)
        return;
    EnterCriticalSection(&s_ov_lock);
    if (s_ov_hide_on_flip) {
        s_ov_on = 0;
        s_ov_hide_on_flip = 0;
    }
    LeaveCriticalSection(&s_ov_lock);
}

/* How the frame fills the window; see fb_window_size(). */
enum { ASPECT_4_3, ASPECT_STRETCH, ASPECT_16_9 };
static int s_aspect;

/* Scale the overlay into its rectangle of the image about to be shown. In
 * 16:9 the frame is anamorphic, so the rectangle is squeezed to 3/4 of its
 * width about the centre, and the stretch to the window gives it back its
 * 4:3 shape. */
static void ov_composite(uint32_t *rgb)
{
    int x, y, ox, odw;

    if (!s_ov_lock_init)
        return;
    EnterCriticalSection(&s_ov_lock);
    ox = s_ov_x;
    odw = s_ov_dw;
    if (s_aspect == ASPECT_16_9) {
        int c = (int)s_fb_width / 2;
        ox = c + (s_ov_x - c) * 3 / 4;
        odw = s_ov_dw * 3 / 4;
    }
    if (s_ov_on && s_ov && odw > 0) {
        for (y = 0; y < s_ov_dh; y++) {
            int dy = s_ov_y + y;
            const uint32_t *src;
            if (dy < 0 || dy >= (int)s_fb_height)
                continue;
            src = s_ov + (size_t)((uint64_t)y * s_ov_h / (uint32_t)s_ov_dh) * s_ov_w;
            for (x = 0; x < odw; x++) {
                int dx = ox + x;
                if (dx < 0 || dx >= (int)s_fb_width)
                    continue;
                rgb[(size_t)dy * s_fb_width + dx] =
                    src[(uint64_t)x * s_ov_w / (uint32_t)odw];
            }
        }
    }
    LeaveCriticalSection(&s_ov_lock);
}

/* Which keys are down, for the pad stand-in in src/input.
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them.
 *
 * Reading this needs no lock. Each entry is written only by the window
 * thread and read only by the USB thread, one byte at a time, and a press
 * seen a frame late is indistinguishable from one made a frame later. */
static volatile unsigned char s_key_down[256];

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i)
        s_title[i] = 0;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk > 255)
        return 0;
    return s_key_down[vk] != 0;
}

static volatile xbox_fb_msg_hook s_msg_hook;

void xbox_FramebufferWindowSetMsgHook(xbox_fb_msg_hook fn)
{
    s_msg_hook = fn;
}

#define WM_FB_DISPLAY (WM_APP + 1)
static void fb_apply_display(HWND h);
static void fb_end_process(void);

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    xbox_fb_msg_hook hook = s_msg_hook;
    if (hook && hook(h, m, (uintptr_t)w, (intptr_t)l))
        return 1;
    switch (m) {
    case WM_FB_DISPLAY:
        fb_apply_display(h);
        return 0;

    /* xboxrecomp: Alt+F4 or the close button ends the game, as the dashboard would. Only stopping this window
     * left the title, render and audio threads running behind a dead (white) window. */
    case WM_CLOSE:
        fb_end_process();
        return 0;

    case WM_DESTROY:
        InterlockedExchange(&s_fb_running, 0);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if ((unsigned)w < 256)
            s_key_down[w] = 1;
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if ((unsigned)w < 256)
            s_key_down[w] = 0;
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* F10 or a bare Alt released: DefWindowProc puts the window in menu mode, a modal loop that holds this thread --
     * the presenter with it, so the whole title stops -- until the next key or click. Alt+Space (l = ' ') still opens
     * the system menu. */
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_KEYMENU && l == 0)
            return 0;
        break;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        memset((void *)s_key_down, 0, sizeof s_key_down);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch;
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

/* Window size and aspect.
 *
 * RECOMP_WINDOW_SIZE=<w>x<h> sets the window's client size (default: the
 * framebuffer's, 640x480). RECOMP_ASPECT picks how the image fills it:
 * "4:3" (the default) scales it as large as fits and centres it between black
 * bars, so a 1920x1080 window shows 1440x1080 with nothing stretched;
 * "stretch" fills the whole window; "16:9" is widescreen: the title renders
 * an anamorphic frame (its own 16:9 camera, 2D squeezed by the pushbuffer
 * executor) that is stretched to the largest 16:9 rectangle that fits, and a
 * full-screen movie is drawn at 4:3 inside it, between black bars. Either way
 * the image follows the window when it is resized or maximised. */

static void fb_window_size(int *w, int *h)
{
    const char *sz = getenv("RECOMP_WINDOW_SIZE");
    const char *asp = getenv("RECOMP_ASPECT");
    int ww = 0, hh = 0;

    if (sz && sscanf(sz, "%dx%d", &ww, &hh) == 2 && ww >= 160 && hh >= 120) {
        *w = ww;
        *h = hh;
    } else {
        *w = (int)s_fb_width;
        *h = (int)s_fb_height;
    }
    s_aspect = !asp ? ASPECT_4_3
             : !_stricmp(asp, "stretch") ? ASPECT_STRETCH
             : !strcmp(asp, "16:9") ? ASPECT_16_9 : ASPECT_4_3;
}

static HWND s_hwnd;

/* Display mode: windowed, borderless or fullscreen, see video_player.h. */
static volatile LONG s_disp_mode = -1, s_disp_w, s_disp_h;
static int  s_mode_changed;        /* ChangeDisplaySettingsEx switched a monitor */
static char s_mode_device[CCHDEVICENAME];

/* the window was closed: the desktop's mode back, then the process ends at once. TerminateProcess, not
 * ExitProcess: ExitProcess runs DLL teardown while the render thread may sit inside the D3D12 driver, and can hang
 * there -- a close that does not close. What the title wrote (saves) is already with the OS; it never waits on a
 * clean shutdown either (HalReturnToFirmware ends the process too). */
static void fb_end_process(void)
{
    fprintf(stderr, "[EXIT] game window closed (Alt+F4 / close button)\n");
    fflush(stdout);
    fflush(stderr);
    if (s_mode_changed)
        ChangeDisplaySettingsExA(s_mode_device, NULL, NULL, 0, NULL);
    TerminateProcess(GetCurrentProcess(), 0);
}

static void fb_display_from_env(void)
{
    const char *m = getenv("RECOMP_DISPLAY_MODE");
    int ww = 0, hh = 0;
    if (s_disp_mode >= 0)
        return;
    fb_window_size(&ww, &hh);
    s_disp_w = ww;
    s_disp_h = hh;
    s_disp_mode = !m ? XBOX_DISPLAY_WINDOWED
                : !_stricmp(m, "borderless") ? XBOX_DISPLAY_BORDERLESS
                : !_stricmp(m, "fullscreen") ? XBOX_DISPLAY_FULLSCREEN
                : XBOX_DISPLAY_WINDOWED;
}

/* Window thread. The renderer's original ApplyDisplay (src/app/main.cpp): the previous mode
 * change undone first, the desktop's own size is no mode change. */
static void fb_apply_display(HWND h)
{
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXA mi;
    int mode = (int)s_disp_mode, w = (int)s_disp_w, hh = (int)s_disp_h;
    LONG_PTR visible = IsWindowVisible(h) ? WS_VISIBLE : 0;
    DEVMODEA desk;

    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (s_mode_changed) {
        ChangeDisplaySettingsExA(s_mode_device, NULL, NULL, 0, NULL);
        s_mode_changed = 0;
    }
    GetMonitorInfoA(mon, (MONITORINFO *)&mi);
    memset(&desk, 0, sizeof(desk));
    desk.dmSize = sizeof(desk);
    EnumDisplaySettingsA(mi.szDevice, ENUM_REGISTRY_SETTINGS, &desk);
    if (mode == XBOX_DISPLAY_FULLSCREEN && w > 0 && hh > 0 &&
        ((DWORD)w != desk.dmPelsWidth || (DWORD)hh != desk.dmPelsHeight)) {
        DEVMODEA dm;
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        dm.dmPelsWidth = (DWORD)w;
        dm.dmPelsHeight = (DWORD)hh;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        s_mode_changed = ChangeDisplaySettingsExA(mi.szDevice, &dm, NULL, CDS_FULLSCREEN, NULL)
                         == DISP_CHANGE_SUCCESSFUL;
        if (!s_mode_changed)
            fprintf(stderr, "  [FBWIN] %dx%d not available on %s, desktop mode kept\n", w, hh, mi.szDevice);
        strncpy(s_mode_device, mi.szDevice, sizeof(s_mode_device) - 1);
        GetMonitorInfoA(mon, (MONITORINFO *)&mi);
    }
    if (mode == XBOX_DISPLAY_WINDOWED) {
        RECT r, a = mi.rcWork;
        int ow, oh;
        if (w <= 0 || hh <= 0) { w = (int)s_fb_width * 2; hh = (int)s_fb_height * 2; }
        r.left = 0; r.top = 0; r.right = w; r.bottom = hh;
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        ow = r.right - r.left;
        oh = r.bottom - r.top;
        SetWindowLongPtrA(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | visible);
        SetWindowPos(h, HWND_NOTOPMOST, a.left + (a.right - a.left - ow) / 2,
                     a.top + (a.bottom - a.top - oh) / 2, ow, oh, SWP_FRAMECHANGED);
    } else {
        RECT r = mi.rcMonitor;
        SetWindowLongPtrA(h, GWL_STYLE, WS_POPUP | visible);
        SetWindowPos(h, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED);
    }
    fprintf(stderr, "  [FBWIN] display %s %dx%d\n",
            mode == XBOX_DISPLAY_WINDOWED ? "windowed" : mode == XBOX_DISPLAY_BORDERLESS ? "borderless" : "fullscreen",
            w, hh);
}

void xbox_FramebufferWindowSetDisplay(int mode, int w, int h)
{
    InterlockedExchange(&s_disp_w, w);
    InterlockedExchange(&s_disp_h, h);
    InterlockedExchange(&s_disp_mode, mode);
    if (s_hwnd)
        PostMessageA(s_hwnd, WM_FB_DISPLAY, 0, 0);
}

/* The largest num:den rectangle that fits a cw x ch client, centred. */
static void fb_fit(int cw, int ch, int num, int den, RECT *out)
{
    int dw, dh;
    if ((long long)cw * den > (long long)ch * num) {
        dh = ch; dw = (int)((long long)ch * num / den);
    } else {
        dw = cw; dh = (int)((long long)cw * den / num);
    }
    out->left = (cw - dw) / 2;
    out->top = (ch - dh) / 2;
    out->right = out->left + dw;
    out->bottom = out->top + dh;
}

/* Black everywhere in the client except r. */
static void fb_bars(HDC hdc, int cw, int ch, const RECT *r)
{
    if (r->left > 0)    PatBlt(hdc, 0, 0, r->left, ch, BLACKNESS);
    if (r->right < cw)  PatBlt(hdc, r->right, 0, cw - r->right, ch, BLACKNESS);
    if (r->top > 0)     PatBlt(hdc, r->left, 0, r->right - r->left, r->top, BLACKNESS);
    if (r->bottom < ch) PatBlt(hdc, r->left, r->bottom, r->right - r->left, ch - r->bottom, BLACKNESS);
}

/* 16:9 with a full-screen movie up: draw the movie frame straight to the
 * window at 4:3 -- its own resolution, not squeezed into the 640-wide frame
 * and stretched back out -- and black around it. Returns 0 when the overlay
 * is off or only covers part of the screen, which is composited as usual. */
static int ov_blit_direct(HWND hwnd, HDC hdc)
{
    RECT c, img, mv;
    int cw, ch, done = 0;

    if (s_aspect != ASPECT_16_9 || !s_ov_lock_init)
        return 0;
    GetClientRect(hwnd, &c);
    cw = c.right - c.left;
    ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0)
        return 0;
    EnterCriticalSection(&s_ov_lock);
    if (s_ov_on && s_ov && s_ov_x <= 0 && s_ov_y <= 0
        && s_ov_x + s_ov_dw >= (int)s_fb_width && s_ov_y + s_ov_dh >= (int)s_fb_height) {
        BITMAPINFO ob;
        fb_fit(cw, ch, 16, 9, &img);
        fb_fit(img.right - img.left, img.bottom - img.top, 4, 3, &mv);
        OffsetRect(&mv, img.left, img.top);
        memset(&ob, 0, sizeof(ob));
        ob.bmiHeader.biSize        = sizeof(ob.bmiHeader);
        ob.bmiHeader.biWidth       = (LONG)s_ov_w;
        ob.bmiHeader.biHeight      = -(LONG)s_ov_h;
        ob.bmiHeader.biPlanes      = 1;
        ob.bmiHeader.biBitCount    = 32;
        ob.bmiHeader.biCompression = BI_RGB;
        fb_bars(hdc, cw, ch, &mv);
        StretchDIBits(hdc, mv.left, mv.top, mv.right - mv.left, mv.bottom - mv.top,
                      0, 0, (int)s_ov_w, (int)s_ov_h, s_ov, &ob, DIB_RGB_COLORS, SRCCOPY);
        done = 1;
    }
    LeaveCriticalSection(&s_ov_lock);
    return done;
}

/* Scale the frame into the window's client area, with black bars in 4:3. */
static void fb_blit(HWND hwnd, HDC hdc, const BITMAPINFO *bi)
{
    RECT c, r;
    int cw, ch;

    GetClientRect(hwnd, &c);
    cw = c.right - c.left;
    ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0)
        return;
    if (s_aspect == ASPECT_STRETCH)
        SetRect(&r, 0, 0, cw, ch);
    else if (s_aspect == ASPECT_16_9)
        fb_fit(cw, ch, 16, 9, &r);
    else
        fb_fit(cw, ch, (int)s_fb_width, (int)s_fb_height, &r);
    fb_bars(hdc, cw, ch, &r);
    StretchDIBits(hdc, r.left, r.top, r.right - r.left, r.bottom - r.top,
                  0, 0, (int)s_fb_width, (int)s_fb_height,
                  s_rgb, bi, DIB_RGB_COLORS, SRCCOPY);
}

/* One window refresh: the movie on its own, or the frame with any overlay. */
static void fb_show(HWND hwnd, HDC hdc, const BITMAPINFO *bi)
{
    if (ov_blit_direct(hwnd, hdc))
        return;
    ov_composite(s_rgb);
    fb_blit(hwnd, hdc, bi);
}


/* The window, for a renderer that presents to it from another thread (the
 * D3D8 HLE renderer's swap chain). NULL until the window thread opens it. */
void *xbox_FramebufferWindowHwnd(void)
{
    return (void *)s_hwnd;
}

/* The movie overlay as the window holds it, for a renderer that presents on
 * its own: fn runs with the overlay locked, info->ov_on 0 when it is off. */
void xbox_FramebufferWindowOverlayRead(void (*fn)(const XboxPresentInfo *, void *), void *ctx)
{
    XboxPresentInfo pi;
    memset(&pi, 0, sizeof(pi));
    pi.aspect = s_aspect;
    pi.fb_w = s_fb_width;
    pi.fb_h = s_fb_height;
    ov_lock_init();
    EnterCriticalSection(&s_ov_lock);
    if (s_ov_on && s_ov) {
        pi.ov_on = 1;
        pi.ov = s_ov;
        pi.ov_w = s_ov_w; pi.ov_h = s_ov_h;
        pi.ov_x = s_ov_x; pi.ov_y = s_ov_y;
        pi.ov_dw = s_ov_dw; pi.ov_dh = s_ov_dh;
        pi.ov_version = s_ov_version;
    }
    fn(&pi, ctx);
    LeaveCriticalSection(&s_ov_lock);
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;

    (void)unused;

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        /* xboxrecomp: the exe's own icon (resource 1, e.g. the title image) on the window and taskbar; NULL, the
         * system's default, when the exe has none */
        wc.hIcon         = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
        /* black until the first frame (no brush: Windows shows the window white while the title boots) */
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    {
        int ww, hh;
        fb_window_size(&ww, &hh);
        r.left = 0; r.top = 0; r.right = ww; r.bottom = hh;
    }
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    s_hwnd = hwnd;
    fb_display_from_env();
    if (s_disp_mode != XBOX_DISPLAY_WINDOWED)
        fb_apply_display(hwnd);
    hdc = GetDC(hwnd);
    /* Smooth scaling; COLORONCOLOR would be nearest-neighbour. */
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (s_presenter) {
            XboxPresentInfo pi;
            memset(&pi, 0, sizeof(pi));
            pi.aspect = s_aspect;
            pi.fb_w = s_fb_width;
            pi.fb_h = s_fb_height;
            ov_lock_init();
            EnterCriticalSection(&s_ov_lock);
            if (s_ov_on && s_ov) {
                pi.ov_on = 1;
                pi.ov = s_ov;
                pi.ov_w = s_ov_w; pi.ov_h = s_ov_h;
                pi.ov_x = s_ov_x; pi.ov_y = s_ov_y;
                pi.ov_dw = s_ov_dw; pi.ov_dh = s_ov_dh;
                pi.ov_version = s_ov_version;
            }
            s_presenter(hwnd, &pi);
            LeaveCriticalSection(&s_ov_lock);
        } else if (s_present_idx >= 0 && s_rgb) {
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            LONG idx = s_present_idx;
            if (s_present[idx])
                memcpy(s_rgb, s_present[idx],
                       (size_t)s_fb_width * s_fb_height * 4);
            fb_show(hwnd, hdc, &bi);
        } else if (s_fb_va && s_fb_pitch && s_rgb) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            fb_show(hwnd, hdc, &bi);
        }
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 1000) {
                LONG f = s_flips;
                wchar_t tb[128];
                _snwprintf(tb, 127, L"%ls | FPS: %.1f | draws: %ld", s_title,
                           t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0,
                           (long)s_frame_draws);
                tb[127] = 0;
                SetWindowTextW(hwnd, tb);
                t0 = now; f0 = f;
            }
        }
        /* The GPU presenter only presents what changed; poll it faster so
         * a 60 Hz title is not shown at the window's own 60 Hz phase. */
        Sleep(s_presenter ? 2 : 16);
    }

    if (s_mode_changed)
        ChangeDisplaySettingsExA(s_mode_device, NULL, NULL, 0, NULL);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    /* read once: the input poll reaches here several times a frame (app::Window), and getenv takes the CRT's
     * environment lock and walks the whole table each time */
    static volatile LONG want = -1;
    HANDLE th;

    if (want < 0)
        InterlockedExchange(&want, getenv("RECOMP_FB_WINDOW") ? 1 : 0);
    if (!want || s_fb_running)
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#else
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowSetDisplay(int mode, int w, int h) { (void)mode; (void)w; (void)h; }
void xbox_FramebufferWindowSetMsgHook(xbox_fb_msg_hook fn) { (void)fn; }
int xbox_FramebufferKeyDown(int vk) { (void)vk; return 0; }
void xbox_FramebufferWindowSetTitle(const uint16_t *n, int m) { (void)n; (void)m; }
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowOverlay(const uint32_t *xrgb, uint32_t w, uint32_t h,
                                   int dst_x, int dst_y, int dst_w, int dst_h)
{ (void)xrgb; (void)w; (void)h; (void)dst_x; (void)dst_y; (void)dst_w; (void)dst_h; }
void xbox_FramebufferWindowOverlayOff(void) {}
void xbox_FramebufferWindowSetPresenter(xbox_fb_presenter fn) { (void)fn; }
void *xbox_FramebufferWindowHwnd(void) { return NULL; }
void xbox_FramebufferWindowOverlayRead(void (*fn)(const XboxPresentInfo *, void *), void *ctx)
{
    XboxPresentInfo pi;
    memset(&pi, 0, sizeof(pi));
    fn(&pi, ctx);
}
void xbox_FramebufferWindowFlipped(void) {}
#endif
