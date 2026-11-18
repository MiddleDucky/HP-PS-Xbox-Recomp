// xboxrecomp: the controls settings (app.h) applied to every pad read, and keyboard and mouse as controller 1.
//
// hle_input.c's XInputGetState calls app_InputPad on the pad state the PC's controller gave: the game's buttons are
// remapped from the pad's (pad_map), each stick gets its deadzone and response, the camera stick its inversion, and
// on port 0 the keyboard and mouse are merged on top when keyboard_mouse is on: bound keys press their pad input,
// WASD tilt the left stick fully, the mouse's movement tilts the right stick by its speed. app_InputRumble scales the
// game's rumble. The keys and the mouse come from the framebuffer window's messages (menu.cpp's hook, InputMessage):
// a key table, and raw mouse input for movement, which keeps counting at the screen's edge. While the game window has
// the focus and no panel is open the pointer is held inside it, so a click cannot land on another window.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app/app.h"
#include "input/xinput_xbox.h"

namespace app {

namespace {

struct KeyName_ {
    uint32_t vk;
    const char* name;
};
const KeyName_ kKeyNames[] = {
    {0, "none"},          {0x01, "Mouse1"},      {0x02, "Mouse2"},     {0x04, "Mouse3"},     {0x05, "Mouse4"},
    {0x06, "Mouse5"},     {0x08, "Backspace"},   {0x09, "Tab"},        {0x0D, "Enter"},      {0x10, "Shift"},
    {0x11, "Ctrl"},       {0x12, "Alt"},         {0x13, "Pause"},      {0x14, "CapsLock"},   {0x1B, "Escape"},
    {0x20, "Space"},      {0x21, "PageUp"},      {0x22, "PageDown"},   {0x23, "End"},        {0x24, "Home"},
    {0x25, "Left"},       {0x26, "Up"},          {0x27, "Right"},      {0x28, "Down"},       {0x2D, "Insert"},
    {0x2E, "Delete"},     {0x60, "Num0"},        {0x61, "Num1"},       {0x62, "Num2"},       {0x63, "Num3"},
    {0x64, "Num4"},       {0x65, "Num5"},        {0x66, "Num6"},       {0x67, "Num7"},       {0x68, "Num8"},
    {0x69, "Num9"},       {0x6A, "NumMultiply"}, {0x6B, "NumPlus"},    {0x6D, "NumMinus"},   {0x6E, "NumDecimal"},
    {0x6F, "NumDivide"},  {0xBA, "Semicolon"},   {0xBB, "Equals"},     {0xBC, "Comma"},      {0xBD, "Minus"},
    {0xBE, "Period"},     {0xBF, "Slash"},       {0xC0, "Backquote"},  {0xDB, "LeftBracket"}, {0xDC, "Backslash"},
    {0xDD, "RightBracket"}, {0xDE, "Quote"},
};

std::atomic<uint8_t> g_down[256];          // keys and mouse buttons held, by VK
std::atomic<long> g_mouse_dx{0}, g_mouse_dy{0};  // raw movement not yet read
std::atomic<bool> g_focus{false};
std::atomic<int> g_capture{-1};            // the menu waits for a key for this binding, -1 none
std::atomic<uint32_t> g_captured{~0u};     // the key it got
bool g_raw = false;                        // raw mouse input registered
bool g_clipped = false;

bool Held(uint32_t vk) { return vk && vk < 256 && g_down[vk].load(std::memory_order_relaxed); }

// the mouse button a message names, as VK
uint32_t MouseVk(unsigned msg, uintptr_t wp) {
    switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: return VK_LBUTTON;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: return VK_RBUTTON;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: return VK_MBUTTON;
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: return HIWORD(wp) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
        default: return 0;
    }
}

int16_t Clamp16(float v) { return int16_t(std::lrintf(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); }

// a stick past its deadzone, rescaled to start from 0 there, times its response, direction kept
void Shape(int16_t* x, int16_t* y, uint32_t deadzone, uint32_t response) {
    float fx = *x / 32767.0f, fy = *y / 32767.0f, m = std::sqrt(fx * fx + fy * fy);
    float dz = std::min(deadzone, 90u) / 100.0f;
    if (m <= dz || m <= 0.0f) {
        *x = *y = 0;
        return;
    }
    float k = (m - dz) / (1.0f - dz) * (response / 100.0f) / m;
    float nx = fx * k, ny = fy * k, n = std::sqrt(nx * nx + ny * ny);
    if (n > 1.0f) nx /= n, ny /= n;
    *x = Clamp16(nx);
    *y = Clamp16(ny);
}

// the pad's inputs as 0..255 in kPadInputNames' order
void Read(const XBOX_GAMEPAD& g, uint8_t v[kPadInputs]) {
    for (int i = 0; i < 8; i++) v[i] = g.bAnalogButtons[i];
    for (int i = 0; i < 8; i++) v[8 + i] = (g.wButtons >> i & 1) ? 255 : 0;
}

void Write(XBOX_GAMEPAD& g, const uint8_t v[kPadInputs]) {
    for (int i = 0; i < 8; i++) g.bAnalogButtons[i] = v[i];
    g.wButtons &= ~0xFFu;
    for (int i = 0; i < 8; i++)
        if (v[8 + i] > XBOX_ANALOG_BUTTON_THRESHOLD) g.wButtons |= uint16_t(1u << i);
}

// the mouse's movement since the last read as a right stick tilt; read at most every 8 ms, so a title polling twice a
// frame sees the same tilt both times rather than the movement and then nothing
// the mouse's tilt of the last read before any cap: past 1 the camera turns faster than the stick's full tilt
// (hle_camera.c reads it; 2026-11-16: 100 felt slow, the stick's full tilt was the most the mouse could ask for)
float g_mouse_tilt_x, g_mouse_tilt_y;
float g_pad_boost = 1.0f;  // the controller camera speed while the right stick turns the camera, else 1

void MouseStick(const Settings& s, float* rx, float* ry) {
    static LARGE_INTEGER last{};
    static float sx, sy;
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    double dt = last.QuadPart ? double(now.QuadPart - last.QuadPart) / double(f.QuadPart) : 0.016;
    if (dt >= 0.008) {
        long dx = g_mouse_dx.exchange(0), dy = g_mouse_dy.exchange(0);
        // counts per second to a tilt: 2000 counts/s is full tilt at sensitivity 50
        float k = float(s.mouse_sensitivity) / 50.0f / 2000.0f;
        float nx = float(dx / std::max(dt, 0.004)) * k, ny = float(-dy / std::max(dt, 0.004)) * k;
        // a little smoothing: the poll and the mouse's reports do not line up
        sx = sx * 0.35f + nx * 0.65f;
        sy = sy * 0.35f + ny * 0.65f;
        if (std::fabs(sx) < 0.01f) sx = 0;
        if (std::fabs(sy) < 0.01f) sy = 0;
        last = now;
    }
    *rx = sx;
    *ry = s.mouse_invert_y ? -sy : sy;
    g_mouse_tilt_x = *rx, g_mouse_tilt_y = *ry;
}

}  // namespace

// xboxrecomp: when the mouse wheel last stepped the keys' pace (menu.cpp shows it a moment)
std::atomic<unsigned long long> g_pace_note{0};

const char* KeyName(uint32_t vk) {
    for (const auto& k : kKeyNames)
        if (k.vk == vk) return k.name;
    static thread_local char buf[16];
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        buf[0] = char(vk);
        buf[1] = 0;
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        std::snprintf(buf, sizeof buf, "F%u", vk - VK_F1 + 1);
    } else {
        std::snprintf(buf, sizeof buf, "0x%02X", vk);
    }
    return buf;
}

int KeyFromName(const char* name) {
    if (!name || !*name) return 0;
    for (const auto& k : kKeyNames)
        if (!_stricmp(name, k.name)) return int(k.vk);
    if (!name[1]) {
        char c = char(std::toupper(static_cast<unsigned char>(name[0])));
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) return c;
    }
    if ((name[0] == 'F' || name[0] == 'f') && std::isdigit(static_cast<unsigned char>(name[1]))) {
        int n = std::atoi(name + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        long v = std::strtol(name, nullptr, 16);
        if (v > 0 && v < 256) return int(v);
    }
    return -1;
}

void InputMessage(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp) {
    if (!g_raw && hwnd) {
        RAWINPUTDEVICE rid{0x01, 0x02, 0, static_cast<HWND>(hwnd)};  // generic desktop, mouse
        g_raw = RegisterRawInputDevices(&rid, 1, sizeof rid) != FALSE;
    }
    switch (msg) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            if (wp < 256) {
                if (g_capture >= 0 && wp != VK_F10) g_captured = uint32_t(wp);
                g_down[wp] = 1;
            }
            break;
        case WM_KEYUP: case WM_SYSKEYUP:
            if (wp < 256) g_down[wp] = 0;
            break;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
            if (g_capture >= 0) g_captured = MouseVk(msg, wp);
            g_down[MouseVk(msg, wp)] = 1;
            break;
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
            g_down[MouseVk(msg, wp)] = 0;
            break;
        case WM_MOUSEWHEEL: {
            // xboxrecomp: the movement keys' pace, a step a notch: up faster, down slower, held at sneak and run. Not
            // while the panel is open (it scrolls) or keyboard and mouse are off
            static int wheel;
            Settings& s = Get();
            if (!s.kbm || MenuOpen()) break;
            wheel += GET_WHEEL_DELTA_WPARAM(wp);
            int notches = wheel / WHEEL_DELTA;
            if (!notches) break;
            wheel -= notches * WHEEL_DELTA;
            uint32_t v = uint32_t(std::clamp(int(s.move_mode) + notches, 0, int(kMoveModes) - 1));
            std::fprintf(stderr, "[WHEEL] %d notch(es): movement keys %s\n", notches, kMoveModeNames[v]);
            g_pace_note = GetTickCount64();  // menu.cpp shows the pace a moment
            if (v != s.move_mode) {
                s.move_mode = v;
                Save();
            }
            break;
        }
        case WM_INPUT: {
            RAWINPUT ri;
            UINT size = sizeof ri;
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) !=
                    UINT(-1) &&
                ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                g_mouse_dx += ri.data.mouse.lLastX;
                g_mouse_dy += ri.data.mouse.lLastY;
            }
            break;
        }
        case WM_SETFOCUS:
            g_focus = true;
            break;
        case WM_KILLFOCUS:
            g_focus = false;
            for (auto& d : g_down) d = 0;
            g_mouse_dx = g_mouse_dy = 0;
            break;
        case WM_ACTIVATE:
            g_focus = LOWORD(wp) != WA_INACTIVE;
            if (!g_focus)
                for (auto& d : g_down) d = 0;
            break;
        default:
            break;
    }
}

// xboxrecomp: keyboard and mouse also play whenever no controller is on port 0 (unplugged mid-game too), with
// keyboard_mouse off; the setting keeps them merged on top of a connected controller
static std::atomic<bool> g_pad0{true};
static bool KbmActive() { return Get().kbm || !g_pad0.load(std::memory_order_relaxed); }

// xboxrecomp: Button prompts (F10 Controls). Automatic follows the device played with last: a key, mouse button or
// mouse move against a controller button or a stick pushed past half (app_InputPad); before either, keys when no
// controller answers on port 0. d3d12.cpp draws the game's button pictures as the bound keys while this says so
static std::atomic<int> g_device{-1};  // 1 keyboard and mouse, 0 controller, -1 not yet

bool PromptsKbmNow() {
    const Settings& s = Get();
    if (s.button_prompts == 1) return false;
    if (s.button_prompts == 2) return true;
    const int d = g_device.load(std::memory_order_relaxed);
    return d < 0 ? !g_pad0.load(std::memory_order_relaxed) : d == 1;
}

static void NoteDevice(bool kbm) {
    if (g_device.exchange(kbm ? 1 : 0) != (kbm ? 1 : 0))
        std::fprintf(stderr, "[PROMPTS] playing with the %s\n", kbm ? "keyboard and mouse" : "controller");
}

void InputFrame() {
    // the pointer held in the game window while keyboard and mouse play, the window has the focus and no panel is up
    HWND w = static_cast<HWND>(Window());
    bool want = KbmActive() && w && GetForegroundWindow() == w && !MenuOpen();
    if (want) {
        RECT r;
        if (GetClientRect(w, &r)) {
            POINT a{r.left, r.top}, b{r.right, r.bottom};
            ClientToScreen(w, &a);
            ClientToScreen(w, &b);
            RECT c{a.x, a.y, b.x, b.y};
            ClipCursor(&c);
            g_clipped = true;
        }
    } else if (g_clipped) {
        ClipCursor(nullptr);
        g_clipped = false;
    }
}

void StartKeyCapture(int binding) {
    g_captured = ~0u;
    g_capture = binding;
}

int KeyCaptureBinding() { return g_capture; }

bool TakeCapturedKey(uint32_t* vk) {
    uint32_t v = g_captured.exchange(~0u);
    if (v == ~0u) return false;
    g_capture = -1;
    *vk = v == VK_ESCAPE ? 0 : v;  // Escape clears the binding
    return true;
}

void CancelKeyCapture() { g_capture = -1; }

}  // namespace app

// xboxrecomp: the sneak toggle. HP:PS has no sneak button: Harry's pace follows how far the left stick is pushed
// (the movement code at 0x855EA: over 0.01 sneak, over his walkstartfraction walk, over runstartfraction run;
// the pace lands in [controller + 0x2574]), which keyboard keys,
// always all the way, never reach and a wide deadzone makes hard. Toggled on, the stick is scaled down to the sneak
// band: a gentle push, as a careful thumb would give the console: pace_sneak % of it (PaceStick below).
namespace app {
static std::atomic<bool> g_sneak{false};
bool SneakOn() { return g_sneak.load(std::memory_order_relaxed); }

static void SneakToggle(const Settings& s, const uint8_t in[kPadInputs], bool keys) {
    static bool key_was, pad_was;
    bool key = keys && s.keys[kKeySneak] && Held(s.keys[kKeySneak]);
    bool pad = s.sneak_pad < kPadInputs && in[s.sneak_pad] > 128;
    if ((key && !key_was) || (pad && !pad_was)) g_sneak.store(!g_sneak.load());
    key_was = key;
    pad_was = pad;
}

// The keys' push for sneaking and walking, a share of the stick's full push (F10 > Controls; hpps.toml pace_sneak /
// pace_walk, %). Fixed: learnt from the pace the game picked (2026-10-25) it went wrong, the game easing Harry from one
// gait to the next gave answers no threshold fits. From play: 44% is a quick sneak, 64% already runs
static void PaceStick(int16_t* x, int16_t* y, uint32_t percent) {
    float k = std::clamp(float(percent) / 100.0f, 0.05f, 1.0f);
    *x = int16_t(float(*x) * k);
    *y = int16_t(float(*y) * k);
}

static void SneakStick(int16_t* x, int16_t* y) { PaceStick(x, y, Get().pace_sneak); }

// xboxrecomp: the movement keys' pace (F10 > Controls, the mouse wheel), as in HP:CoS: the keys push the stick all
// the way, which is a run; at Sneak or Walk their push is brought down to that pace the same way as the sneak toggle
static void KeysPace(const Settings& s, int16_t* x, int16_t* y) {
    if (s.move_mode == kMoveSneak) SneakStick(x, y);
    else if (s.move_mode == kMoveWalk) PaceStick(x, y, s.pace_walk);
}
}  // namespace app

// hle_input.c: a pad read for the game, after the PC's controller filled it in
extern "C" void app_InputPad(int port, XBOX_GAMEPAD* g) {
    const app::Settings& s = app::Get();
    if (!s.controller) std::memset(g, 0, sizeof(*g));  // xboxrecomp: "Use controller" off: the pad at rest
    uint8_t in[app::kPadInputs], out[app::kPadInputs];
    app::Read(*g, in);
    if (port == 0) app::g_mouse_tilt_x = app::g_mouse_tilt_y = 0;  /* set again below only while the mouse plays */
    for (uint32_t i = 0; i < app::kPadInputs; i++) out[i] = in[std::min(s.pad_map[i], app::kPadInputs - 1)];
    // the sneak toggle's pad button is the toggle's only, not the game's too
    if (port == 0 && s.sneak_pad < app::kPadInputs) {
        for (uint32_t i = 0; i < app::kPadInputs; i++)
            if (std::min(s.pad_map[i], app::kPadInputs - 1) == s.sneak_pad) out[i] = 0;
    }
    app::Shape(&g->sThumbLX, &g->sThumbLY, s.deadzone_left, s.sensitivity_left);
    app::Shape(&g->sThumbRX, &g->sThumbRY, s.deadzone_right, s.sensitivity_right);
    // the controller's camera speed past the game's (hle_camera.c, as the mouse's boost): while the right stick turns
    if (port == 0) app::g_pad_boost = (g->sThumbRX || g->sThumbRY) ? s.stick_camera_speed / 100.0f : 1.0f;
    // Button prompts' Automatic: which device this read came from (the controller's before the keys go on top)
    bool pad_used = false, kbm_used = false;
    if (port == 0) {
        for (uint32_t i = 0; i < app::kPadInputs; i++) pad_used |= in[i] > 64;
        pad_used |= std::abs(int(g->sThumbLX)) > 16000 || std::abs(int(g->sThumbLY)) > 16000 ||
                    std::abs(int(g->sThumbRX)) > 16000 || std::abs(int(g->sThumbRY)) > 16000;
    }
    if (port == 0 && app::KbmActive() && GetForegroundWindow() == static_cast<HWND>(app::Window()) && !app::MenuOpen()) {
        for (uint32_t i = 0; i < app::kKeyBinds; i++) kbm_used |= s.keys[i] && app::Held(s.keys[i]);
        for (uint32_t i = 0; i < app::kPadInputs; i++)
            if (app::Held(s.keys[i])) out[i] = 255;
        float lx = float(app::Held(s.keys[19])) - float(app::Held(s.keys[18]));
        float ly = float(app::Held(s.keys[16])) - float(app::Held(s.keys[17]));
        if (lx || ly) {
            float m = std::sqrt(lx * lx + ly * ly);
            g->sThumbLX = app::Clamp16(lx / m);
            g->sThumbLY = app::Clamp16(ly / m);
            if (port == 0 && !app::SneakOn()) app::KeysPace(s, &g->sThumbLX, &g->sThumbLY);
        }
        float rx, ry;
        app::MouseStick(s, &rx, &ry);
        kbm_used |= std::fabs(rx) > 0.2f || std::fabs(ry) > 0.2f;
        rx += float(app::Held(s.keys[23])) - float(app::Held(s.keys[22]));
        ry += float(app::Held(s.keys[20])) - float(app::Held(s.keys[21]));
        if (rx || ry) {
            app::g_pad_boost = 1.0f;  // the mouse or the camera keys turn it, not the stick
            float m = std::sqrt(rx * rx + ry * ry);
            if (m > 1.0f) rx /= m, ry /= m;
            g->sThumbRX = app::Clamp16(rx);
            g->sThumbRY = app::Clamp16(ry);
        }
    }
    if (pad_used != kbm_used) app::NoteDevice(kbm_used);
    if (s.invert_x) g->sThumbRX = int16_t(std::max(-32767, -int(g->sThumbRX)));
    if (s.invert_y) g->sThumbRY = int16_t(std::max(-32767, -int(g->sThumbRY)));
    if (port == 0) {
        const bool keys = app::KbmActive() && GetForegroundWindow() == static_cast<HWND>(app::Window()) &&
                          !app::MenuOpen();
        app::SneakToggle(s, in, keys);
        if (app::SneakOn()) app::SneakStick(&g->sThumbLX, &g->sThumbLY);
    }
    app::Write(*g, out);
}

// hle_camera.c: how far past the stick's full tilt the mouse asks the camera to turn (1 at most the stick's), X and Y
// (or the controller camera speed while the right stick turns it)
extern "C" float app_MouseBoostX(void) {
    return std::min(4.0f, std::max(app::g_pad_boost, std::fabs(app::g_mouse_tilt_x)));
}
extern "C" float app_MouseBoostY(void) {
    return std::min(4.0f, std::max(app::g_pad_boost, std::fabs(app::g_mouse_tilt_y)));
}

// hle_input.c: keyboard and mouse play as controller 1 (port 0 is there with no controller plugged in)
extern "C" int app_KbmOn(void) { return app::KbmActive(); }

// d3d12.cpp: the button pictures as keys now, and the key bound to a pad input (kPadInputNames' order), 0 unbound
extern "C" int app_PromptsKbm(void) { return app::PromptsKbmNow(); }
extern "C" uint32_t app_PadInputKey(int pad_input) {
    return pad_input >= 0 && pad_input < int(app::kPadInputs) ? app::Get().keys[pad_input] : 0;
}
// hle_frontend.c: the key bound to a pad input as the game's text names it ("Space", "E", "Left Click"), "" unbound
extern "C" const char* app_ButtonKeyLabel(int pad_input) {
    const uint32_t vk = app_PadInputKey(pad_input);
    switch (vk) {
        case 0: return "";
        case 0x01: return "Left Click";
        case 0x02: return "Right Click";
        case 0x04: return "Middle Click";
        case 0x05: return "Mouse 4";
        case 0x06: return "Mouse 5";
        default: return app::KeyName(vk);
    }
}
// hle_input.c, after each read of port 0: whether a controller answered
extern "C" void app_InputPad0Present(int present) { app::g_pad0.store(present != 0, std::memory_order_relaxed); }

// hle_input.c: the game's rumble before it goes to the PC's controller
extern "C" void app_InputRumble(uint16_t* left, uint16_t* right) {
    const app::Settings& s = app::Get();
    uint32_t k = s.vibration && s.controller ? std::min(s.vibration_strength, 100u) : 0;
    *left = uint16_t(*left * k / 100);
    *right = uint16_t(*right * k / 100);
}
