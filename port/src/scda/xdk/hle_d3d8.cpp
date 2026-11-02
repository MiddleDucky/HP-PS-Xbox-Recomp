// Direct3D 8 (XDK 5659) entry points the title calls, implemented on a host
// D3D12 renderer instead of the emulated NV2A pushbuffer.
//
// Ported from SCDA-XBERecomp's src/xdk/d3d8.cpp (MIT, Copyright (c) 2026
// TGP482, Joshhhuaaa; see ../LICENSE.SCDA), which targets XDK 5849, by way of
// the HP: Chamber of Secrets port (XDK 5028). The addresses, CDevice offsets and argument counts below are this XBE's: the
// entry points from XbSymbolDatabase, the rest read from the functions' own
// code (the comment by each constant says where).
//
// Resource creation, locking and the XG helpers stay recompiled: they only
// manage memory. Everything that would push hardware commands is replaced, and
// what still pushes (anything not caught here) writes into a scratch buffer
// that MakeSpace rewinds, so the pushbuffer executor sees nothing.
//
// Each entry point keeps the generated XDK body as sub_X_gen, so
// RECOMP_HLE_D3D=0 runs the original Direct3D on the emulated NV2A.
// Built into the recompile with --exclude-manual pointing at this file.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

extern "C" {
#include "video/video_player.h"

// the recompiled code's registers (recomp_types.h), one set per guest thread
extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp;
extern __thread uint32_t g_ebx, g_esi, g_edi;
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
void xbox_SetVblankHook(void (*fn)(void));  // kernel_bridge.c
uint32_t xbox_Nv2aVblankCount(void);
}

using namespace kernel;

namespace d3d {

namespace {

// ── This XBE's D3D globals ──────────────────────────────────────────────────

constexpr uint32_t kDevice = 0x001A13E0;       // the static CDevice: Direct3D_CreateDevice stores it to g_pDevice
constexpr uint32_t kDeviceBytes = 0x928 * 4;   // Direct3D_CreateDevice clears this many dwords when Init fails
constexpr uint32_t kDevicePtr = 0x001A13D8;    // D3D__pDevice
constexpr uint32_t kDirtyFlags = 0x001A0F38;   // D3D__DirtyFlags
constexpr uint32_t kTextureState = 0x001A0F40; // D3D__TextureState [4][32]
constexpr uint32_t kRenderState = 0x001A1140;  // D3D__RenderState [166]
constexpr uint32_t kStreams = 0x001A3898;      // g_Stream [16] stride, offset, buffer
constexpr uint32_t kSimpleEncode = 0x001FB2B0; // D3DSIMPLERENDERSTATEENCODE, SetRenderStateNotInline

// recompiled D3D helpers kept as guest code
constexpr uint32_t kDestroyResource = 0x00194A70;  // D3D_DestroyResource
constexpr uint32_t kInitDefaultState = 0x00196D50; // called by CDevice::Init after SetRenderTarget

// CDevice fields mirrored so the recompiled getters and resource code see what the XDK would (5659 layout)
constexpr uint32_t kDevPush = 0x00, kDevThreshold = 0x04;
constexpr uint32_t kDevFence = 0x2C;             // stored into a resource as it is released (SetTexture)
constexpr uint32_t kDevPsTextureModes = 0x790;   // SetRenderState_PSTextureModes
constexpr uint32_t kDevVertexShader = 0x794;     // m_VertexShader, by XbSymbolDatabase
constexpr uint32_t kDevZScale = 0x948;           // the viewport helper 0x190910
constexpr uint32_t kDevSuperX = 0x95C, kDevSuperY = 0x960;
constexpr uint32_t kDevTransforms = 0xC60;       // SetTransform: 10 matrices
constexpr uint32_t kDevViewport = 0xEE0;         // SetViewport's copy, GetViewport reads it
constexpr uint32_t kDevScreenOffset = 0xEF8;     // SetScreenSpaceOffset
constexpr uint32_t kDevTextures = 0xF88;         // SetTexture
constexpr uint32_t kDevPalettes = 0xF98;         // SetPalette
constexpr uint32_t kDevRenderTarget = 0x1A04;    // GetRenderTarget2
constexpr uint32_t kDevDepth = 0x1A08;           // GetDepthStencilSurface2
constexpr uint32_t kDevFrameBuffers = 0x1A14;    // CDevice::Init: back buffer, front buffer, ..., +0xC auto depth
constexpr uint32_t kDevVBlankCallback = 0x1DB8;  // m_VBlankCallback, by XbSymbolDatabase
constexpr uint32_t kDevSwapCount = 0x2478;       // Swap's return

gpu::State g_state;
std::unique_ptr<gpu::Backend> g_backend;
std::recursive_mutex g_lock;

std::unordered_map<uint32_t, std::shared_ptr<gpu::VertexShader>> g_vs;
std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> g_ps;
std::array<uint32_t, 3> g_buffers{};  // front, back surfaces
uint32_t g_buffer_count = 2;
uint32_t g_depth = 0;
uint32_t g_scratch_push = 0;
uint32_t g_interval = 1;  // vertical blanks a frame is shown for, from the presentation parameters

// ── Guest calls ─────────────────────────────────────────────────────────────

// runs a recompiled function from here on the current guest thread: the
// arguments pushed right to left, then a return address, as `call` leaves it
uint32_t CallGuest(uint32_t fn, std::initializer_list<uint32_t> args) {
    uint32_t esp = g_esp, ebx = g_ebx, esi = g_esi, edi = g_edi;
    const uint32_t* a = args.begin();
    for (size_t i = args.size(); i-- > 0;) {
        g_esp -= 4;
        W32(g_esp, a[i]);
    }
    g_esp -= 4;
    W32(g_esp, 0);
    recomp_func_t f = recomp_lookup(fn);
    if (f) f();
    else LOG_FIRST(8, "d3d: no recompiled function at %08X", fn);
    g_esp = esp;  // stdcall or cdecl alike
    g_ebx = ebx;
    g_esi = esi;
    g_edi = edi;
    return g_eax;
}

// the XDK's internal reference count lives in bits 19-22 of Common; the last one
// going (with no external references either) destroys the resource
void AddRef(uint32_t res) {
    if (res) W32(res, R32(res) + 0x80000);
}
void Release(uint32_t res) {
    if (!res) return;
    uint32_t c = R32(res) - 0x80000;
    W32(res, c);
    if (!(c & 0x78FFFF)) CallGuest(kDestroyResource, {res});
}

uint32_t Dev(uint32_t off) { return R32(kDevice + off); }
void SetDev(uint32_t off, uint32_t v) { W32(kDevice + off, v); }
float ArgF(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
float GF(uint32_t va) {
    float f;
    std::memcpy(&f, HostPtr(va), 4);
    return f;
}

gpu::Surface SurfaceOf(uint32_t header) {
    gpu::Surface s;
    s.header = header;
    if (header) {
        s.data = R32(header + 4);
        s.format = R32(header + 12);
        s.size = R32(header + 16);
    }
    return s;
}

uint32_t NewSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp) {
    uint32_t pitch = (w * bpp + 63) & ~63u;
    uint32_t data = mem::AllocContiguous(pitch * h, 0, 0xFFFFFFFF, 0x1000);
    uint32_t hdr = mem::AllocSystem(24);
    W32(hdr + 0, 0x000D0001);  // surface, one reference, one internal reference held by the device
    W32(hdr + 4, mem::PhysicalFromVirtual(data));
    W32(hdr + 8, 0);
    W32(hdr + 12, 0x00010021 | (fmt << 8));  // dma A, 2D, one level
    W32(hdr + 16, (w - 1) | (h - 1) << 12 | (pitch / 64 - 1) << 24);
    W32(hdr + 20, 0);
    return hdr;
}

void SyncDeferred() {
    // deferred states live in the title's memory, the inline SetRenderState and SetTextureStageState write them
    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++) g_state.rs[i] = R32(kRenderState + i * 4);
    for (int s = 0; s < 4; s++)
        for (uint32_t i = 0; i < gpu::kTssBumpEnvMat00; i++) g_state.tss[s][i] = R32(kTextureState + (s * 32 + i) * 4);
}

float ZScale(const gpu::Surface& zs) {
    uint32_t f = zs.header ? (zs.format >> 8) & 0xFF : 0x2E;
    return f == 0x2C || f == 0x30 ? 65535.0f : 16777215.0f;
}

// c-38 and c-37 hold the viewport scale and offset, as D3D_GetViewportOffsetAndScale computes them
void UpdateViewportConstants() {
    auto& v = g_state.viewport;
    g_state.zscale = ZScale(g_state.zs);
    float hw = v.w * 0.5f, hh = v.h * 0.5f;
    g_state.vsc[58] = {hw, -hh, (v.max_z - v.min_z) * g_state.zscale, 0};
    g_state.vsc[59] = {v.x + g_state.ss_offset_x + hw, v.y + g_state.ss_offset_y + hh, v.min_z * g_state.zscale, 0};
}

void SetComplex(uint32_t rs, uint32_t v) {
    g_state.rs[rs] = v;
    W32(kRenderState + rs * 4, v);
}

// frame pacing on the runtime's vblank counter (kernel_bridge.c, 59.94 Hz): Swap returns once g_interval vblanks have
// passed since the last one, as the console flips at a vblank. A title that already waited for one
// (BlockUntilVerticalBlank) then swaps at once; a separate grid here made such a frame take two vblanks (30 fps menu).
void WaitVblank(uint32_t since, uint32_t count) {
    for (int guard = 0; xbox_Nv2aVblankCount() - since < count && guard < 200; guard++) Sleep(1);
}
void Pace() {
    if (app::Get().fps_limit == app::kFpsUnlocked) return;  // xboxrecomp: unlocked (experimental), vsync paces
    static uint32_t last;
    // xboxrecomp: the 30 fps limit (fps_limit) shows each frame for two vblanks at least, as a title asking for
    // D3DPRESENT_INTERVAL_TWO does
    uint32_t n = std::max<uint32_t>(g_interval, app::Get().fps_limit == 30 ? 2 : 1);
    uint32_t now = xbox_Nv2aVblankCount();
    if (now - last < n) WaitVblank(last, n);
    last = xbox_Nv2aVblankCount();
}

int g_on = -1, g_trace = 0;

}  // namespace

bool On() {
    if (g_on < 0) {
        const char* e = std::getenv("RECOMP_HLE_D3D");
        const char* t = std::getenv("RECOMP_D3D_TRACE");
        g_on = !(e && *e == '0');
        g_trace = t && *t && *t != '0';
        std::fprintf(stderr, "[D3D] %s\n", g_on ? "host D3D12 renderer (HLE, SCDA's)" : "XDK Direct3D on the emulated NV2A");
    }
    return g_on != 0;
}

}  // namespace d3d

// the generated XDK bodies this file wraps (tools/recomp/manual_scan.py reads this list)
extern "C" {
extern void sub_0019D860_gen(void);
extern void sub_00196140_gen(void);
extern void sub_00191CA0_gen(void);
extern void sub_00191E40_gen(void);
extern void sub_00192060_gen(void);
extern void sub_00192070_gen(void);
extern void sub_00192170_gen(void);
extern void sub_00192890_gen(void);
extern void sub_00192E30_gen(void);
extern void sub_001930F0_gen(void);
extern void sub_00193400_gen(void);
extern void sub_00195B90_gen(void);
extern void sub_00190720_gen(void);
extern void sub_00190750_gen(void);
extern void sub_00190780_gen(void);
extern void sub_00190A50_gen(void);
extern void sub_00190A90_gen(void);
extern void sub_00190AD0_gen(void);
extern void sub_00190B20_gen(void);
extern void sub_00190B90_gen(void);
extern void sub_00190BD0_gen(void);
extern void sub_00190C10_gen(void);
extern void sub_00190C70_gen(void);
extern void sub_00190CE0_gen(void);
extern void sub_00190D60_gen(void);
extern void sub_00190DE0_gen(void);
extern void sub_00190E40_gen(void);
extern void sub_00190E90_gen(void);
extern void sub_00190EF0_gen(void);
extern void sub_00190F70_gen(void);
extern void sub_00190FC0_gen(void);
extern void sub_001910D0_gen(void);
extern void sub_00191140_gen(void);
extern void sub_00191180_gen(void);
extern void sub_001911C0_gen(void);
extern void sub_001919A0_gen(void);
extern void sub_00191A30_gen(void);
extern void sub_00191AC0_gen(void);
extern void sub_00191B30_gen(void);
extern void sub_00191B60_gen(void);
extern void sub_00191BD0_gen(void);
extern void sub_00191C40_gen(void);
extern void sub_00191C60_gen(void);
extern void sub_00191C80_gen(void);
extern void sub_00191CD0_gen(void);
extern void sub_00191D00_gen(void);
extern void sub_00191D80_gen(void);
extern void sub_00191DD0_gen(void);
extern void sub_001944D0_gen(void);
extern void sub_00194560_gen(void);
extern void sub_001945C0_gen(void);
extern void sub_00194660_gen(void);
extern void sub_001948B0_gen(void);
extern void sub_00192340_gen(void);
extern void sub_00191ED0_gen(void);
extern void sub_00192020_gen(void);
extern void sub_00192090_gen(void);
extern void sub_001920C0_gen(void);
extern void sub_00192110_gen(void);
extern void sub_00192270_gen(void);
extern void sub_001925C0_gen(void);
extern void sub_00192BC0_gen(void);
extern void sub_00192BE0_gen(void);
extern void sub_00192C00_gen(void);
extern void sub_00193250_gen(void);
extern void sub_00193490_gen(void);
extern void sub_00193500_gen(void);
extern void sub_001935E0_gen(void);
extern void sub_001938E0_gen(void);
extern void sub_001952B0_gen(void);
extern void sub_00195300_gen(void);
extern void sub_00195500_gen(void);
extern void sub_00194CE0_gen(void);
extern void sub_0019E390_gen(void);
extern void sub_00194EC0_gen(void);
extern void sub_00195BB0_gen(void);
extern void sub_00195CD0_gen(void);
extern void sub_00195D90_gen(void);
extern void sub_00195F00_gen(void);
extern void sub_00196040_gen(void);
extern void sub_00196050_gen(void);
extern void sub_001960A0_gen(void);
extern void sub_001960C0_gen(void);
extern void sub_00193D30_gen(void);
extern void sub_00193EA0_gen(void);
extern void sub_00193F40_gen(void);
extern void sub_001941A0_gen(void);
extern void sub_001941E0_gen(void);
extern void sub_00194220_gen(void);
extern void sub_00198370_gen(void);
}

using namespace d3d;

// ── Entry point glue ────────────────────────────────────────────────────────

#define A(i) R32(g_esp + 4 + 4 * (i))
#define TRACE(...)                                                      \
    do {                                                                \
        if (g_trace) {                                                  \
            std::fprintf(stderr, "[D3D] " __VA_ARGS__);                 \
            std::fputc('\n', stderr);                                   \
        }                                                               \
    } while (0)

// a host body for the guest function at addr: __stdcall with nargs stack
// arguments (also __fastcall, whose first two are ecx and edx and the rest on
// the stack), or the XDK's own body when the HLE is off
#define HLE(addr, nargs)                                         \
    extern "C" void sub_##addr##_gen(void);                      \
    static uint32_t hle_##addr();                                \
    extern "C" void sub_##addr(void) {                           \
        if (!On()) {                                             \
            sub_##addr##_gen();                                  \
            return;                                              \
        }                                                        \
        uint32_t r = hle_##addr();                               \
        g_eax = r;                                               \
        g_esp += 4 + 4 * (nargs);                                \
    }                                                            \
    static uint32_t hle_##addr()

// device

// Direct3D_CreateDevice(adapter, type, window, flags, params, device**)
HLE(0019D860, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t pp = A(4);
    uint32_t w = R32(pp), h = R32(pp + 4), fmt = R32(pp + 8), count = std::max<uint32_t>(R32(pp + 12), 1);
    uint32_t auto_depth = R32(pp + 0x20), depth_fmt = R32(pp + 0x24), interval = R32(pp + 0x30);
    if (!w) w = 640;
    if (!h) h = 480;
    LOGI("d3d: CreateDevice %ux%u format %X buffers %u depth %u format %X interval %X flags %X", w, h, fmt, count,
         auto_depth, depth_fmt, interval, R32(pp + 0x28));
    g_interval = interval == 2 ? 2 : interval == 3 ? 3 : interval == 4 ? 4 : 1;
    if (interval == 0x80000000u) g_interval = 1;
    std::memset(HostPtr(kDevice), 0, kDeviceBytes);
    W32(kDevicePtr, kDevice);
    // anything still pushing hardware commands writes into a scratch buffer that MakeSpace rewinds
    g_scratch_push = mem::AllocContiguous(0x10000, 0, 0xFFFFFFFF, 0x1000);
    SetDev(kDevPush, g_scratch_push);
    SetDev(kDevThreshold, g_scratch_push + 0xF000);

    uint32_t color_fmt = gpu::LinearColorFormat(fmt);
    g_buffer_count = std::min<uint32_t>(count + 1, 3);
    for (uint32_t i = 0; i < g_buffer_count; i++) g_buffers[i] = NewSurface(w, h, color_fmt, 4);
    g_depth = 0;
    if (auto_depth) {
        uint32_t zf = gpu::LinearDepthFormat(depth_fmt);
        g_depth = NewSurface(w, h, zf, zf == 0x30 || zf == 0x31 ? 2 : 4);  // 0x30 LIN_D16, 0x31 LIN_F16
    }
    if (!g_backend) {
        g_backend = gpu::CreateBackend();
        if (!g_backend->Init(w, h)) LOGE("d3d: renderer init failed");
        g_backend = gpu::CreateThreadedBackend(std::move(g_backend));
        // the window thread stops painting its own picture: the swap chain owns the window now
        xbox_FramebufferWindowSetPresenter([](void*, const XboxPresentInfo*) { return 1; });
        // xboxrecomp: the start menu (app/menu.cpp): the title waits here, before its first movie, while the page is
        // drawn over a black frame; its aspect ratio is the run's (below, and hle_video.c's widescreen check)
        if (app::Get().start_menu) {
            app::OpenStartMenu();
            LOGI("d3d: start menu up, the title waits for Start");
            while (app::StartMenuOpen()) {
                {
                    std::lock_guard<std::recursive_mutex> lock(g_lock);
                    g_backend->Present(SurfaceOf(g_buffers[1]), false);
                }
                Sleep(16);
            }
        }
    }
    g_state = gpu::State{};
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    SetDev(kDevRenderTarget, g_buffers[1]);
    SetDev(kDevDepth, g_depth);
    SetDev(kDevFrameBuffers + 0, g_buffers[1]);
    SetDev(kDevFrameBuffers + 4, g_buffers[0]);
    SetDev(kDevFrameBuffers + 0xC, g_depth);
    float one = 1.0f, zs = g_depth && (R32(g_depth + 12) >> 8 & 0xFF) == 0x30 ? 65535.0f : 16777215.0f;
    std::memcpy(HostPtr(kDevice + kDevSuperX), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevSuperY), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevZScale), &zs, 4);
    g_state.rt = SurfaceOf(g_buffers[1]);
    g_state.zs = SurfaceOf(g_depth);
    UpdateViewportConstants();
    // the XDK's own default state setup, through the entry points below
    CallGuest(kInitDefaultState, {});
    if (A(5)) W32(A(5), kDevice);
    return 0;
}

// synchronisation, the renderer never runs behind

// D3D_BlockOnResource(resource)
HLE(001960C0, 1) {
    uint32_t res = A(0);
    if (res && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        uint32_t type = R32(res) & 0x70000;  // 0x40000 texture, 0x50000 surface
        if (type == 0x40000 || type == 0x50000) {
            uint32_t data = mem::VirtualFromPhysical(R32(res + 4));
            g_backend->Sync(data, 0);
        } else {
            g_backend->WaitResource(res);
        }
    }
    gpu::MarkDirty(res);
    return 0;
}

// Lock2DSurface(resource, face, level, locked, rect, flags): D3DLOCK_NOOVERWRITE skips the block above, yet a
// title may read what the GPU drew
extern "C" void sub_00198370_gen(void);
extern "C" void sub_00198370(void) {
    if (On()) {
        uint32_t res = A(0);
        if ((A(5) & 0x20) && res && g_backend) {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            g_backend->SyncLate(mem::VirtualFromPhysical(R32(res + 4)));
        }
    }
    sub_00198370_gen();
}

// D3D_BlockOnNonSurfaceResource(resource), 5659: what D3DVertexBuffer_Lock2 (and the other non-surface locks) wait
// in unless D3DLOCK_NOOVERWRITE. The XDK waits through D3D_BlockOnTime, which the HLE answers at once, so the title
// rewrote its ring vertex buffer of CPU-skinned characters while the render thread still read it: bodies drawn with
// another character's vertices (missing, flickering, eyes floating alone). Wait for the render thread instead.
HLE(00196140, 1) {
    uint32_t res = A(0);
    if (res && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(res);
    }
    return 0;
}

// D3DResource_BlockUntilNotBusy(resource)
HLE(00194CE0, 1) {
    if (A(0) && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(A(0));
    }
    gpu::MarkDirty(A(0));
    return 0;
}
HLE(00193490, 0) { return 0; }  // D3DDevice_IsBusy
HLE(00195D90, 2) { return 0; }  // D3D_BlockOnTime(time, ...)
// D3D_SetFence(flags)
HLE(00195CD0, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
// D3D_KickOffAndWaitForIdle
HLE(001960A0, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Flush();
    return 0;
}
HLE(00195BB0, 0) { return 0; }  // CDevice_KickOff (__thiscall, no stack arguments)

// D3DDevice_MakeSpace
HLE(00196040, 0) {
    LOG_FIRST(16, "d3d: MakeSpace from %08X, some caller still pushes hardware commands", R32(g_esp));
    SetDev(kDevPush, g_scratch_push);
    return g_scratch_push;
}
// D3D_MakeRequestedSpace(minimum, requested)
HLE(00195F00, 2) {
    LOG_FIRST(16, "d3d: MakeRequestedSpace from %08X", R32(g_esp));
    SetDev(kDevPush, g_scratch_push);
    return g_scratch_push;
}
// D3DDevice_BeginStateBig(count): room for a long run of pushes
HLE(00196050, 1) {
    LOG_FIRST(16, "d3d: BeginStateBig from %08X", R32(g_esp));
    SetDev(kDevPush, g_scratch_push);
    return g_scratch_push;
}

// D3DDevice_BlockUntilVerticalBlank
HLE(00192090, 0) {
    if (app::Get().fps_limit == app::kFpsUnlocked) return 0;  // xboxrecomp: unlocked (experimental)
    WaitVblank(xbox_Nv2aVblankCount(), 1);  // the next vblank
    return 0;
}

// fences (SCDA): the renderer's own
// D3DDevice_InsertFence()
HLE(00192170, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
// D3DDevice_IsFencePending(fence)
HLE(00195B90, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend && g_backend->FencePending(A(0)) ? 1 : 0;
}
// D3DDevice_BlockOnFence(fence)
HLE(00192060, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->WaitFence(A(0));
    return 0;
}

// D3DDevice_SetGammaRamp(flags, ramp): 3 x 256 bytes, red, green, blue (SCDA)
HLE(00191E40, 2) {
    uint8_t ramp[768];
    std::memcpy(ramp, HostPtr(A(1)), 768);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->SetGamma(ramp);
    return 0;
}

// D3DDevice_SetVerticalBlankCallback(callback). The XDK calls it from its vblank interrupt's DPC with a
// D3DVBLANKDATA { VBlank, Swap, Flags }; the HLE device has no interrupt, so the runtime's vblank tick does, on its
// timer thread (kernel_bridge.c xbox_SetVblankHook). This title's callback runs its ten registered vblank handlers.
namespace {
uint32_t g_vblank_data = 0;
void VblankHook() {
    uint32_t cb = Dev(kDevVBlankCallback);
    if (!cb || !g_vblank_data) return;
    static bool first = true;
    if (first) {
        first = false;
        LOGI("d3d: vblank callback %08X runs from the runtime's vblank", cb);
    }
    W32(g_vblank_data + 0, xbox_Nv2aVblankCount());
    W32(g_vblank_data + 4, Dev(kDevSwapCount));
    W32(g_vblank_data + 8, 0);
    CallGuest(cb, {g_vblank_data});
}
}  // namespace
HLE(00192070, 1) {
    if (!g_vblank_data) {
        g_vblank_data = mem::AllocSystem(16, 16);
        std::memset(HostPtr(g_vblank_data), 0, 16);
        xbox_SetVblankHook(VblankHook);
    }
    SetDev(kDevVBlankCallback, A(0));
    TRACE("SetVerticalBlankCallback %08X", A(0));
    return 0;
}
HLE(001920C0, 1) { return 0; }  // D3DDevice_SetFlickerFilter
HLE(00192110, 1) { return 0; }  // D3DDevice_SetSoftDisplayFilter
HLE(00193500, 2) { return 0; }  // D3D_SetTileNoWait: the console's memory controller's business

// D3DDevice_GetDisplayMode(mode): Width, Height, RefreshRate, Flags, Format of the device's buffers
HLE(00192270, 1) {
    uint32_t m = A(0);
    uint32_t w = 640, h = 480;
    if (g_buffers[1]) gpu::SurfaceSize(SurfaceOf(g_buffers[1]), w, h);
    W32(m + 0, w);
    W32(m + 4, h);
    W32(m + 8, 60);
    W32(m + 12, 0);
    W32(m + 16, 0x1E);  // D3DFMT_LIN_X8R8G8B8
    return 0;
}

// targets and presentation

// D3DDevice_SetRenderTarget(target, depth)
HLE(00192340, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t rt = A(0) ? A(0) : Dev(kDevRenderTarget), zs = A(1);
    AddRef(rt);
    AddRef(zs);
    uint32_t old_rt = Dev(kDevRenderTarget), old_zs = Dev(kDevDepth);
    SetDev(kDevRenderTarget, rt);
    SetDev(kDevDepth, zs);
    Release(old_rt);
    Release(old_zs);
    g_state.rt = SurfaceOf(rt);
    g_state.zs = SurfaceOf(zs);
    // SetRenderTarget resets the viewport to the whole target
    uint32_t w, h;
    gpu::SurfaceSize(g_state.rt, w, h);
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    float zscale = ZScale(g_state.zs);
    std::memcpy(HostPtr(kDevice + kDevZScale), &zscale, 4);
    UpdateViewportConstants();
    return 0;
}

// D3DDevice_GetBackBuffer2(index): -1 the front buffer
HLE(001925C0, 1) {
    int32_t i = int32_t(A(0));
    uint32_t s = i < 0 ? g_buffers[0] : g_buffers[std::min<uint32_t>(uint32_t(i) + 1, g_buffer_count - 1)];
    W32(s, R32(s) + 1);
    return s;
}
// D3DDevice_GetRenderTarget2
HLE(00192BC0, 0) {
    uint32_t s = Dev(kDevRenderTarget);
    if (s) W32(s, R32(s) + 1);
    return s;
}
// D3DDevice_GetDepthStencilSurface2
HLE(00192BE0, 0) {
    uint32_t s = Dev(kDevDepth);
    if (s) W32(s, R32(s) + 1);
    return s;
}

// D3DDevice_Swap(flags)
static std::atomic<uint64_t> g_last_swap_ms{0};
// when the last Swap happened (QueryPerformanceCounter), for main.c's [STALL] watcher
extern "C" volatile int64_t hpps_last_swap_qpc = 0;
static std::atomic<int> g_hud_trace{0};  // xboxrecomp debug: HudTrace, F12
static uint32_t g_hud_n = 0;

// xboxrecomp: the debug trainer's report asks for an F12 capture of the next frame (hle_trainer.c)
extern "C" void hpps_RequestCapture(void) { gpu::g_capture = true; }

// xboxrecomp: the run is 16:9 (hle_video.c widens the 4:3 cameras)
extern "C" int hpps_Widescreen(void) { return app::BootAspect() == app::AspectMode::Wide; }

// A movie frame went up (hle_video.c's UpdateOverlay). The NV2A scans the overlay out by itself, so a title playing
// an XMV need not Swap; when it has not swapped lately, present the last frame for the overlay to be drawn over.
extern "C" void hle_d3d8_overlay_present(void) {
    if (!On() || GetTickCount64() - g_last_swap_ms.load() < 50) return;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend && g_buffers[1]) g_backend->Present(SurfaceOf(g_buffers[1]), false);
}

HLE(0019E390, 1) {
    g_last_swap_ms.store(GetTickCount64());
    {   // xboxrecomp debug: F12 down starts a [HUD] trace of the next frame, this Swap ends one
        static bool was_down;
        // Insert as well: some desktops and terminals keep F12 for themselves
        bool down = ((GetAsyncKeyState(VK_F12) | GetAsyncKeyState(VK_INSERT)) & 0x8000) != 0;
        if (g_hud_trace.exchange(0)) fprintf(stderr, "[HUD] end\n");
        // RECOMP_CAPTURE_AFTER=<seconds>[:<every>:<count>]: a capture that long after the first Swap, then <count>
        // more <every> seconds apart (unattended screenshots)
        static double cap_after, cap_every;
        static int cap_left = [] {
            const char* e = std::getenv("RECOMP_CAPTURE_AFTER");
            int n = 0;
            if (e && std::sscanf(e, "%lf:%lf:%d", &cap_after, &cap_every, &n) < 3) n = 0;
            return e && cap_after > 0 ? n + 1 : 0;
        }();
        static uint64_t first_swap_ms;
        if (!first_swap_ms) first_swap_ms = GetTickCount64();
        if (cap_left > 0 && GetTickCount64() - first_swap_ms >= uint64_t(cap_after * 1000)) {
            cap_left--;
            cap_after += cap_every;
            gpu::g_capture = true;
            fprintf(stderr, "[D3D] timed capture\n");
        }
        if (down && !was_down) {
            gpu::g_capture = true;  // this GetAsyncKeyState takes F12's pressed bit the renderer's own check reads
            g_hud_n = 0;
            g_hud_trace.store(1);
            fprintf(stderr, "[HUD] begin\n");
        }
        was_down = down;
    }
    {   // frame pacing, every 5 s: frames, the slowest one and how many took over 25 ms (a dropped 60 Hz frame)
        static LARGE_INTEGER freq, last, t0;
        static uint32_t frames, slow;
        static double worst;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!freq.QuadPart) {
            QueryPerformanceFrequency(&freq);
            t0 = last = now;
        }
        double dt = double(now.QuadPart - last.QuadPart) * 1000.0 / double(freq.QuadPart);
        last = now;
        hpps_last_swap_qpc = now.QuadPart;
        frames++;
        if (dt > worst) worst = dt;
        if (dt > 25.0) slow++;
        double span = double(now.QuadPart - t0.QuadPart) / double(freq.QuadPart);
        if (span >= 5.0) {
            fprintf(stderr, "[FRAME] %.1f fps, slowest %.1f ms, %u frames over 25 ms\n", frames / span, worst, slow);
            frames = slow = 0;
            worst = 0;
            t0 = now;
        }
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        if (g_backend) g_backend->Present(SurfaceOf(g_buffers[1]), false);
        // like the XDK, the surfaces stay put and their memory rotates: titles keep back buffer pointers
        uint32_t last = g_buffers[g_buffer_count - 1], data = R32(last + 4), lk = R32(last + 8);
        for (uint32_t i = g_buffer_count - 1; i > 0; i--) {
            W32(g_buffers[i] + 4, R32(g_buffers[i - 1] + 4));
            W32(g_buffers[i] + 8, R32(g_buffers[i - 1] + 8));
        }
        W32(g_buffers[0] + 4, data);
        W32(g_buffers[0] + 8, lk);
        g_state.rt = SurfaceOf(Dev(kDevRenderTarget));
    }
    xbox_FramebufferWindowFlipped();
    xbox_FramebufferWindowFrameStats(0);
    Pace();
    uint32_t n = Dev(kDevSwapCount) + 1;
    SetDev(kDevSwapCount, n);
    return n;
}

// D3DDevice_Clear(count, rects, flags, color, z, stencil)
HLE(00194EC0, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint8_t* rects = A(0) && A(1) ? static_cast<const uint8_t*>(HostPtr(A(1))) : nullptr;
    if (g_backend) g_backend->Clear(g_state, A(0), rects, A(2), A(3), ArgF(A(4)), A(5));
    return 0;
}

// D3DDevice_CopyRects(source, rects, count, destination, points) (SCDA)
HLE(00192890, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Copy(SurfaceOf(A(0)), SurfaceOf(A(3)), A(2), A(1), A(4));
    return 0;
}

// D3DDevice_SetViewport(viewport)
HLE(00192C00, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t v = A(0);
    g_state.viewport = {R32(v), R32(v + 4), R32(v + 8), R32(v + 12), GF(v + 16), GF(v + 20)};
    g_state.scissor_count = 0;
    // the XDK's copy, which GetViewport and GetViewportOffsetAndScale read
    std::memcpy(HostPtr(kDevice + kDevViewport), HostPtr(v), 24);
    UpdateViewportConstants();
    return 0;
}

// D3DDevice_SetScissors(count, exclusive, rects)
HLE(001935E0, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.scissor_count = std::min<uint32_t>(A(0), 8);
    for (uint32_t i = 0; i < g_state.scissor_count * 4; i++) g_state.scissors[i] = R32(A(2) + i * 4);
    return 0;
}

// D3DDevice_SetScreenSpaceOffset(x, y)
HLE(001938E0, 2) {
    g_state.ss_offset_x = ArgF(A(0));
    g_state.ss_offset_y = ArgF(A(1));
    W32(kDevice + kDevScreenOffset, A(0));
    W32(kDevice + kDevScreenOffset + 4, A(1));
    UpdateViewportConstants();
    return 0;
}

// render states

// D3DDevice_SetRenderState_Simple(method, value), __fastcall
HLE(00190750, 0) {
    uint32_t method = g_ecx & 0x1FFC, value = g_edx;
    g_state.simple[method >> 2] = value;
    return 0;
}

// D3DDevice_SetRenderStateNotInline(state, value)
HLE(00190780, 2) {
    uint32_t rs = A(0), v = A(1);
    if (rs < gpu::kRsDeferredFirst) {
        uint32_t method = R32(kSimpleEncode + rs * 4) & 0x1FFC;
        g_state.simple[method >> 2] = v;
        W32(kRenderState + rs * 4, v);
    } else if (rs < gpu::kRsComplexFirst) {
        W32(kRenderState + rs * 4, v);
    } else if (rs < gpu::kRsMax) {
        SetComplex(rs, v);
    }
    return 0;
}

// D3DDevice_SetRenderState_ZBias(value). Not a plain complex state: the XDK turns it into the polygon offset simple
// states, which are what the renderer keys depth bias on (decals such as windows z-fight without it). As 5028 does:
// f = -(float)(DWORD)value; PolygonOffsetZSlopeScale (0x4D) = f / 4, PolygonOffsetZOffset (0x4E) = f, and the
// point/wireframe/solid offset enables (0x4F-0x51) = value != 0.
HLE(00190D60, 1) {
    uint32_t v = A(0);
    float f = -float(v);  // the XDK reads the DWORD unsigned
    float slope = f * 0.25f;
    uint32_t bits_slope, bits_f, on = v != 0;
    std::memcpy(&bits_slope, &slope, 4);
    std::memcpy(&bits_f, &f, 4);
    const uint32_t states[5][2] = {{0x4D, bits_slope}, {0x4E, bits_f}, {0x4F, on}, {0x50, on}, {0x51, on}};
    for (const auto& sv : states) {
        uint32_t method = R32(kSimpleEncode + sv[0] * 4) & 0x1FFC;
        g_state.simple[method >> 2] = sv[1];
        W32(kRenderState + sv[0] * 4, sv[1]);
    }
    SetComplex(gpu::kRsZBias, v);
    return 0;
}

#define COMPLEX_STATE(addr, rs) \
    HLE(addr, 1) {              \
        SetComplex(rs, A(0));   \
        return 0;               \
    }
// D3DDevice_SetRenderState_<name>(value), at this XBE's addresses
COMPLEX_STATE(00190720, gpu::kRsPsTextureModes)
COMPLEX_STATE(00190F70, gpu::kRsVertexBlend)
COMPLEX_STATE(00190AD0, gpu::kRsFogColor)
COMPLEX_STATE(00190E40, gpu::kRsFillMode)
COMPLEX_STATE(00190E90, gpu::kRsBackFillMode)
COMPLEX_STATE(00190EF0, gpu::kRsTwoSidedLighting)
COMPLEX_STATE(00190BD0, gpu::kRsNormalizeNormals)
COMPLEX_STATE(001919A0, gpu::kRsZEnable)
COMPLEX_STATE(00191A30, gpu::kRsStencilEnable)
COMPLEX_STATE(00191AC0, gpu::kRsStencilFail)
COMPLEX_STATE(00190B90, gpu::kRsFrontFace)
COMPLEX_STATE(00190B20, gpu::kRsCullMode)
COMPLEX_STATE(00190C10, gpu::kRsTextureFactor)
COMPLEX_STATE(00190DE0, gpu::kRsLogicOp)
COMPLEX_STATE(00190A50, gpu::kRsEdgeAntiAlias)
COMPLEX_STATE(00191D00, gpu::kRsMultiSampleAntiAlias)
COMPLEX_STATE(00191D80, gpu::kRsMultiSampleMask)
COMPLEX_STATE(00191CD0, gpu::kRsMultiSampleRenderTargetMode)
COMPLEX_STATE(00191CA0, gpu::kRsMultiSampleMode)
COMPLEX_STATE(00190A90, gpu::kRsShadowFunc)
COMPLEX_STATE(00190C70, gpu::kRsLineWidth)
COMPLEX_STATE(00191DD0, gpu::kRsSampleAlpha)
COMPLEX_STATE(00190CE0, gpu::kRsDxt1NoiseEnable)
COMPLEX_STATE(00191B30, gpu::kRsYuvEnable)
COMPLEX_STATE(00191B60, gpu::kRsOcclusionCullEnable)
COMPLEX_STATE(00191BD0, gpu::kRsStencilCullEnable)
COMPLEX_STATE(00191C40, gpu::kRsRopZCmpAlwaysRead)
COMPLEX_STATE(00191C60, gpu::kRsRopZRead)
COMPLEX_STATE(00191C80, gpu::kRsDoNotCullUncompressed)

// D3DDevice_SetTextureState_TexCoordIndex(stage, value)
HLE(00190FC0, 2) {
    g_state.tss[A(0) & 3][gpu::kTssTexCoordIndex] = A(1);
    W32(kTextureState + ((A(0) & 3) * 32 + gpu::kTssTexCoordIndex) * 4, A(1));
    return 0;
}
// D3DDevice_SetTextureState_BumpEnv(stage, type, value)
HLE(001910D0, 3) {
    if (A(1) >= gpu::kTssBumpEnvMat00 && A(1) <= gpu::kTssBumpEnvLOffset) g_state.tss[A(0) & 3][A(1)] = A(2);
    return 0;
}
// D3DDevice_SetTextureState_BorderColor(stage, value)
HLE(00191140, 2) {
    g_state.tss[A(0) & 3][gpu::kTssBorderColor] = A(1);
    return 0;
}
// D3DDevice_SetTextureState_ColorKeyColor(stage, value)
HLE(00191180, 2) {
    g_state.tss[A(0) & 3][gpu::kTssColorKeyColor] = A(1);
    return 0;
}
// D3DDevice_SetTextureStageStateNotInline(stage, type, value)
HLE(001911C0, 3) {
    uint32_t stage = A(0) & 3, type = A(1), v = A(2);
    if (type < gpu::kTssCount) {
        g_state.tss[stage][type] = v;
        W32(kTextureState + (stage * 32 + type) * 4, v);
    }
    return 0;
}

// D3DDevice_SetTransform(type, matrix): the XDK's types are view 0, projection 1, texture 2-5, world 6-9; the
// renderer keeps Direct3D 8's numbers (view 2, projection 3, texture 16, world 256). The device copy is what
// GetTransform reads.
HLE(00191ED0, 2) {
    uint32_t t = A(0);
    if (t >= 10) return 0;
    std::memcpy(HostPtr(kDevice + kDevTransforms + t * 64), HostPtr(A(1)), 64);
    uint32_t pc = t < 2 ? t + 2 : t < 6 ? t + 14 : t + 250;
    std::memcpy(g_state.transforms[pc].data(), HostPtr(A(1)), 64);
    return 0;
}

// D3DDevice_SetMaterial(material)
HLE(00192020, 1) {
    std::memcpy(g_state.material.data(), HostPtr(A(0)), 17 * 4);
    return 0;
}

// the XDK keeps its own light lists, the renderer gets a copy (SCDA)
// D3DDevice_SetLight(index, light)
extern "C" void sub_00192E30_gen(void);
extern "C" void sub_00192E30(void) {
    uint32_t i = A(0), l = A(1);
    if (On() && i < 8 && l) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        std::memcpy(g_state.lights[i].data(), HostPtr(l), 26 * 4);
    }
    sub_00192E30_gen();
}
// D3DDevice_LightEnable(index, enable)
extern "C" void sub_001930F0_gen(void);
extern "C" void sub_001930F0(void) {
    uint32_t i = A(0), on = A(1);
    if (On() && i < 8) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_state.light_enable = on ? g_state.light_enable | 1u << i : g_state.light_enable & ~(1u << i);
    }
    sub_001930F0_gen();
}

// resources in use

// D3DDevice_SetTexture(stage, texture)
HLE(00193250, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = A(0) & 3, tex = A(1);
    uint32_t old = Dev(kDevTextures + stage * 4);
    AddRef(tex);
    SetDev(kDevTextures + stage * 4, tex);
    if (old) W32(old + 8, Dev(kDevFence));
    Release(old);
    g_state.textures[stage] = tex;
    W32(kDirtyFlags, R32(kDirtyFlags) | 0x4800);
    return 0;
}

// D3DDevice_SetPalette(stage, palette): the device's slot at +0xF98, references as the XDK keeps them
HLE(00193400, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = A(0) & 3, pal = A(1);
    uint32_t old = Dev(kDevPalettes + stage * 4);
    AddRef(pal);
    SetDev(kDevPalettes + stage * 4, pal);
    if (old) W32(old + 8, Dev(kDevFence));
    Release(old);
    g_state.palettes[stage] = pal;
    return 0;
}

// D3DDevice_SetStreamSource(stream, buffer, stride)
HLE(001944D0, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t n = A(0) & 15, vb = A(1), stride = A(2);
    uint32_t old = R32(kStreams + n * 12 + 8);
    AddRef(vb);
    W32(kStreams + n * 12, stride);
    W32(kStreams + n * 12 + 8, vb);
    Release(old);
    g_state.streams[n] = {vb, stride};
    return 0;
}

// shaders

// D3DDevice_SetVertexShader(handle)
HLE(001948B0, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t h = A(0);
    g_state.vs_handle = h;
    g_state.vs = nullptr;
    SetDev(kDevVertexShader, h);
    if (h & 1) {
        auto it = g_vs.find(h);
        if (it != g_vs.end()) {
            g_state.vs = it->second;
            // constants declared with the shader load when it is selected
            for (auto& [reg, v] : it->second->constants)
                if (reg < 192) g_state.vsc[reg] = v;
        } else {
            LOG_FIRST(8, "d3d: SetVertexShader with unknown handle %08X", h);
        }
    }
    return 0;
}

namespace {
// constants by hardware register (D3D register + 96), also into the XDK's mirror that GetVertexShaderConstant reads
void VsConstants(uint32_t reg, uint32_t src, uint32_t count) {
    // xboxrecomp debug: RECOMP_VSC_TRACE=<hw register, e.g. 109> logs each change of that constant and its caller
    static int trace = [] { const char* e = std::getenv("RECOMP_VSC_TRACE"); return e && *e ? std::atoi(e) : -1; }();
    if (trace >= 0 && uint32_t(trace) >= reg && uint32_t(trace) < reg + count) {
        static float last[4];
        const float* v = reinterpret_cast<const float*>(HostPtr(src + (trace - reg) * 16));
        if (std::memcmp(v, last, 8)) {  // x and y: the fog constant's z and w are stack garbage
            std::memcpy(last, v, 16);
            std::fprintf(stderr, "[VSC] c%d = %g %g %g %g from %08X (stack %08X %08X %08X %08X)\n", trace, v[0], v[1], v[2],
                         v[3], R32(g_esp), R32(g_esp + 4), R32(g_esp + 8), R32(g_esp + 12), R32(g_esp + 16));
        }
    }
    for (uint32_t i = 0; i < count && reg + i < 192; i++) {
        std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    }
}
}  // namespace

// D3DDevice_LoadVertexShader(handle, address)
HLE(00194560, 2) {
    LOG_FIRST(4, "d3d: LoadVertexShader %08X at %u is not supported", A(0), A(1));
    return 0;
}
// D3DDevice_SelectVertexShader(handle, address)
HLE(001945C0, 2) {
    LOG_FIRST(4, "d3d: SelectVertexShader %08X at %u is not supported", A(0), A(1));
    return 0;
}
HLE(00194660, 1) { return 0; }  // D3DDevice_SetShaderConstantMode

// D3DDevice_CreatePixelShader(definition, handle*)
HLE(001952B0, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto ps = std::make_shared<gpu::PixelShader>();
    std::memcpy(ps->def, HostPtr(A(0)), sizeof(ps->def));
    uint32_t rec = mem::AllocSystem(16, 16);
    ps->handle = rec;
    g_ps[rec] = ps;
    W32(A(1), rec);
    return 0;
}

// D3DDevice_SetPixelShader(handle)
HLE(00195300, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.ps = nullptr;
    if (!A(0)) return 0;
    auto it = g_ps.find(A(0));
    if (it == g_ps.end()) {
        LOG_FIRST(8, "d3d: SetPixelShader with unknown handle %08X", A(0));
        return 0;
    }
    g_state.ps = it->second;
    // the definition loads the combiner state like the XDK pushes it
    const uint32_t* d = it->second->def;
    auto put = [&](uint32_t method, uint32_t v) { g_state.simple[method >> 2] = v; };
    for (int i = 0; i < 8; i++) {
        put(gpu::kMPsAlphaInputs + i * 4, d[0 + i]);
        put(gpu::kMPsConstant0 + i * 4, d[10 + i]);
        put(gpu::kMPsConstant1 + i * 4, d[18 + i]);
        put(gpu::kMPsAlphaOutputs + i * 4, d[26 + i]);
        put(gpu::kMPsRgbInputs + i * 4, d[34 + i]);
        put(gpu::kMPsRgbOutputs + i * 4, d[45 + i]);
    }
    put(gpu::kMPsFinalAbcd, d[8]);
    put(gpu::kMPsFinalEfgh, d[9]);
    put(gpu::kMPsCompareMode, d[42]);
    put(gpu::kMPsFinalConstant0, d[43]);
    put(gpu::kMPsFinalConstant1, d[44]);
    put(gpu::kMPsCombinerCount, d[53]);
    put(gpu::kMPsDotMapping, d[55]);
    put(gpu::kMPsInputTexture, d[56]);
    SetComplex(gpu::kRsPsTextureModes, d[54]);
    return 0;
}

// D3DDevice_SetPixelShaderConstant(register, data, count)
HLE(00195500, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t reg = A(0), src = A(1), n = A(2);
    for (uint32_t i = 0; i < n && reg + i < 16; i++) std::memcpy(g_state.psc[reg + i].data(), HostPtr(src + i * 16), 16);
    gpu::ApplyPixelConstants(g_state, reg, n);
    return 0;
}

// drawing

// xboxrecomp debug: pressing F12 also logs the next frame's small undepth-tested draws from the title's side ([HUD]
// lines): their screen rectangle, texture, and the title's return addresses found on the stack, to find what draws
// each HUD element
namespace {
void HudTrace(uint32_t prim, uint32_t count, uint32_t verts, uint32_t stride) {
    if (!g_hud_trace.load() || count > 32 || g_state.rs[gpu::kRsZEnable]) return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    if (verts && stride >= 8) {
        for (uint32_t v = 0; v < count; v++) {
            float xy[2];
            std::memcpy(xy, HostPtr(verts + v * stride), 8);
            x0 = std::min(x0, xy[0]), y0 = std::min(y0, xy[1]), x1 = std::max(x1, xy[0]), y1 = std::max(y1, xy[1]);
        }
    }
    fprintf(stderr, "[HUD] %04u prim %u count %u vs %08X tex %08X rect %.1f %.1f %.1f %.1f rets", g_hud_n++, prim, count,
            g_state.vs_handle, g_state.textures[0], x0, y0, x1, y1);
    int found = 0;
    for (uint32_t i = 0; i < 256 && found < 10; i++) {
        uint32_t v = R32(g_esp + 4 * i);
        if (v < 0x11005 || v >= 0x190714) continue;  // the title's own code (.text)
        if (R8(v - 5) == 0xE8 || R8(v - 2) == 0xFF || R8(v - 3) == 0xFF || R8(v - 6) == 0xFF) {
            fprintf(stderr, " %08X", v);
            found++;
        }
    }
    fprintf(stderr, "\n");
}

// the draw's vertices for HudTrace: stream 0 from start
uint32_t StreamVertices(uint32_t start) {
    const auto& st = g_state.streams[0];
    if (!st.vb || !st.stride) return 0;
    return mem::VirtualFromPhysical(R32(st.vb + 4)) + start * st.stride;
}
}  // namespace

// D3DDevice_DrawVertices(primitive, start, count)
HLE(00193EA0, 3) {
    HudTrace(A(0), A(2), StreamVertices(A(1)), g_state.streams[0].stride);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    SyncDeferred();
    if (g_backend) g_backend->Draw(g_state, A(0), A(1), A(2), nullptr);
    return 0;
}

// D3DDevice_DrawIndexedVertices(primitive, count, indices)
HLE(00193F40, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    SyncDeferred();
    const uint8_t* indices = A(2) ? static_cast<const uint8_t*>(HostPtr(A(2))) : nullptr;
    if (g_backend) g_backend->Draw(g_state, A(0), 0, A(1), indices);
    return 0;
}

// DrawVerticesUP(prim, count, vertices, stride): the vertices are copied to a ring the render thread reads, stream 0
// points there through a vertex buffer header for the draw. A half of the ring is written again once the draws
// that read it are done.
namespace {
constexpr uint32_t kUpRing = 4u << 20, kUpHalf = kUpRing / 2;
uint32_t g_up_ring = 0, g_up_pos = 0, g_up_header = 0, g_up_fence[2] = {};

uint32_t UpVertices(uint32_t src, uint32_t bytes) {
    if (!g_up_ring) {
        g_up_ring = mem::AllocContiguous(kUpRing, 0, 0xFFFFFFFF, 0x1000);
        g_up_header = mem::AllocSystem(20);
        std::memset(HostPtr(g_up_header), 0, 20);
        W32(g_up_header, 1);
    }
    bytes = std::min(bytes, kUpHalf);
    uint32_t room = (bytes + 63) & ~63u;
    uint32_t pos = g_up_pos + room > kUpRing ? 0 : g_up_pos;
    uint32_t from = g_up_pos ? (g_up_pos - 1) / kUpHalf : 1, to = (pos + room - 1) / kUpHalf;
    if (to != from) {
        g_up_fence[from] = g_backend->Fence();
        g_backend->WaitFence(g_up_fence[to]);
    }
    std::memcpy(HostPtr(g_up_ring + pos), HostPtr(src), bytes);
    g_up_pos = pos + room;
    W32(g_up_header + 4, mem::PhysicalFromVirtual(g_up_ring + pos));
    return g_up_header;
}
}  // namespace

// D3DDevice_DrawVerticesUP(primitive, count, vertices, stride)
HLE(00193D30, 4) {
    HudTrace(A(0), A(1), A(2), A(3));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_backend || !A(1) || !A(3)) return 0;
    SyncDeferred();
    auto stream = g_state.streams[0];
    g_state.streams[0] = {UpVertices(A(2), A(1) * A(3)), A(3)};
    g_backend->Draw(g_state, A(0), 0, A(1), nullptr);
    g_state.streams[0] = stream;
    return 0;
}

// immediate mode: not drawn, said once
HLE(001941E0, 1) {  // D3DDevice_Begin(primitive)
    LOG_FIRST(4, "d3d: Begin/End immediate mode is not supported (from %08X)", R32(g_esp));
    return 0;
}
HLE(00194220, 0) { return 0; }  // D3DDevice_End
HLE(001941A0, 3) { return 0; }  // D3DDevice_SetVertexData2f(register, a, b)
