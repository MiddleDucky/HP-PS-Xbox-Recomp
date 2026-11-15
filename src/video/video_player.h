/**
 * Shared FMV player: Media Foundation decode onto a D3D8 texture.
 *
 * For a title whose video is a container Windows can already decode, the
 * decoder does not have to be emulated for the video to be watchable. The
 * recompiled title still decides when: it opens the file as its own logic
 * dictates, and the runtime plays the file it opened.
 */

#ifndef XBOXRECOMP_VIDEO_PLAYER_H
#define XBOXRECOMP_VIDEO_PLAYER_H

#include <stdint.h>

/* Initialize/shutdown Media Foundation (call once at app start/end) */
int  video_init(void);
void video_shutdown(void);

/* Open a video file for playback. Returns 0 on success. */
int  video_open(const char *path);

/* Advance playback by dt seconds, decode next frame if needed.
 * Returns 1 if a new frame is ready, 0 if unchanged, -1 if finished. */
int  video_update(float dt);

/* Render the current video frame as a fullscreen quad.
 * Call between BeginScene/EndScene. */
void video_render(void);

/* Check if the video has finished playing. */
int  video_is_finished(void);

/* Close the current video and release resources. */
void video_close(void);

/* === Boot sequence state machine === */

/* Boot phases */

/* Get current boot phase */

/* Advance boot state machine. Call once per frame.
 * skip=1 if user pressed a button to skip current video.
 * Returns the new phase. */

/* Render current boot phase (video frame or press-start screen). */


/* Play one video file, start to finish, in its own window on its own thread.
 * Returns 0 if the pump started. Blocking work happens on that thread, so the
 * guest carries on running while the video plays. */
int  xbox_VideoPlayFile(const char *host_path);
int  xbox_VideoIsPlaying(void);

/* Write the current decoded frame to a 24-bit BMP -- evidence that real
 * pixels reached the renderer, which a log line cannot give. */
int  video_dump_frame_bmp(const char *path);

/* Show the guest framebuffer in its own window (RECOMP_FB_WINDOW). Whatever
 * the title renders into guest RAM appears there; nothing else scans it out. */
/* Non-zero while that virtual key is held in the framebuffer window. Always
 * zero when there is no window, which is the right answer: with nothing to
 * focus there is nothing to type into. */
int xbox_FramebufferKeyDown(int vk);

void xbox_FramebufferWindowStart(void);
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
int  xbox_FramebufferDumpBmp(const char *path);
/* Title bar: "(XBOX Recomp) <game name>". SetGameName (UTF-8, from the port, at
 * start) wins; otherwise SetTitle gives the XBE certificate's UTF-16 title
 * (40 chars max), ignored when empty; with neither, "Xbox Recomp".
 * FrameStats is kept for its callers and shows nothing. */
void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars);
void xbox_FramebufferWindowSetGameName(const char *utf8);
void xbox_FramebufferWindowFrameStats(uint32_t draws);
/* The NV2A video overlay (PVIDEO), which the CRTC mixes over the framebuffer
 * at scan-out: XMV movies are shown this way, not drawn into the framebuffer.
 * The caller converts each frame to 32-bit XRGB (w x h, tightly packed); the
 * window scales it into the destination rectangle (display pixels) on top of
 * whatever the framebuffer holds, until the overlay is switched off. */
void xbox_FramebufferWindowOverlay(const uint32_t *xrgb, uint32_t w, uint32_t h,
                                   int dst_x, int dst_y, int dst_w, int dst_h);
void xbox_FramebufferWindowOverlayOff(void);

/* GPU presentation (src/kernel/nv2a_gpu_d3d11.c). aspect: 0 4:3, 1 stretch,
 * 2 16:9 (see fb_window_size). The overlay fields are valid when ov_on. */
typedef struct {
    int aspect;
    uint32_t fb_w, fb_h;
    int ov_on;
    const uint32_t *ov;
    uint32_t ov_w, ov_h;
    int ov_x, ov_y, ov_dw, ov_dh;
    uint32_t ov_version;
} XboxPresentInfo;
typedef int (*xbox_fb_presenter)(void *hwnd, const XboxPresentInfo *info);
void xbox_FramebufferWindowSetPresenter(xbox_fb_presenter fn);
void xbox_FramebufferWindowFlipped(void);
void *xbox_FramebufferWindowHwnd(void);
void xbox_FramebufferWindowOverlayRead(void (*fn)(const XboxPresentInfo *, void *), void *ctx);
/* Display mode of the window: XBOX_DISPLAY_WINDOWED (a w x h client area,
 * centred), XBOX_DISPLAY_BORDERLESS (covers its monitor) or
 * XBOX_DISPLAY_FULLSCREEN (the monitor switched to w x h, then covered; the
 * desktop mode when w or h is 0). Any thread; the window thread applies it.
 * RECOMP_DISPLAY_MODE=windowed|borderless|fullscreen picks the first one. */
enum { XBOX_DISPLAY_WINDOWED, XBOX_DISPLAY_BORDERLESS, XBOX_DISPLAY_FULLSCREEN };
void xbox_FramebufferWindowSetDisplay(int mode, int w, int h);
/* Every window message goes to fn first, on the window thread; nonzero
 * returned means fn took it (an options panel with keyboard focus) and the
 * title does not see it. */
typedef int (*xbox_fb_msg_hook)(void *hwnd, unsigned msg, uintptr_t wp, intptr_t lp);
void xbox_FramebufferWindowSetMsgHook(xbox_fb_msg_hook fn);

#endif /* BURNOUT3_VIDEO_PLAYER_H */
