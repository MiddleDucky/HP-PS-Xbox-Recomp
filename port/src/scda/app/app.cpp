// Host settings and window for the ported SCDA renderer. See app.h.
//
// hpps.toml beside the exe holds the settings, one `key = value` line each, as
// SCDA's scda.toml; it is written with every key on first run and by the F10
// panel, and re-read while the game runs when it changes on disk. A RECOMP_*
// variable in the environment at launch overrides its line for that run
// (debug.env beside the exe, debugging) and is not written back.
#include "app/app.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "core/log.h"

extern "C" void* xbox_FramebufferWindowHwnd(void);
extern "C" void xbox_FramebufferWindowStart(void);
extern "C" void xbox_FramebufferWindowSetDisplay(int mode, int w, int h);

namespace app {

namespace {

enum class Kind { U32, Bool, Display, Aspect, Key, Pad, PadOpt, ColorBlind, Fps, Language };  // PadOpt: a pad input or 'none'

struct Entry {
    const char* key;  // in hpps.toml
    const char* env;  // RECOMP_* that overrides it, or null
    Kind kind;
    size_t off;       // into Settings
    uint32_t lo, hi;  // U32 range
    const char* note; // the comment line above it in the file
};

#define FIELD(m) offsetof(Settings, m)
const Entry kEntries[] = {
    // xboxrecomp: first in the file, where a player looks for how to get the launcher's page back
    {"show_launcher", nullptr, Kind::Bool, FIELD(show_launcher), 0, 0,
     "the launcher's settings page at startup: true shows it, false starts the game at once (also: hold Shift while "
     "starting, or F10 > Show the launcher at startup)"},
    {"display_mode", "RECOMP_DISPLAY_MODE", Kind::Display, FIELD(display_mode), 0, 0,
     "'windowed', 'borderless' (covers the monitor) or 'fullscreen' (switches the monitor to width x height)"},
    {"width", nullptr, Kind::U32, FIELD(width), 0, 16384,
     "window size, or the fullscreen mode; 0 0 is the desktop's (RECOMP_WINDOW_SIZE=WxH overrides both)"},
    {"height", nullptr, Kind::U32, FIELD(height), 0, 16384, nullptr},
    {"aspect", "RECOMP_ASPECT", Kind::Aspect, FIELD(aspect), 0, 0,
     "'4:3' (black bars), '16:9' (the game's widescreen camera) or 'stretch'; takes a restart"},
    {"internal_height", "RECOMP_INTERNAL_HEIGHT", Kind::U32, FIELD(internal_height), 0, 8640,
     "internal resolution: lines the 3D is drawn at (480 is the Xbox's, 960 2x, 1440 3x, 2160 4.5x); 0 follows the window"},
    {"render_percent", "RECOMP_RENDER_PERCENT", Kind::U32, FIELD(render_percent), 25, 400,
     "with internal_height 0: the render height as % of the window's"},
    {"render_max_height", "RECOMP_RENDER_MAX_HEIGHT", Kind::U32, FIELD(render_max_height), 0, 8640,
     "with internal_height 0: the most lines it follows the window to, 0 no limit"},
    {"msaa", "RECOMP_MSAA", Kind::U32, FIELD(msaa), 0, 8, "multisample anti-aliasing: 0 off, 2, 4 or 8 samples"},
    {"smaa", "RECOMP_SMAA", Kind::Bool, FIELD(smaa), 0, 0,
     "SMAA anti-aliasing over the finished frame; combines with msaa"},
    {"anisotropic_filtering", "RECOMP_ANISO", Kind::U32, FIELD(aniso), 0, 16, "0 as the game asks, 2, 4, 8 or 16"},
    {"brightness", nullptr, Kind::U32, FIELD(brightness), 0, 100, "0 to 100, 50 leaves the picture alone"},
    {"contrast", nullptr, Kind::U32, FIELD(contrast), 0, 100, nullptr},
    {"vsync", "RECOMP_VSYNC", Kind::Bool, FIELD(vsync), 0, 0, nullptr},
    {"fps_counter", nullptr, Kind::Bool, FIELD(fps_counter), 0, 0, nullptr},
    {"fov", "RECOMP_FOV", Kind::U32, FIELD(fov), 80, 130, "field of view, % of the game's"},
    {"cutscene_bars", nullptr, Kind::Bool, FIELD(cutscene_bars), 0, 0,
     "16:9: cutscenes fill the screen and keep the console's black bars over the picture (subtitles in the bottom one)"},
    {"debug_logs", "RECOMP_DEBUG_LOGS", Kind::Bool, FIELD(debug_logs), 0, 0,
     "extra traces in the run log (speech and the like), from the next start (F10 > Debug). The logs themselves are "
     "always written: logs\\runs\\hpps_run_<date_time>.log and xbox_kernel_<date_time>.log, the newest 10 runs kept"},
    {"controller", nullptr, Kind::Bool, FIELD(controller), 0, 0,
     "false: the game ignores the PC's controller (keyboard and mouse still play)"},
    {"async_shaders", "RECOMP_ASYNC_SHADERS", Kind::Bool, FIELD(async_shaders), 0, 0,
     "a draw whose pipeline is still compiling is skipped instead of waited for"},
    {"start_menu", "RECOMP_START_MENU", Kind::Bool, FIELD(start_menu), 0, 0,
     "the settings page before the game starts (aspect ratio, resolution, quality)"},
    {"fps_limit", "RECOMP_FPS_LIMIT", Kind::Fps, FIELD(fps_limit), 0, 0,
     "frame rate: 0 the game's own (59.94), 30 caps it at 30 fps, 'unlocked' as fast as vsync / the PC allow (experimental: the game was made for 60)"},
    {"language", "RECOMP_LANGUAGE", Kind::Language, FIELD(language), 0, 0,
     "the language the game starts in: 'english', 'french', 'german', 'spanish', 'italian', 'dutch' or 'swedish'; takes a restart"},
    {"speakers", "RECOMP_SPEAKERS", Kind::U32, FIELD(speakers), 2, 6,
     "2 stereo, 6 5.1 surround (the console's speaker mix, each channel to its own speaker); takes a restart"},
    {"colorblind", nullptr, Kind::ColorBlind, FIELD(colorblind), 0, 0,
     "colour-blind correction over the finished frame: 'off', 'protanopia', 'deuteranopia' or 'tritanopia'"},
    {"colorblind_strength", nullptr, Kind::U32, FIELD(colorblind_strength), 0, 100, "0 to 100 %"},
    {"bloom", "RECOMP_BLOOM", Kind::Bool, FIELD(bloom), 0, 0, "a glow round the 3D scene's bright parts, under the HUD"},
    {"bloom_strength", nullptr, Kind::U32, FIELD(bloom_strength), 0, 100, "0 to 100"},
    {"sharpen", "RECOMP_SHARPEN", Kind::U32, FIELD(sharpen), 0, 100, "0 (off) to 100: contrast adaptive sharpening"},
    {"bean_names", "RECOMP_BEAN_NAMES", Kind::Bool, FIELD(bean_names), 0, 0,
     "a bean's flavour said aloud as Harry picks it up, as the game does; false: not spoken"},
    {"keyboard_mouse", nullptr, Kind::Bool, FIELD(kbm), 0, 0,
     "keyboard and mouse play as controller 1, alongside a pad (key names below; Mouse1-5 the mouse buttons)"},
    {"mouse_sensitivity", nullptr, Kind::U32, FIELD(mouse_sensitivity), 1, 100, "1 to 100, the camera's speed with the mouse"},
    {"move_mode", nullptr, Kind::U32, FIELD(move_mode), 0, 2,
     "the movement keys' pace: 0 sneak, 1 walk, 2 run; the mouse wheel steps it"},
    {"pace_sneak", nullptr, Kind::U32, FIELD(pace_sneak), 5, 100,
     "the keys' push sneaking (and the sneak toggle's), % of the stick's full push"},
    {"pace_walk", nullptr, Kind::U32, FIELD(pace_walk), 5, 100, "the keys' push walking, %"},
    {"mouse_invert_y", nullptr, Kind::Bool, FIELD(mouse_invert_y), 0, 0, nullptr},
    {"deadzone_left", nullptr, Kind::U32, FIELD(deadzone_left), 0, 50,
     "controller: % of full tilt read as centred, the stick's response past it in %, the camera's axes inverted"},
    {"deadzone_right", nullptr, Kind::U32, FIELD(deadzone_right), 0, 50, nullptr},
    {"sensitivity_left", nullptr, Kind::U32, FIELD(sensitivity_left), 25, 200, nullptr},
    {"sensitivity_right", nullptr, Kind::U32, FIELD(sensitivity_right), 25, 200, nullptr},
    {"invert_camera_x", nullptr, Kind::Bool, FIELD(invert_x), 0, 0, nullptr},
    {"invert_camera_y", nullptr, Kind::Bool, FIELD(invert_y), 0, 0, nullptr},
    {"vibration", nullptr, Kind::Bool, FIELD(vibration), 0, 0, nullptr},
    {"vibration_strength", nullptr, Kind::U32, FIELD(vibration_strength), 0, 100, "%"},
    {"pad_a", nullptr, Kind::Pad, FIELD(pad_map[0]), 0, 0, "controller remapping: the pad button each game button reads (A B X Y Black White LT RT Up Down Left Right Start Back LThumb RThumb; on a 360/One pad LB is White, RB Black)"},
    {"pad_b", nullptr, Kind::Pad, FIELD(pad_map[1]), 0, 0, nullptr},
    {"pad_x", nullptr, Kind::Pad, FIELD(pad_map[2]), 0, 0, nullptr},
    {"pad_y", nullptr, Kind::Pad, FIELD(pad_map[3]), 0, 0, nullptr},
    {"pad_black", nullptr, Kind::Pad, FIELD(pad_map[4]), 0, 0, nullptr},
    {"pad_white", nullptr, Kind::Pad, FIELD(pad_map[5]), 0, 0, nullptr},
    {"pad_lt", nullptr, Kind::Pad, FIELD(pad_map[6]), 0, 0, nullptr},
    {"pad_rt", nullptr, Kind::Pad, FIELD(pad_map[7]), 0, 0, nullptr},
    {"pad_up", nullptr, Kind::Pad, FIELD(pad_map[8]), 0, 0, nullptr},
    {"pad_down", nullptr, Kind::Pad, FIELD(pad_map[9]), 0, 0, nullptr},
    {"pad_left", nullptr, Kind::Pad, FIELD(pad_map[10]), 0, 0, nullptr},
    {"pad_right", nullptr, Kind::Pad, FIELD(pad_map[11]), 0, 0, nullptr},
    {"pad_start", nullptr, Kind::Pad, FIELD(pad_map[12]), 0, 0, nullptr},
    {"pad_back", nullptr, Kind::Pad, FIELD(pad_map[13]), 0, 0, nullptr},
    {"pad_lthumb", nullptr, Kind::Pad, FIELD(pad_map[14]), 0, 0, nullptr},
    {"pad_rthumb", nullptr, Kind::Pad, FIELD(pad_map[15]), 0, 0, nullptr},
    {"key_a", nullptr, Kind::Key, FIELD(keys[0]), 0, 0, "keyboard and mouse bindings: a key name (A-Z, 0-9, F1-F12, Space, Enter, Escape, Tab, Shift, Ctrl, Alt, Up, Down, Left, Right, Num0-Num9, ...), Mouse1-Mouse5 or none"},
    {"key_b", nullptr, Kind::Key, FIELD(keys[1]), 0, 0, nullptr},
    {"key_x", nullptr, Kind::Key, FIELD(keys[2]), 0, 0, nullptr},
    {"key_y", nullptr, Kind::Key, FIELD(keys[3]), 0, 0, nullptr},
    {"key_black", nullptr, Kind::Key, FIELD(keys[4]), 0, 0, nullptr},
    {"key_white", nullptr, Kind::Key, FIELD(keys[5]), 0, 0, nullptr},
    {"key_lt", nullptr, Kind::Key, FIELD(keys[6]), 0, 0, nullptr},
    {"key_rt", nullptr, Kind::Key, FIELD(keys[7]), 0, 0, nullptr},
    {"key_up", nullptr, Kind::Key, FIELD(keys[8]), 0, 0, nullptr},
    {"key_down", nullptr, Kind::Key, FIELD(keys[9]), 0, 0, nullptr},
    {"key_left", nullptr, Kind::Key, FIELD(keys[10]), 0, 0, nullptr},
    {"key_right", nullptr, Kind::Key, FIELD(keys[11]), 0, 0, nullptr},
    {"key_start", nullptr, Kind::Key, FIELD(keys[12]), 0, 0, nullptr},
    {"key_back", nullptr, Kind::Key, FIELD(keys[13]), 0, 0, nullptr},
    {"key_lthumb", nullptr, Kind::Key, FIELD(keys[14]), 0, 0, nullptr},
    {"key_rthumb", nullptr, Kind::Key, FIELD(keys[15]), 0, 0, nullptr},
    {"key_move_forward", nullptr, Kind::Key, FIELD(keys[16]), 0, 0, nullptr},
    {"key_move_back", nullptr, Kind::Key, FIELD(keys[17]), 0, 0, nullptr},
    {"key_move_left", nullptr, Kind::Key, FIELD(keys[18]), 0, 0, nullptr},
    {"key_move_right", nullptr, Kind::Key, FIELD(keys[19]), 0, 0, nullptr},
    {"key_camera_up", nullptr, Kind::Key, FIELD(keys[20]), 0, 0, nullptr},
    {"key_camera_down", nullptr, Kind::Key, FIELD(keys[21]), 0, 0, nullptr},
    {"key_camera_left", nullptr, Kind::Key, FIELD(keys[22]), 0, 0, nullptr},
    {"key_camera_right", nullptr, Kind::Key, FIELD(keys[23]), 0, 0, nullptr},
    {"key_sneak", nullptr, Kind::Key, FIELD(keys[kKeySneak]), 0, 0,
     "sneak toggle: press once to sneak (Harry's slowest pace, as a gentle push of the stick), again to walk and run"},
    {"pad_sneak", nullptr, Kind::PadOpt, FIELD(sneak_pad), 0, 0,
     "the controller button that toggles sneaking instead of its game action, or 'none' (a gentle push sneaks anyway)"},
};
#undef FIELD

constexpr const char* kDisplayNames[] = {"windowed", "borderless", "fullscreen"};
constexpr const char* kAspectNames[] = {"4:3", "16:9", "stretch"};
constexpr const char* kColorBlindNames[] = {"off", "protanopia", "deuteranopia", "tritanopia"};
// the languages this disc carries speech for (the main menu's flag list, hle_frontend.c's sub_0002A790)
constexpr const char* kLanguageNames[] = {"english", "french", "german", "spanish", "italian", "dutch", "swedish"};
constexpr uint32_t kLanguages = 7;

uint32_t& U(Settings& s, const Entry& e) { return *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(&s) + e.off); }
bool& B(Settings& s, const Entry& e) { return *reinterpret_cast<bool*>(reinterpret_cast<char*>(&s) + e.off); }

std::string Trim(std::string v) {
    size_t a = v.find_first_not_of(" \t\r"), b = v.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
}

// one value as the file or the environment spells it; false leaves the setting
bool Parse(Settings& s, const Entry& e, std::string v) {
    v = Trim(v);
    if (v.size() >= 2 && (v[0] == '\'' || v[0] == '"') && v.back() == v[0]) v = v.substr(1, v.size() - 2);
    switch (e.kind) {
        case Kind::U32: {
            char* end = nullptr;
            unsigned long n = std::strtoul(v.c_str(), &end, 0);
            if (end == v.c_str()) return false;
            U(s, e) = std::clamp<uint32_t>(uint32_t(n), e.lo, e.hi);
            return true;
        }
        case Kind::Bool:
            if (v == "true" || v == "1") B(s, e) = true;
            else if (v == "false" || v == "0") B(s, e) = false;
            else return false;
            return true;
        case Kind::Display:
            for (uint32_t i = 0; i < 3; i++) {
                if (!_stricmp(v.c_str(), kDisplayNames[i])) return U(s, e) = i, true;
            }
            return false;
        case Kind::Aspect:
            for (uint32_t i = 0; i < 3; i++) {
                if (!_stricmp(v.c_str(), kAspectNames[i])) return U(s, e) = i, true;
            }
            return false;
        case Kind::Fps:
            if (!_stricmp(v.c_str(), "unlocked")) return U(s, e) = kFpsUnlocked, true;
            if (v == "0" || v == "30") return U(s, e) = uint32_t(std::atoi(v.c_str())), true;
            return false;
        case Kind::ColorBlind:
            for (uint32_t i = 0; i < 4; i++) {
                if (!_stricmp(v.c_str(), kColorBlindNames[i])) return U(s, e) = i, true;
            }
            return false;
        case Kind::Language:
            for (uint32_t i = 0; i < kLanguages; i++) {
                if (!_stricmp(v.c_str(), kLanguageNames[i])) return U(s, e) = i, true;
            }
            return false;
        case Kind::Pad:
        case Kind::PadOpt:
            for (uint32_t i = 0; i < kPadInputs; i++) {
                if (!_stricmp(v.c_str(), kPadInputNames[i])) return U(s, e) = i, true;
            }
            if (e.kind == Kind::PadOpt && !_stricmp(v.c_str(), "none")) return U(s, e) = kPadInputs, true;
            return false;
        case Kind::Key: {
            int vk = KeyFromName(v.c_str());
            if (vk < 0) return false;
            U(s, e) = uint32_t(vk);
            return true;
        }
    }
    return false;
}

std::string Format(Settings& s, const Entry& e) {
    switch (e.kind) {
        case Kind::U32: return std::to_string(U(s, e));
        case Kind::Bool: return B(s, e) ? "true" : "false";
        case Kind::Display: return std::string("'") + kDisplayNames[std::min(U(s, e), 2u)] + "'";
        case Kind::Aspect: return std::string("'") + kAspectNames[std::min(U(s, e), 2u)] + "'";
        case Kind::Fps: return U(s, e) == kFpsUnlocked ? "'unlocked'" : std::to_string(U(s, e));
        case Kind::ColorBlind: return std::string("'") + kColorBlindNames[std::min(U(s, e), 3u)] + "'";
        case Kind::Language: return std::string("'") + kLanguageNames[std::min(U(s, e), kLanguages - 1)] + "'";
        case Kind::Pad: return std::string("'") + kPadInputNames[std::min(U(s, e), kPadInputs - 1)] + "'";
        case Kind::PadOpt:
            return U(s, e) >= kPadInputs ? std::string("'none'") : std::string("'") + kPadInputNames[U(s, e)] + "'";
        case Kind::Key: return std::string("'") + KeyName(U(s, e)) + "'";
    }
    return {};
}

struct State {
    std::mutex lock;
    Settings cur;          // what the game uses
    Settings file;         // what hpps.toml holds
    bool locked[std::size(kEntries)] = {};  // the environment set it at launch
    bool size_locked = false;               // RECOMP_WINDOW_SIZE
    AspectMode boot_aspect = AspectMode::Standard;
    std::filesystem::file_time_type stamp{};
    ULONGLONG checked = 0;
    bool ready = false;
};

State& St() {
    static State st;
    return st;
}

std::filesystem::path FilePath() { return ExeDir() / "hpps.toml"; }

// false when there is no file
bool ReadFile(Settings& s) {
    std::ifstream f(FilePath());
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (size_t c = line.find('#'); c != std::string::npos && line.find_first_of("'\"") > c) line.resize(c);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        for (const Entry& e : kEntries) {
            if (key == e.key && !Parse(s, e, line.substr(eq + 1)))
                LOGW("hpps.toml: %s = %s not understood, kept %s", e.key, Trim(line.substr(eq + 1)).c_str(),
                     Format(s, e).c_str());
        }
    }
    return true;
}

void WriteFile(Settings& s) {
    std::ostringstream o;
    o << "# Harry Potter and the Philosopher's Stone (xboxrecomp) settings, as SCDA's scda.toml.\n"
         "# F10 in game changes them too. Saved while the game runs, this file applies at once,\n"
         "# except aspect. A RECOMP_* environment variable of the same setting wins over its line.\n";
    for (const Entry& e : kEntries) {
        if (e.note) o << "\n# " << e.note << "\n";
        o << e.key << " = " << Format(s, e) << "\n";
    }
    std::error_code ec;
    std::filesystem::path p = FilePath(), tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << o.str();
        if (!f) {
            LOGW("hpps.toml: can't write %s", p.string().c_str());
            return;
        }
    }
    std::filesystem::rename(tmp, p, ec);
    if (ec) LOGW("hpps.toml: can't write %s: %s", p.string().c_str(), ec.message().c_str());
}

// file values into cur, but not over what the environment set
void Merge(State& st) {
    for (size_t i = 0; i < std::size(kEntries); i++) {
        const Entry& e = kEntries[i];
        if (st.locked[i] || (st.size_locked && (!std::strcmp(e.key, "width") || !std::strcmp(e.key, "height")))) continue;
        if (e.kind == Kind::Bool) B(st.cur, e) = B(st.file, e);
        else U(st.cur, e) = U(st.file, e);  // aspect too, for the next boot: Aspect() keeps the boot value
    }
}

void SetEnvDefault(const char* name, const std::string& v);

void Init(State& st) {
    if (st.ready) return;
    st.ready = true;
    bool had = ReadFile(st.file);
    st.cur = st.file;
    for (size_t i = 0; i < std::size(kEntries); i++) {
        const Entry& e = kEntries[i];
        const char* v = e.env ? std::getenv(e.env) : nullptr;
        if (v && *v && Parse(st.cur, e, v)) st.locked[i] = true;
    }
    if (const char* sz = std::getenv("RECOMP_WINDOW_SIZE")) {
        unsigned w = 0, h = 0;
        if (std::sscanf(sz, "%ux%u", &w, &h) == 2) {
            st.cur.width = w;
            st.cur.height = h;
            st.size_locked = true;
        }
    }
    st.boot_aspect = st.cur.aspect;
    // the game's starting language (hle_frontend.c's sub_0002A790 puts it in place of the console's language as the title starts)
    SetEnvDefault("RECOMP_LANGUAGE", kLanguageNames[std::min(st.cur.language, kLanguages - 1)]);
    if (!had) WriteFile(st.file);
    std::error_code ec;
    st.stamp = std::filesystem::last_write_time(FilePath(), ec);
    LOGI("settings: %s%s", FilePath().string().c_str(), had ? "" : " (new, defaults)");
}

void SetEnvDefault(const char* name, const std::string& v) {
    _putenv_s(name, v.c_str());
}

// the size the window opens at: 0 0 is the desktop in borderless / fullscreen, half of it windowed
void WindowSize(const Settings& s, uint32_t& w, uint32_t& h) {
    w = s.width;
    h = s.height;
    if (w && h) return;
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
    if (s.display_mode == DisplayMode::Windowed) {
        w = w * 3 / 4;
        h = h * 3 / 4;
    }
}

}  // namespace

Settings& Get() {
    State& st = St();
    if (!st.ready) {
        std::lock_guard lock(st.lock);
        Init(st);
    }
    return st.cur;
}

void Save() {
    State& st = St();
    std::lock_guard lock(st.lock);
    Init(st);
    // a setting the environment holds keeps the file's own value
    Settings out = st.file;
    for (size_t i = 0; i < std::size(kEntries); i++) {
        const Entry& e = kEntries[i];
        if (st.locked[i] || (st.size_locked && (!std::strcmp(e.key, "width") || !std::strcmp(e.key, "height")))) continue;
        if (e.kind == Kind::Bool) B(out, e) = B(st.cur, e);
        else U(out, e) = U(st.cur, e);
    }
    st.file = out;
    WriteFile(out);
    std::error_code ec;
    st.stamp = std::filesystem::last_write_time(FilePath(), ec);
}

void Poll() {
    State& st = St();
    ULONGLONG now = GetTickCount64();
    if (now - st.checked < 500) return;
    st.checked = now;
    std::error_code ec;
    auto t = std::filesystem::last_write_time(FilePath(), ec);
    if (ec || t == st.stamp) return;
    bool display;
    {
        std::lock_guard lock(st.lock);
        st.stamp = t;
        Settings before = st.cur;
        if (!ReadFile(st.file)) return;
        Merge(st);
        display = before.display_mode != st.cur.display_mode || before.width != st.cur.width ||
                  before.height != st.cur.height;
    }
    LOGI("settings: hpps.toml changed, applied");
    if (display) ApplyDisplay();
}

void ApplyDisplay() {
    const Settings& s = Get();
    uint32_t w, h;
    WindowSize(s, w, h);
    if (s.display_mode == DisplayMode::Fullscreen && !(s.width && s.height)) w = h = 0;  // the desktop mode
    xbox_FramebufferWindowSetDisplay(int(s.display_mode), int(w), int(h));
}

void* Window() {
    // the window thread opens it. The XDK's device init used to start that thread
    // (via the NV2A/AvSetDisplayMode paths); CreateDevice is HLE now, so start it here.
    xbox_FramebufferWindowStart();
    for (int i = 0; i < 500 && !xbox_FramebufferWindowHwnd(); i++) Sleep(10);
    return xbox_FramebufferWindowHwnd();
}

float RenderScale(uint32_t guest_height) {
    const Settings& s = Get();
    float lines;
    if (s.internal_height) {
        lines = float(s.internal_height);
    } else {
        RECT rc{};
        GetClientRect(static_cast<HWND>(Window()), &rc);
        uint32_t h = std::max<LONG>(rc.bottom - rc.top, 1);
        lines = float(h) * s.render_percent / 100.0f;
        // a maximised window on a 4K screen would otherwise render ~2100 lines; past the cap the frame is upscaled
        if (s.render_max_height) lines = std::min(lines, float(s.render_max_height));
    }
    return std::max(1.0f, lines / float(std::max<uint32_t>(guest_height, 1)));
}


AspectMode BootAspect() {
    Get();
    return St().boot_aspect;
}

// xboxrecomp: chosen on the start menu before the title has asked anything that depends on it (its widescreen check
// reads RECOMP_ASPECT once, later)
void SetBootAspect(AspectMode a) {
    Get();
    St().boot_aspect = a;
    SetEnvDefault("RECOMP_ASPECT", kAspectNames[uint32_t(a)]);
}

float Aspect() {
    AspectMode a = BootAspect();
    // 16:9 is the title's widescreen camera (hle_video.c): exactly that, or a window that is not 16:9 distorts it
    if (a == AspectMode::Wide) return 16.0f / 9.0f;
    if (a == AspectMode::Stretch) {
        RECT rc{};
        GetClientRect(static_cast<HWND>(Window()), &rc);
        float w = float(std::max<LONG>(rc.right - rc.left, 1)), h = float(std::max<LONG>(rc.bottom - rc.top, 1));
        return std::max(4.0f / 3.0f, w / h);
    }
    return 4.0f / 3.0f;
}

std::filesystem::path ExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

}  // namespace app

// hle_dsound.c, when the mixer starts: output channels
extern "C" int hpps_BeanNames(void) { return app::Get().bean_names; }
extern "C" uint32_t hpps_FovPercent(void) { return std::clamp<uint32_t>(app::Get().fov, 80, 130); }
extern "C" int app_Speakers(void) { return app::Get().speakers == 6 ? 6 : 2; }

// main.c, first thing: the settings read, and handed to the C side (the window, the title's widescreen camera, the
// pushbuffer path) as the RECOMP_* variables it reads, before any thread starts
extern "C" void hpps_SettingsInit(void) {
    const app::Settings& s = app::Get();
    uint32_t w, h;
    app::WindowSize(s, w, h);
    app::SetEnvDefault("RECOMP_WINDOW_SIZE", std::to_string(w) + "x" + std::to_string(h));
    app::SetEnvDefault("RECOMP_DISPLAY_MODE", app::kDisplayNames[uint32_t(s.display_mode)]);
    app::SetEnvDefault("RECOMP_ASPECT", app::kAspectNames[uint32_t(s.aspect)]);
    // the extra traces ([SPEECH] and the like) read this; only "1" is passed: "0" would also stop the kernel's log
    // (kernel_thunks.c), which is written every run now, as CoS's is
    if (s.debug_logs) app::SetEnvDefault("RECOMP_DEBUG_LOGS", "1");
}

// hle_video.c: the gameplay cameras' field of view
