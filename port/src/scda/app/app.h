// The part of SCDA-XBERecomp's app layer (src/app/app.h) its renderer reads:
// settings, the window, the output shape, the F10 options panel. Settings live
// in hpps.toml beside the exe as SCDA's scda.toml does (app.cpp); a RECOMP_*
// environment variable set at launch wins over its line. The window is
// xboxrecomp's framebuffer window.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>

namespace app {

enum class DisplayMode : uint32_t { Windowed, Borderless, Fullscreen };  // as xboxrecomp's XBOX_DISPLAY_*
enum class AspectMode : uint32_t { Standard, Wide, Stretch };            // 4:3, 16:9, stretch
constexpr uint32_t kFpsUnlocked = 1000;  // fps_limit 'unlocked' (experimental): Swap and vblank waits do not wait
enum ColorBlind : uint32_t { kCbOff, kCbProtanopia, kCbDeuteranopia, kCbTritanopia };

// xboxrecomp: the pad's inputs in one order, the analog buttons as the Xbox's bAnalogButtons (A B X Y Black White LT
// RT) then the digital ones as wButtons' bits (Up Down Left Right Start Back LThumb RThumb)
constexpr uint32_t kPadInputs = 16;
inline const char* const kPadInputNames[kPadInputs] = {"A",    "B",     "X",     "Y",     "Black", "White",
                                                       "LT",   "RT",    "Up",    "Down",  "Left",  "Right",
                                                       "Start", "Back", "LThumb", "RThumb"};
// key bindings: the 16 pad inputs, then the left stick (forward back left right), the camera (up down left right)
// and the sneak toggle (input.cpp)
constexpr uint32_t kKeyBinds = kPadInputs + 9;
constexpr uint32_t kKeySneak = kPadInputs + 8;
inline const char* const kKeyBindNames[kKeyBinds] = {
    "A", "B", "X", "Y", "Black", "White", "LT", "RT", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Start",
    "Back", "Left stick click", "Right stick click", "Move forward", "Move back", "Move left", "Move right",
    "Camera up", "Camera down", "Camera left", "Camera right", "Sneak (toggle)"};
// Space, E, left mouse, right mouse, R, Q, Shift, Ctrl, 1-4, Escape, Tab, C, middle mouse; W S A D; arrows; Caps Lock
#define kDefaultKeys                                                                                          \
    0x20, 'E', 0x01, 0x02, 'R', 'Q', 0x10, 0x11, '1', '2', '3', '4', 0x1B, 0x09, 'C', 0x04, 'W', 'S', 'A', 'D', \
        0x26, 0x28, 0x25, 0x27, 0x14

struct Settings {
    DisplayMode display_mode = DisplayMode::Windowed;  // RECOMP_DISPLAY_MODE
    uint32_t width = 1920, height = 1080;  // RECOMP_WINDOW_SIZE: the window, or the fullscreen mode; 0 the desktop's
    AspectMode aspect = AspectMode::Standard;  // RECOMP_ASPECT; read at boot, the title picks its camera then
    uint32_t internal_height = 0;      // RECOMP_INTERNAL_HEIGHT: 3D render lines, 0 follows the window
    uint32_t render_percent = 100;     // RECOMP_RENDER_PERCENT, of the window height
    uint32_t render_max_height = 1080; // RECOMP_RENDER_MAX_HEIGHT, 0 no cap
    uint32_t brightness = 50;          // 0 to 100, 50 each leaves the picture alone
    uint32_t contrast = 50;
    bool fps_counter = false;
    uint32_t fov = 100;                // xboxrecomp: field of view, % of the game's (hle_video.c), 80 to 130
    bool vsync = true;                 // RECOMP_VSYNC
    bool cutscene_bars = true;         // xboxrecomp: 16:9's zoomed cutscenes keep the console's black bars over the picture
    bool debug_logs = false;           // RECOMP_DEBUG_LOGS: hpps_run.log (launcher) and xbox_kernel.log, from the next start
    uint32_t aniso = 0;                // RECOMP_ANISO, 0 as the game asks
    uint32_t msaa = 0;                 // RECOMP_MSAA samples, 0 off
    bool smaa = false;                 // RECOMP_SMAA, over the finished frame; combines with msaa
    uint32_t shadow_resolution = 512;  // SCDA's shadow-map path; this title has none
    // SCDA's shader model 3 effects, kept off for this title (no setting)
    uint32_t shader_model = 1;
    bool hdr_rendering = false;
    bool tone_mapping = false;
    uint32_t ambient_occlusion = 0;    // SCDA's needs a hook into the game's translucent pass; none here yet
    bool controller = true;            // xboxrecomp: false: the PC's controller is read as at rest (keyboard and mouse stay)
    bool async_shaders = true;         // RECOMP_ASYNC_SHADERS=0: wait for every pipeline
    bool show_launcher = true;         // xboxrecomp: hpps_launcher.exe shows its page; false starts the game straight away (Shift shows it)
    bool start_menu = false;           // xboxrecomp: the settings page before the game starts (RECOMP_START_MENU); hpps_launcher.exe is the usual way
    // xboxrecomp: frame rate, accessibility and controls (input.cpp, the final pass)
    uint32_t fps_limit = 0;            // 0 the game's own rate, 30: each frame two vblanks at least, kFpsUnlocked none
    uint32_t colorblind = 0;           // ColorBlind: correction filter over the finished frame
    uint32_t colorblind_strength = 100;  // 0 to 100 %
    // xboxrecomp (from hpcos): post-processing (d3d12.cpp): bloom over the 3D scene before the HUD, sharpening over
    // the finished frame. Off by default: the console's picture
    bool bloom = false;
    uint32_t bloom_strength = 40;      // 0 to 100
    uint32_t sharpen = 0;              // 0 (off) to 100
    bool bean_names = true;            // xboxrecomp: a bean's flavour shown on the screen as Harry picks it up (hle_trainer.c)
    uint32_t speakers = 2;             // xboxrecomp: 2 stereo, 6 5.1 surround (hle_dsound.c, at start)
    uint32_t language = 0;             // xboxrecomp: the language the game starts in: 0 English, 1 French, 2 German,
                                       // 3 Spanish, 4 Italian, 5 Dutch, 6 Swedish (RECOMP_LANGUAGE, hle_frontend.c)
    bool kbm = true;                   // keyboard and mouse drive pad 1 alongside a controller
    uint32_t mouse_sensitivity = 50;   // 1 to 100
    bool mouse_invert_y = false;
    uint32_t deadzone_left = 10, deadzone_right = 10;          // % of full tilt read as centred
    uint32_t sensitivity_left = 100, sensitivity_right = 100;  // % response past the deadzone
    bool invert_x = false, invert_y = false;                   // the camera (right stick and mouse)
    bool vibration = true;
    uint32_t vibration_strength = 100;  // %
    uint32_t pad_map[kPadInputs] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};  // game input <- pad input
    uint32_t move_mode = 2;
    uint32_t pace_sneak = 44, pace_walk = 54;  // xboxrecomp: the keys' push sneaking / walking, % of full  // xboxrecomp: the movement keys' pace: kMoveSneak, kMoveWalk, kMoveRun (the mouse wheel)
    uint32_t sneak_pad = kPadInputs;  // xboxrecomp: the pad input that toggles sneaking (then not the game's), kPadInputs none
    uint32_t keys[kKeyBinds] = {kDefaultKeys};  // VK codes (1 2 4 5 6 the mouse buttons), 0 unbound
};

Settings& Get();
void Save();
// xboxrecomp: the movement keys' paces (input.cpp tunes the keys' push to each; the mouse wheel steps them), as in
// HP:CoS, with HP:PS's own three: sneak, walk, run
enum MoveMode : uint32_t { kMoveSneak, kMoveWalk, kMoveRun, kMoveModes };
extern std::atomic<unsigned long long> g_pace_note;  // GetTickCount64 when the wheel last stepped it (menu.cpp shows it)
inline const char* const kMoveModeNames[kMoveModes] = {"Sneak", "Walk", "Run"};
// re-reads hpps.toml when it changed on disk; the renderer calls it once a frame
void Poll();
// the window to the display settings, any thread
void ApplyDisplay();
void* Window();  // HWND of the framebuffer window
float RenderScale(uint32_t guest_height);
// the output's shape: 4:3, or the window's own for 16:9 / stretch
float Aspect();
AspectMode BootAspect();  // the aspect setting this run started with
void SetBootAspect(AspectMode a);  // xboxrecomp: the start menu's choice, before the title asks
inline float Narrow() { return 4.0f / 3.0f / Aspect(); }
inline bool Widescreen() { return Aspect() > 1.34f; }
std::filesystem::path ExeDir();

// options panel (menu.cpp), drawn by the renderer each frame with the lock held
std::recursive_mutex& UiLock();
bool MenuOpen();
void ToggleMenu();
// xboxrecomp: the start menu, shown before the title's first frame: open while it waits for Start
void OpenStartMenu();
bool StartMenuOpen();
void InitUi();  // ImGui's context and Win32 side, once the window is up
void DrawUi();
// xboxrecomp: controls (input.cpp): key names as hpps.toml spells them, the window's keys and mouse, the pointer held
// in the window, and the menu's "press a key" for a binding (Escape clears it)
const char* KeyName(uint32_t vk);
int KeyFromName(const char* name);  // -1 not a key name
void InputMessage(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp);
void InputFrame();
void StartKeyCapture(int binding);
int KeyCaptureBinding();
bool TakeCapturedKey(uint32_t* vk);
void CancelKeyCapture();
bool SneakOn();  // xboxrecomp: the sneak toggle is on (input.cpp)
inline std::atomic<float> g_fps{0};

}  // namespace app
