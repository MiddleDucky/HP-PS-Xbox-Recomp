// The options panel on F10, after SCDA-XBERecomp's (src/app/menu.cpp): its graphics settings on one ImGui page,
// applied at once and saved to hpps.toml. Alt+Enter flips windowed and borderless. The fps counter draws here too.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstring>
#include <windows.h>
#include "edition.h"
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/log.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
extern "C" void hpps_MarkState(int mark);  // hle_trainer.c: the game state (Harry, level, pad) into the run log
extern "C" void xbox_FramebufferWindowSetMsgHook(int (*fn)(void*, unsigned, uintptr_t, intptr_t));

namespace app {

namespace {

std::atomic<bool> g_open{false};
std::atomic<bool> g_start{false};  // xboxrecomp: the start menu is up, the title waits for it
std::atomic<bool> g_ui{false};  // ImGui is set up

constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"Game's", "2x", "4x", "8x", "16x"};
constexpr uint32_t kMsaaValues[] = {0, 2, 4, 8};
const char* const kMsaaNames[] = {"Off", "2x", "4x", "8x"};
const char* const kDisplayNames[] = {"Windowed", "Borderless", "Fullscreen"};
const char* const kAspectNames[] = {"4:3", "16:9", "Stretch"};

template <size_t N>
int IndexOf(const uint32_t (&values)[N], uint32_t v) {
    for (size_t i = 0; i < N; i++) {
        if (values[i] == v) return int(i);
    }
    return 0;
}

// the monitor's biggest size. EnumDisplaySettings lists the real modes; GetSystemMetrics is scaled by Windows' DPI
// setting for a program that is not DPI-aware (a 4K screen at 150% reads 2560 x 1440 there), so it only stands in
std::pair<uint32_t, uint32_t> ScreenMax() {
    static const std::pair<uint32_t, uint32_t> max = [] {
        uint32_t w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
        DEVMODEA dm{};
        dm.dmSize = sizeof(dm);
        auto take = [&] {
            if (uint64_t(dm.dmPelsWidth) * dm.dmPelsHeight > uint64_t(w) * h) {
                w = dm.dmPelsWidth;
                h = dm.dmPelsHeight;
            }
        };
        for (DWORD i = 0; EnumDisplaySettingsA(nullptr, i, &dm); i++) take();
        if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) take();
        return std::pair<uint32_t, uint32_t>{w, h};
    }();
    return max;
}

// internal resolution: the lines the 3D is drawn at, 0 follows the window. Steps of the Xbox's 480 up to the screen's
// height, and that height itself; a saved value off the list (set in the toml) stays on it
struct InternalChoice {
    uint32_t lines;
    std::string name;
};

std::vector<InternalChoice> InternalChoices(uint32_t screen_h, uint32_t cur) {
    static const uint32_t kSteps[] = {480, 720, 960, 1080, 1440, 1920, 2160, 2880, 3240, 3840, 4320};
    std::vector<uint32_t> v;
    for (uint32_t l : kSteps)
        if (l <= screen_h) v.push_back(l);
    if (screen_h >= 480) v.push_back(screen_h);
    if (cur) v.push_back(cur);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    std::vector<InternalChoice> out{{0, "Follow window"}};
    for (uint32_t l : v) {
        uint32_t x = (l * 100 + 240) / 480;  // the scale over the Xbox's lines, in hundredths
        std::string scale = std::to_string(x / 100);
        if (x % 100) scale += "." + std::to_string(x % 100 / 10) + (x % 10 ? std::to_string(x % 10) : "");
        std::string name = std::to_string(l) + " (" + (l == 480 ? "Xbox, " : "") + scale + "x";
        if (l == screen_h) name += ", screen";
        out.push_back({l, name + ")"});
    }
    return out;
}

// the primary monitor's modes, biggest first, each size once
const std::vector<std::pair<uint32_t, uint32_t>>& Modes() {
    static std::vector<std::pair<uint32_t, uint32_t>> modes = [] {
        std::vector<std::pair<uint32_t, uint32_t>> m;
        DEVMODEA dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsA(nullptr, i, &dm); i++) {
            if (dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480) m.emplace_back(dm.dmPelsWidth, dm.dmPelsHeight);
        }
        for (auto s : {std::pair<uint32_t, uint32_t>{1280, 960}, {1440, 1080}, {1920, 1080}, {2560, 1440}})
            m.push_back(s);
        std::sort(m.begin(), m.end(), [](auto a, auto b) { return a.first * a.second > b.first * b.second; });
        m.erase(std::unique(m.begin(), m.end()), m.end());
        return m;
    }();
    return modes;
}

// xboxrecomp: the sizes for an aspect ratio, biggest first: the monitor's own modes of that shape and the common ones
// that fit the desktop; stretch takes every mode
bool Shaped(uint32_t w, uint32_t h, AspectMode a) {
    if (a == AspectMode::Stretch) return true;
    float r = float(w) / float(h), want = a == AspectMode::Wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    return r > want - 0.02f && r < want + 0.02f;
}

// the biggest size of a shape inside w x h, even on both sides (a 3440 x 1440 screen holds 2560 x 1440 at 16:9)
std::pair<uint32_t, uint32_t> FitShape(uint32_t w, uint32_t h, AspectMode a) {
    if (a == AspectMode::Stretch) return {w, h};
    uint32_t rw = a == AspectMode::Wide ? 16 : 4, rh = a == AspectMode::Wide ? 9 : 3, k = std::min(w / rw, h / rh) & ~1u;
    return {k * rw, k * rh};
}

// up to the screen's biggest mode (ScreenMax): the screen's own size on top when it has another shape, the picture
// keeping its shape in it
std::vector<std::pair<uint32_t, uint32_t>> ModesFor(AspectMode a) {
    static const std::pair<uint32_t, uint32_t> kCommon[] = {
        {640, 480},   {800, 600},   {1024, 768},  {1280, 960},  {1440, 1080}, {1600, 1200}, {1920, 1440},
        {2048, 1536}, {2880, 2160}, {1280, 720},  {1600, 900},  {1920, 1080}, {2560, 1440}, {3200, 1800},
        {3840, 2160}, {5120, 2880}, {7680, 4320}, {3840, 2880}, {5760, 4320}};
    auto [sw, sh] = ScreenMax();
    std::vector<std::pair<uint32_t, uint32_t>> m;
    for (auto [w, h] : Modes())
        if (Shaped(w, h, a) && w <= sw && h <= sh) m.emplace_back(w, h);
    auto fit = FitShape(sw, sh, a);
    if (fit.first >= 640) m.push_back(fit);
    for (auto [w, h] : kCommon)
        if (Shaped(w, h, a) && w <= sw && h <= sh) m.emplace_back(w, h);
    std::sort(m.begin(), m.end(), [](auto x, auto y) { return x.first * x.second > y.first * y.second; });
    m.erase(std::unique(m.begin(), m.end()), m.end());
    if (!Shaped(sw, sh, a)) m.insert(m.begin(), {sw, sh});
    return m;
}

// a setting changed: applied by the renderer next frame, kept in hpps.toml
void Changed(bool display = false) {
    Save();
    if (display) ApplyDisplay();
}

bool Combo(const char* label, int* cur, const char* const* names, int n) {
    return ImGui::Combo(label, cur, names, n);
}

void Row(const char* text) {
    ImGui::Spacing();
    ImGui::SeparatorText(text);
}

// xboxrecomp: the Controls tab: keyboard and mouse bindings (click, then press a key or a mouse button; Escape clears),
// the controller's remapping, sticks, camera and vibration
void DrawControls(Settings& s) {
    ImGuiIO& io = ImGui::GetIO();
    uint32_t vk;
    if (int b = KeyCaptureBinding(); b >= 0 && TakeCapturedKey(&vk)) {
        s.keys[b] = vk;
        Changed();
    }
    Row("Keyboard and mouse");
    if (ImGui::Checkbox("Keyboard and mouse play as controller 1", &s.kbm)) Changed();
    // xboxrecomp: the HUD's, the inventory's and the menus' button pictures as the bound keys (d3d12.cpp)
    static const char* const kPromptNames[] = {"Automatic", "Controller buttons", "Keyboard keys"};
    int bp = int(std::min(s.button_prompts, 2u));
    if (Combo("Button prompts", &bp, kPromptNames, 3)) {
        s.button_prompts = uint32_t(bp);
        Changed();
    }
    ImGui::TextDisabled("Automatic switches as soon as you use the other device.");
    int ms = int(s.mouse_sensitivity);
    if (ImGui::SliderInt("Mouse sensitivity", &ms, 1, 100)) s.mouse_sensitivity = uint32_t(ms);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    // xboxrecomp: the movement keys' pace; the mouse wheel steps it in game (as in HP:CoS)
    int mv = int(std::min<uint32_t>(s.move_mode, kMoveModes - 1));
    if (ImGui::Combo("Movement keys (mouse wheel)", &mv, kMoveModeNames, int(kMoveModes))) {
        s.move_mode = uint32_t(mv);
        Changed();
    }
    ImGui::TextDisabled("Mouse wheel in game:  down  Sneak  <  Walk  <  Run  up");
    int ps = int(s.pace_sneak), pw = int(s.pace_walk);
    if (ImGui::SliderInt("Sneak speed %", &ps, 5, 100)) s.pace_sneak = uint32_t(ps);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::SliderInt("Walk speed %", &pw, 5, 100)) s.pace_walk = uint32_t(pw);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::Checkbox("Invert mouse Y", &s.mouse_invert_y)) Changed();
    ImGui::BeginDisabled(!s.kbm);
    if (ImGui::TreeNode("Key bindings")) {
        if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            for (uint32_t i = 0; i < kKeyBinds; i++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kKeyBindNames[i]);
                ImGui::TableNextColumn();
                ImGui::PushID(int(i));
                bool waiting = KeyCaptureBinding() == int(i);
                std::string label = waiting ? "Press a key or mouse button (Esc clears)" : KeyName(s.keys[i]);
                if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) {
                    if (waiting) CancelKeyCapture();
                    else StartKeyCapture(int(i));
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Default keys")) {
            Settings d;
            std::copy(std::begin(d.keys), std::end(d.keys), std::begin(s.keys));
            Changed();
        }
        ImGui::TreePop();
    }
    ImGui::EndDisabled();

    Row("Controller");
    if (ImGui::Checkbox("Use controller", &s.controller)) Changed();
    if (!s.controller) ImGui::TextDisabled("The game ignores the controller; keyboard and mouse still play.");
    int dl = int(s.deadzone_left), dr = int(s.deadzone_right), sl = int(s.sensitivity_left), sr = int(s.sensitivity_right);
    if (ImGui::SliderInt("Left stick deadzone %", &dl, 0, 50)) s.deadzone_left = uint32_t(dl);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::SliderInt("Left stick sensitivity %", &sl, 25, 200)) s.sensitivity_left = uint32_t(sl);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::SliderInt("Right stick deadzone %", &dr, 0, 50)) s.deadzone_right = uint32_t(dr);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::SliderInt("Right stick sensitivity %", &sr, 25, 200)) s.sensitivity_right = uint32_t(sr);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::Checkbox("Invert camera X", &s.invert_x)) Changed();
    ImGui::SameLine();
    if (ImGui::Checkbox("Invert camera Y", &s.invert_y)) Changed();
    {   // xboxrecomp: a pad button for the sneak toggle (keyboard: the "Sneak (toggle)" binding above)
        const char* names[kPadInputs + 1];
        names[0] = "None";
        for (uint32_t i = 0; i < kPadInputs; i++) names[i + 1] = kPadInputNames[i];
        int sn = s.sneak_pad >= kPadInputs ? 0 : int(s.sneak_pad) + 1;
        if (ImGui::Combo("Sneak toggle button", &sn, names, int(kPadInputs + 1))) {
            s.sneak_pad = sn == 0 ? kPadInputs : uint32_t(sn - 1);
            Changed();
        }
        ImGui::TextDisabled("Harry sneaks with a gentle push of the stick; the toggle holds that pace.");
    }
    if (ImGui::Checkbox("Vibration", &s.vibration)) Changed();
    ImGui::BeginDisabled(!s.vibration);
    int vs = int(s.vibration_strength);
    if (ImGui::SliderInt("Vibration strength %", &vs, 0, 100)) s.vibration_strength = uint32_t(vs);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    ImGui::EndDisabled();
    if (ImGui::TreeNode("Button remapping")) {
        ImGui::TextDisabled("The pad button each game button reads (a 360/One pad's LB is White, RB Black).");
        ImGui::PushItemWidth(160 * io.FontGlobalScale);
        for (uint32_t i = 0; i < kPadInputs; i++) {
            int m = int(std::min(s.pad_map[i], kPadInputs - 1));
            std::string label = std::string("Game ") + kPadInputNames[i];
            if (ImGui::Combo(label.c_str(), &m, kPadInputNames, int(kPadInputs))) {
                s.pad_map[i] = uint32_t(m);
                Changed();
            }
        }
        ImGui::PopItemWidth();
        if (ImGui::Button("Default buttons")) {
            for (uint32_t i = 0; i < kPadInputs; i++) s.pad_map[i] = i;
            Changed();
        }
        ImGui::TreePop();
    }
}

// xboxrecomp: F10's Defaults resets the page it is on, not every setting (the aspect ratio, a boot setting, stays)
static void ResetPage(Settings& s, const char* page) {
    const Settings d;
    if (!std::strcmp(page, "Graphics")) {
        std::memcpy(&s.aniso, &d.aniso, sizeof(s.aniso));
        std::memcpy(&s.brightness, &d.brightness, sizeof(s.brightness));
        std::memcpy(&s.bloom, &d.bloom, sizeof(s.bloom));
        std::memcpy(&s.bloom_strength, &d.bloom_strength, sizeof(s.bloom_strength));
        std::memcpy(&s.sharpen, &d.sharpen, sizeof(s.sharpen));
        std::memcpy(&s.contrast, &d.contrast, sizeof(s.contrast));
        std::memcpy(&s.display_mode, &d.display_mode, sizeof(s.display_mode));
        std::memcpy(&s.fps_counter, &d.fps_counter, sizeof(s.fps_counter));
        std::memcpy(&s.fov, &d.fov, sizeof(s.fov));
        std::memcpy(&s.fps_limit, &d.fps_limit, sizeof(s.fps_limit));
        std::memcpy(&s.height, &d.height, sizeof(s.height));
        std::memcpy(&s.internal_height, &d.internal_height, sizeof(s.internal_height));
        std::memcpy(&s.msaa, &d.msaa, sizeof(s.msaa));
        std::memcpy(&s.render_max_height, &d.render_max_height, sizeof(s.render_max_height));
        std::memcpy(&s.render_percent, &d.render_percent, sizeof(s.render_percent));
        std::memcpy(&s.smaa, &d.smaa, sizeof(s.smaa));
        std::memcpy(&s.vsync, &d.vsync, sizeof(s.vsync));
        std::memcpy(&s.width, &d.width, sizeof(s.width));
    }
    if (!std::strcmp(page, "Audio")) {
        std::memcpy(&s.speakers, &d.speakers, sizeof(s.speakers));
    }
    if (!std::strcmp(page, "Controls")) {
        std::memcpy(&s.controller, &d.controller, sizeof(s.controller));
        std::memcpy(&s.deadzone_left, &d.deadzone_left, sizeof(s.deadzone_left));
        std::memcpy(&s.deadzone_right, &d.deadzone_right, sizeof(s.deadzone_right));
        std::memcpy(&s.invert_x, &d.invert_x, sizeof(s.invert_x));
        std::memcpy(&s.invert_y, &d.invert_y, sizeof(s.invert_y));
        std::memcpy(&s.kbm, &d.kbm, sizeof(s.kbm));
        std::memcpy(&s.button_prompts, &d.button_prompts, sizeof(s.button_prompts));
        std::memcpy(&s.keys, &d.keys, sizeof(s.keys));
        std::memcpy(&s.mouse_invert_y, &d.mouse_invert_y, sizeof(s.mouse_invert_y));
        std::memcpy(&s.mouse_sensitivity, &d.mouse_sensitivity, sizeof(s.mouse_sensitivity));
        std::memcpy(&s.move_mode, &d.move_mode, sizeof(s.move_mode));
        std::memcpy(&s.pace_sneak, &d.pace_sneak, sizeof(s.pace_sneak));
        std::memcpy(&s.pace_walk, &d.pace_walk, sizeof(s.pace_walk));
        std::memcpy(&s.pad_map, &d.pad_map, sizeof(s.pad_map));
        std::memcpy(&s.sensitivity_left, &d.sensitivity_left, sizeof(s.sensitivity_left));
        std::memcpy(&s.sensitivity_right, &d.sensitivity_right, sizeof(s.sensitivity_right));
        std::memcpy(&s.sneak_pad, &d.sneak_pad, sizeof(s.sneak_pad));
        std::memcpy(&s.vibration, &d.vibration, sizeof(s.vibration));
        std::memcpy(&s.vibration_strength, &d.vibration_strength, sizeof(s.vibration_strength));
    }
    if (!std::strcmp(page, "Accessibility")) {
        std::memcpy(&s.colorblind, &d.colorblind, sizeof(s.colorblind));
        std::memcpy(&s.colorblind_strength, &d.colorblind_strength, sizeof(s.colorblind_strength));
    }
    if (!std::strcmp(page, "Enhancements")) {
        std::memcpy(&s.cutscene_bars, &d.cutscene_bars, sizeof(s.cutscene_bars));
        std::memcpy(&s.bean_names, &d.bean_names, sizeof(s.bean_names));
        std::memcpy(&s.free_camera, &d.free_camera, sizeof(s.free_camera));
        std::memcpy(&s.skip_intro, &d.skip_intro, sizeof(s.skip_intro));
    }
    if (!std::strcmp(page, "Debug")) {
        std::memcpy(&s.debug_logs, &d.debug_logs, sizeof(s.debug_logs));
    }
}

// start: the start menu (the aspect ratio applies at once, Start game instead of Close); otherwise F10's panel
void DrawPanel(bool start) {
    Settings& s = Get();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520 * io.FontGlobalScale, 0), ImGuiCond_Appearing);
    bool open = true;
    // the edition in the title (hpps_pal.dll / hpps_ntsc.dll); ### keeps the window's id whatever it says
    const char* title = start ? HPPS_GAME_TITLE " (" HPPS_EDITION_NAME ") - Settings###start"
                              : "Options (F10) - " HPPS_GAME_TITLE ", " HPPS_EDITION_NAME "###options";
    if (!ImGui::Begin(title, start ? nullptr : &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        if (!open) g_open = false;
        return;
    }
    ImGui::PushItemWidth(260 * io.FontGlobalScale);

    bool tabs = ImGui::BeginTabBar("##tabs");
    static const char* page = "Graphics";  // the open page, for Defaults
    if (tabs && ImGui::BeginTabItem("Graphics")) {
        page = "Graphics";
    // xboxrecomp: the launcher's page before the game, on a tab as well as in the bar below (2026-11-15: a player who
    // had turned it off found no way back but holding Shift)
    Row("Startup");
    if (ImGui::Checkbox("Show the launcher at startup##graphics", &s.show_launcher)) Changed();
    ImGui::SameLine();
    ImGui::TextDisabled("its settings page before the game starts (or hold Shift as the launcher starts)");
    Row("Display");
    // the aspect ratio first: the sizes below follow it
    int asp = int(s.aspect);
    if (Combo("Aspect ratio", &asp, kAspectNames, 3)) {
        s.aspect = AspectMode(asp);
        // a window or mode of the old shape: the biggest of the new one
        if (s.width && s.height && !Shaped(s.width, s.height, s.aspect)) {
            auto m = ModesFor(s.aspect);
            if (!m.empty()) {
                s.width = m.front().first;
                s.height = m.front().second;
            }
        }
        if (start) SetBootAspect(s.aspect);
        Changed(true);
    }
    if (!start && s.aspect != BootAspect())
        ImGui::TextDisabled("Aspect ratio applies at the next start.");
    int dm = int(s.display_mode);
    if (Combo("Display mode", &dm, kDisplayNames, 3)) {
        s.display_mode = DisplayMode(dm);
        Changed(true);
    }
    {
        const auto modes = ModesFor(s.aspect);
        std::string cur = s.width && s.height ? std::to_string(s.width) + " x " + std::to_string(s.height) : "Desktop";
        if (ImGui::BeginCombo(s.display_mode == DisplayMode::Windowed ? "Window size" : "Resolution", cur.c_str())) {
            if (ImGui::Selectable("Desktop", !(s.width && s.height))) {
                s.width = s.height = 0;
                Changed(true);
            }
            for (auto [w, h] : modes) {
                std::string name = std::to_string(w) + " x " + std::to_string(h);
                if (std::pair{w, h} == ScreenMax()) name += "  (screen)";
                if (ImGui::Selectable(name.c_str(), w == s.width && h == s.height)) {
                    s.width = w;
                    s.height = h;
                    Changed(true);
                }
            }
            ImGui::EndCombo();
        }
        if (s.display_mode == DisplayMode::Borderless)
            ImGui::TextDisabled("Borderless covers the monitor; the size is for windowed / fullscreen.");
    }
    if (ImGui::Checkbox("Vsync", &s.vsync)) Changed();
    int fl = s.fps_limit == 30 ? 1 : s.fps_limit == kFpsUnlocked ? 2 : 0;
    const char* const kFpsNames[] = {"Game's rate (60 fps)", "30 fps", "Unlocked (experimental)"};
    if (Combo("Frame rate", &fl, kFpsNames, 3)) {
        s.fps_limit = fl == 1 ? 30 : fl == 2 ? kFpsUnlocked : 0;
        Changed();
    }
    if (s.fps_limit == kFpsUnlocked)
        ImGui::TextDisabled("Runs the game faster than the Xbox did; some timing may misbehave. Vsync caps it at the monitor.");

    Row("Resolution");
    {
        auto choices = InternalChoices(ScreenMax().second, s.internal_height);
        const InternalChoice* cur = &choices.front();
        for (const auto& c : choices)
            if (c.lines == s.internal_height) cur = &c;
        if (ImGui::BeginCombo("Internal resolution", cur->name.c_str())) {
            for (const auto& c : choices)
                if (ImGui::Selectable(c.name.c_str(), &c == cur)) {
                    s.internal_height = c.lines;
                    Changed();
                }
            ImGui::EndCombo();
        }
    }
    if (!s.internal_height) {
        int pct = int(s.render_percent);
        if (ImGui::SliderInt("Render scale %", &pct, 25, 200)) s.render_percent = uint32_t(pct);
        if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
        int cap = int(s.render_max_height);
        if (ImGui::SliderInt("Max render height", &cap, 0, 4320, cap ? "%d lines" : "No limit")) s.render_max_height = uint32_t(cap);
        if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    }

    Row("Anti-aliasing and filtering");
    int ms = IndexOf(kMsaaValues, s.msaa);
    if (Combo("MSAA", &ms, kMsaaNames, int(std::size(kMsaaNames)))) {
        s.msaa = kMsaaValues[ms];
        Changed();
    }
    if (ImGui::Checkbox("SMAA", &s.smaa)) Changed();
    int an = IndexOf(kAnisoValues, s.aniso);
    if (Combo("Anisotropic filtering", &an, kAnisoNames, int(std::size(kAnisoNames)))) {
        s.aniso = kAnisoValues[an];
        Changed();
    }

    Row("Picture");
    int br = int(s.brightness), co = int(s.contrast);
    if (ImGui::SliderInt("Brightness", &br, 0, 100)) s.brightness = uint32_t(br);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::SliderInt("Contrast", &co, 0, 100)) s.contrast = uint32_t(co);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::Checkbox("FPS counter", &s.fps_counter)) Changed();
    int fv = int(std::clamp(s.fov, 80u, 130u));
    if (ImGui::SliderInt("Field of view (experimental)", &fv, 80, 130, fv == 100 ? "as the game" : "%d %%")) s.fov = uint32_t(fv);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    // xboxrecomp: the game's camera logic reads the widened view back (hle_video.c); above 80 % in 16:9 a camera
    // could hold one place while Harry ran on (day3diffindo, 2026-11-02)
    ImGui::TextDisabled("80 %% recommended. A wider view can make the camera stick at some places; lower it if it does.");
    // xboxrecomp (from hpcos): post-processing, off by default
    Row("Post-processing");
    if (ImGui::Checkbox("Bloom", &s.bloom)) Changed();
    ImGui::SameLine();
    ImGui::TextDisabled("a glow round the 3D scene's bright parts, under the HUD");
    if (!s.bloom) ImGui::BeginDisabled();
    int bs = int(s.bloom_strength);
    if (ImGui::SliderInt("Bloom strength", &bs, 0, 100)) s.bloom_strength = uint32_t(bs);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (!s.bloom) ImGui::EndDisabled();
    int sh = int(s.sharpen);
    if (ImGui::SliderInt("Sharpening", &sh, 0, 100, sh == 0 ? "off" : "%d")) s.sharpen = uint32_t(sh);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
        ImGui::EndTabItem();
    }
    if (tabs && ImGui::BeginTabItem("Audio")) {
        page = "Audio";
        Row("Output");
        // xboxrecomp: the game is stereo on the console (front left / right only); 5.1 upmixes it as a Pro Logic
        // receiver does (hle_dsound.c upmix), for analog 5.1 speakers with no receiver in between
        int sp = s.speakers == 6 ? 1 : 0;
        const char* const kSpeakerNames[] = {"Stereo", "5.1 surround"};
        if (Combo("Speakers", &sp, kSpeakerNames, 2)) {
            s.speakers = sp ? 6 : 2;
            Changed();
        }
        ImGui::TextDisabled("Takes effect the next time the game starts.");
        ImGui::TextDisabled("The game is stereo on the console too: 5.1 spreads it over six speakers");
        ImGui::TextDisabled("as a Pro Logic receiver does. Needs Windows' speaker setup set to 5.1.");
        ImGui::TextDisabled("With a receiver that upmixes by itself (S/PDIF, HDMI), Stereo is enough.");
        ImGui::EndTabItem();
    }
    if (tabs && ImGui::BeginTabItem("Controls")) {
        page = "Controls";
        DrawControls(s);
        ImGui::EndTabItem();
    }
    if (tabs && ImGui::BeginTabItem("Accessibility")) {
        page = "Accessibility";
        Row("Colour blindness");
        int cb = int(std::min(s.colorblind, 3u));
        const char* const kCbNames[] = {"Off", "Protanopia (red-weak)", "Deuteranopia (green-weak)",
                                        "Tritanopia (blue-weak)"};
        if (Combo("Correction filter", &cb, kCbNames, 4)) {
            s.colorblind = uint32_t(cb);
            Changed();
        }
        ImGui::BeginDisabled(!s.colorblind);
        int st = int(s.colorblind_strength);
        if (ImGui::SliderInt("Strength %", &st, 0, 100)) s.colorblind_strength = uint32_t(st);
        if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
        ImGui::EndDisabled();
        ImGui::TextDisabled("Shifts the colours a colour-blind player confuses toward ones they can tell apart.");
        ImGui::EndTabItem();
    }
    if (tabs && ImGui::BeginTabItem("Enhancements")) {
        // xboxrecomp: changes to how the game plays that go beyond the console's (moved from Accessibility, 2026-10-30)
        page = "Enhancements";
        Row("Bertie Bott's beans");
        if (ImGui::Checkbox("Announce bean flavours on pickup", &s.bean_names)) Changed();
        ImGui::TextDisabled("Off: the flavour is no longer said aloud when Harry picks up a bean.");
        Row("Camera");
        if (ImGui::Checkbox("Free camera", &s.free_camera)) Changed();
        ImGui::TextDisabled("The right stick's or mouse's up and down tilt the camera up and down round Harry.");
        ImGui::TextDisabled("Off: they move the camera nearer and further, as on the console.");
        // xboxrecomp: moved from Graphics (2026-11-10), worded as the PoA and CoS ports' (the setting is still
        // cutscene_bars, true keeping the bars, so hpps.toml reads as before)
        Row("Cutscenes");
        bool no_bars = !s.cutscene_bars;
        if (ImGui::Checkbox("Cutscenes without black bars", &no_bars)) s.cutscene_bars = !no_bars, Changed();
        ImGui::TextDisabled("In 16:9 cutscenes fill the screen with no bars at the top and bottom (subtitles over the");
        ImGui::TextDisabled("picture). Off: the console's bars over the picture.");
        Row("Startup");
        if (ImGui::Checkbox("Skip the Warner Bros. and EA logos", &s.skip_intro)) Changed();
        ImGui::TextDisabled("The logo movies before the title screen are not played. Off: played, as on the console.");
        ImGui::EndTabItem();
    }
    // xboxrecomp: the Debug tab, as CoS has it: where this run's log is (the launcher writes one per run, always), a
    // marker line in it for "it happened here" with the game's state under it, and the extra traces
    if (tabs && ImGui::BeginTabItem("Debug")) {
        page = "Debug";
        static char log_name[MAX_PATH];
        if (!log_name[0] && !GetEnvironmentVariableA("HPPS_RUN_LOG", log_name, sizeof log_name))
            std::snprintf(log_name, sizeof log_name, "none (started without the launcher)");
        Row("Logs");
        ImGui::TextWrapped("This run's log: %s", log_name);
        if (ImGui::Button("Open logs folder")) {
            char dir[MAX_PATH];
            DWORD n = GetModuleFileNameA(nullptr, dir, sizeof dir);
            char* slash = n ? std::strrchr(dir, '\\') : nullptr;
            if (slash) {
                std::snprintf(slash + 1, size_t(dir + sizeof dir - slash - 1), "logs\\runs");
                if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) *slash = 0;  // started without the launcher
                ShellExecuteA(nullptr, "open", dir, nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        ImGui::SameLine();
        static int marks;
        if (ImGui::Button("Mark this moment in the log")) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            LOGI("[MARK] %d at %02u:%02u:%02u.%03u (marked from F10's Debug tab)", ++marks, t.wHour, t.wMinute,
                 t.wSecond, t.wMilliseconds);
            hpps_MarkState(marks);
        }
        if (marks) ImGui::TextDisabled("%d mark%s written: search the log for [MARK]", marks, marks == 1 ? "" : "s");
        ImGui::TextDisabled("Mark right after something goes wrong, then give the mark's number with your report.");
        if (ImGui::Checkbox("Extra debug traces", &s.debug_logs)) Changed();
        ImGui::TextDisabled("More detail in the log (speech, among others). Takes effect the next time the game starts.");
        ImGui::EndTabItem();
    }
    if (tabs) ImGui::EndTabBar();

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Defaults (this page)")) {
        ResetPage(s, page);
        Changed(true);
    }
    ImGui::SameLine();
    if (start) {
        if (ImGui::Checkbox("Show this page at startup", &s.start_menu)) Changed();
        ImGui::Spacing();
        if (ImGui::Button("Start game", ImVec2(160 * io.FontGlobalScale, 0))) {
            SetBootAspect(s.aspect);
            Changed();
            g_start = false;
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        ImGui::TextDisabled("Saved to hpps.toml; F10 in game changes them too");
    } else {
        if (ImGui::Button("Close")) open = false;
        ImGui::SameLine();
        if (ImGui::Checkbox("Show the launcher at startup", &s.show_launcher)) Changed();
        ImGui::SameLine();
        ImGui::TextDisabled("Saved to hpps.toml");
    }
    ImGui::PopItemWidth();
    ImGui::End();
    if (!open) g_open = false;
}

void DrawFps() {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f FPS", g_fps.load());
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float sz = ImGui::GetFontSize() * 1.25f;
    ImVec2 p(12, 8);
    dl->AddText(ImGui::GetFont(), sz, ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(ImGui::GetFont(), sz, p, IM_COL32(255, 255, 80, 255), text);
}

// the window thread's messages: the pointer hidden, F10 and Alt+Enter, then ImGui; while the panel is open it keeps the
// keyboard and mouse
int MsgHook(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp) {
    InputMessage(hwnd, msg, wp, lp);  // xboxrecomp: keyboard and mouse as controller 1 (input.cpp)
    // xboxrecomp: no mouse pointer over the game; the panel shows it again
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && !g_open && !g_start) {
        SetCursor(nullptr);
        return 1;
    }
    bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    if (down && wp == VK_F10) {
        if (!(lp & (1 << 30)) && !g_start) ToggleMenu();  // not on auto-repeat; the start menu is already up
        return 1;
    }
    if (msg == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29))) {
        if (!(lp & (1 << 30))) {
            Settings& s = Get();
            s.display_mode = s.display_mode == DisplayMode::Windowed ? DisplayMode::Borderless : DisplayMode::Windowed;
            Changed(true);
        }
        return 1;
    }
    if (!g_ui) return 0;
    std::lock_guard lock(UiLock());
    ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), msg, WPARAM(wp), LPARAM(lp));
    if (!g_open && !g_start) return 0;
    switch (msg) {
        case WM_KEYDOWN: case WM_CHAR: case WM_MOUSEWHEEL: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
            return 1;
        case WM_SYSKEYDOWN:
            return wp != VK_F4;  // Alt+F4 still closes (fb_present.c ends the process)
        default:
            return 0;
    }
}

// ── xboxrecomp: the F10 settings as rows of the game's own menus (hle_frontend.c: Options and the pause menu's
// "PC Settings", after HP:CoS's) ──────────────────────────────────────────────────────────────────────────────────
//
// Pages of rows, each a label and a value as words; left and right step the value (A steps it on), with the same
// limits and the same saving (Changed) as the panel's own controls. Key bindings, the button remapping and the Debug
// tab stay in F10. The game draws them in its menu font, so the words keep to letters, digits and ": . -" (no '%',
// brackets or '*' shown); a label ending in '*' is a setting for the next start, which the menu says under the rows.

std::string LinesName(uint32_t lines) {
    if (!lines) return "Match window";
    if (lines == 480) return "Original 480p";
    if (lines == 2160) return "4K 2160p";
    return std::to_string(lines) + "p";
}

struct GameRow {
    const char* label;
    std::string (*value)(const Settings&);
    void (*step)(Settings&, int dir);  // dir -1 or +1; changes the settings only
    bool display = false;  // applies to the window (ApplyDisplay)
    bool held = false;     // the window's size and shape: chosen here, applied as the page is left (app_GameOptLeave)
};

std::string OnOff(bool b) { return b ? "On" : "Off"; }

uint32_t Stepped(uint32_t v, int dir, uint32_t lo, uint32_t hi, uint32_t by) {
    int64_t n = int64_t(v) + int64_t(dir) * by;
    return uint32_t(std::clamp<int64_t>(n, lo, hi));
}

int Cycled(int i, int dir, int n) { return ((i + dir) % n + n) % n; }

std::string Num(uint32_t v, const char* off = nullptr) { return v == 0 && off ? off : std::to_string(v); }

// the next of a list of values from v (a value off the list goes to its neighbour that way)
uint32_t NextOf(const uint32_t* list, size_t n, uint32_t v, int dir) {
    if (dir > 0) {
        for (size_t i = 0; i < n; i++)
            if (list[i] > v) return list[i];
        return list[n - 1];
    }
    for (size_t i = n; i-- > 0;)
        if (list[i] < v) return list[i];
    return list[0];
}

const GameRow kDisplayRows[] = {
    {"Aspect ratio *", [](const Settings& s) { return std::string(kAspectNames[std::min(uint32_t(s.aspect), 2u)]); },
     [](Settings& s, int d) {
         s.aspect = AspectMode(Cycled(int(s.aspect), d, 3));
         if (s.width && s.height && !Shaped(s.width, s.height, s.aspect)) {
             auto m = ModesFor(s.aspect);
             if (!m.empty()) s.width = m.front().first, s.height = m.front().second;
         }
     },
     true, true},
    {"Display mode", [](const Settings& s) { return std::string(kDisplayNames[std::min(uint32_t(s.display_mode), 2u)]); },
     [](Settings& s, int d) { s.display_mode = DisplayMode(Cycled(int(s.display_mode), d, 3)); }, true},
    {"Resolution",
     [](const Settings& s) {
         return s.width && s.height ? std::to_string(s.width) + " x " + std::to_string(s.height) : std::string("Desktop");
     },
     [](Settings& s, int d) {
         // Desktop, then the shape's sizes biggest first
         auto m = ModesFor(s.aspect);
         int cur = 0;
         for (size_t i = 0; i < m.size(); i++)
             if (m[i].first == s.width && m[i].second == s.height) cur = int(i) + 1;
         int n = Cycled(cur, d, int(m.size()) + 1);
         if (n == 0) s.width = s.height = 0;
         else s.width = m[n - 1].first, s.height = m[n - 1].second;
     },
     true, true},
    {"Vsync", [](const Settings& s) { return OnOff(s.vsync); }, [](Settings& s, int) { s.vsync = !s.vsync; }},
    {"Frame rate",
     [](const Settings& s) {
         return std::string(s.fps_limit == 30 ? "30 fps" : s.fps_limit == kFpsUnlocked ? "Unlocked" : "60 fps");
     },
     [](Settings& s, int d) {
         int i = s.fps_limit == 30 ? 1 : s.fps_limit == kFpsUnlocked ? 2 : 0;
         i = Cycled(i, d, 3);
         s.fps_limit = i == 1 ? 30 : i == 2 ? kFpsUnlocked : 0;
     }},
};

const GameRow kGraphicsRows[] = {
    {"Internal resolution", [](const Settings& s) { return LinesName(s.internal_height); },
     [](Settings& s, int d) {
         auto c = InternalChoices(ScreenMax().second, s.internal_height);
         int cur = 0;
         for (size_t i = 0; i < c.size(); i++)
             if (c[i].lines == s.internal_height) cur = int(i);
         s.internal_height = c[size_t(std::clamp(cur + d, 0, int(c.size()) - 1))].lines;
     }},
    {"Render scale", [](const Settings& s) { return std::to_string(s.render_percent); },
     [](Settings& s, int d) { s.render_percent = Stepped(s.render_percent, d, 25, 200, 25); }},
    {"Max render height",
     [](const Settings& s) { return s.render_max_height ? std::to_string(s.render_max_height) + "p" : std::string("No limit"); },
     [](Settings& s, int d) {
         // 0 (no limit) last: up from 4320 lifts the limit, down from it comes back to 4320
         static const uint32_t k[] = {480, 720, 960, 1080, 1440, 2160, 2880, 4320, 0xFFFFFFFFu};
         uint32_t v = s.render_max_height ? s.render_max_height : 0xFFFFFFFFu;
         v = NextOf(k, std::size(k), v, d);
         s.render_max_height = v == 0xFFFFFFFFu ? 0 : v;
     }},
    {"MSAA", [](const Settings& s) { return std::string(kMsaaNames[IndexOf(kMsaaValues, s.msaa)]); },
     [](Settings& s, int d) {
         s.msaa = kMsaaValues[std::clamp(IndexOf(kMsaaValues, s.msaa) + d, 0, int(std::size(kMsaaValues)) - 1)];
     }},
    {"SMAA", [](const Settings& s) { return OnOff(s.smaa); }, [](Settings& s, int) { s.smaa = !s.smaa; }},
    {"Anisotropic filter", [](const Settings& s) { return std::string(kAnisoNames[IndexOf(kAnisoValues, s.aniso)]); },
     [](Settings& s, int d) {
         s.aniso = kAnisoValues[std::clamp(IndexOf(kAnisoValues, s.aniso) + d, 0, int(std::size(kAnisoValues)) - 1)];
     }},
    {"Brightness", [](const Settings& s) { return std::to_string(s.brightness); },
     [](Settings& s, int d) { s.brightness = Stepped(s.brightness, d, 0, 100, 5); }},
    {"Contrast", [](const Settings& s) { return std::to_string(s.contrast); },
     [](Settings& s, int d) { s.contrast = Stepped(s.contrast, d, 0, 100, 5); }},
    {"Field of view",
     [](const Settings& s) {
         uint32_t f = std::clamp(s.fov, 80u, 130u);
         return f == 100 ? std::string("Game") : std::to_string(f);
     },
     [](Settings& s, int d) { s.fov = Stepped(std::clamp(s.fov, 80u, 130u), d, 80, 130, 5); }},
    {"FPS counter", [](const Settings& s) { return OnOff(s.fps_counter); },
     [](Settings& s, int) { s.fps_counter = !s.fps_counter; }},
};

const GameRow kPostRows[] = {
    {"Bloom", [](const Settings& s) { return OnOff(s.bloom); }, [](Settings& s, int) { s.bloom = !s.bloom; }},
    {"Bloom strength", [](const Settings& s) { return std::to_string(s.bloom_strength); },
     [](Settings& s, int d) { s.bloom_strength = Stepped(s.bloom_strength, d, 0, 100, 5); }},
    {"Sharpening", [](const Settings& s) { return Num(s.sharpen, "Off"); },
     [](Settings& s, int d) { s.sharpen = Stepped(s.sharpen, d, 0, 100, 5); }},
};

const GameRow kAudioRows[] = {
    {"Speakers *", [](const Settings& s) { return std::string(s.speakers == 6 ? "5.1 surround" : "Stereo"); },
     [](Settings& s, int) { s.speakers = s.speakers == 6 ? 2 : 6; }},
};

const GameRow kControlsRows[] = {
    {"Keyboard and mouse", [](const Settings& s) { return OnOff(s.kbm); }, [](Settings& s, int) { s.kbm = !s.kbm; }},
    {"Button prompts",
     [](const Settings& s) {
         static const char* const k[] = {"Automatic", "Controller", "Keyboard"};
         return std::string(k[std::min(s.button_prompts, 2u)]);
     },
     [](Settings& s, int d) { s.button_prompts = uint32_t(Cycled(int(std::min(s.button_prompts, 2u)), d, 3)); }},
    {"Mouse sensitivity", [](const Settings& s) { return std::to_string(s.mouse_sensitivity); },
     [](Settings& s, int d) { s.mouse_sensitivity = Stepped(s.mouse_sensitivity, d, 1, 100, 1); }},
    {"Invert mouse Y", [](const Settings& s) { return OnOff(s.mouse_invert_y); },
     [](Settings& s, int) { s.mouse_invert_y = !s.mouse_invert_y; }},
    {"Movement keys",
     [](const Settings& s) { return std::string(kMoveModeNames[std::min<uint32_t>(s.move_mode, kMoveModes - 1)]); },
     [](Settings& s, int d) {
         s.move_mode = uint32_t(Cycled(int(std::min<uint32_t>(s.move_mode, kMoveModes - 1)), d, int(kMoveModes)));
     }},
    {"Sneak speed", [](const Settings& s) { return std::to_string(s.pace_sneak); },
     [](Settings& s, int d) { s.pace_sneak = Stepped(s.pace_sneak, d, 5, 100, 1); }},
    {"Walk speed", [](const Settings& s) { return std::to_string(s.pace_walk); },
     [](Settings& s, int d) { s.pace_walk = Stepped(s.pace_walk, d, 5, 100, 1); }},
    {"Use controller", [](const Settings& s) { return OnOff(s.controller); },
     [](Settings& s, int) { s.controller = !s.controller; }},
    {"Left deadzone", [](const Settings& s) { return std::to_string(s.deadzone_left); },
     [](Settings& s, int d) { s.deadzone_left = Stepped(s.deadzone_left, d, 0, 50, 1); }},
    {"Left sensitivity", [](const Settings& s) { return std::to_string(s.sensitivity_left); },
     [](Settings& s, int d) { s.sensitivity_left = Stepped(s.sensitivity_left, d, 25, 200, 5); }},
    {"Right deadzone", [](const Settings& s) { return std::to_string(s.deadzone_right); },
     [](Settings& s, int d) { s.deadzone_right = Stepped(s.deadzone_right, d, 0, 50, 1); }},
    {"Right sensitivity", [](const Settings& s) { return std::to_string(s.sensitivity_right); },
     [](Settings& s, int d) { s.sensitivity_right = Stepped(s.sensitivity_right, d, 25, 200, 5); }},
    {"Invert camera X", [](const Settings& s) { return OnOff(s.invert_x); },
     [](Settings& s, int) { s.invert_x = !s.invert_x; }},
    {"Invert camera Y", [](const Settings& s) { return OnOff(s.invert_y); },
     [](Settings& s, int) { s.invert_y = !s.invert_y; }},
    {"Vibration", [](const Settings& s) { return OnOff(s.vibration); },
     [](Settings& s, int) { s.vibration = !s.vibration; }},
    {"Vibration strength", [](const Settings& s) { return std::to_string(s.vibration_strength); },
     [](Settings& s, int d) { s.vibration_strength = Stepped(s.vibration_strength, d, 0, 100, 5); }},
};

const GameRow kAccessRows[] = {
    {"Colour filter",
     [](const Settings& s) {
         static const char* const k[] = {"Off", "Protanopia", "Deuteranopia", "Tritanopia"};
         return std::string(k[std::min(s.colorblind, 3u)]);
     },
     [](Settings& s, int d) { s.colorblind = uint32_t(Cycled(int(std::min(s.colorblind, 3u)), d, 4)); }},
    {"Filter strength", [](const Settings& s) { return std::to_string(s.colorblind_strength); },
     [](Settings& s, int d) { s.colorblind_strength = Stepped(s.colorblind_strength, d, 0, 100, 5); }},
};

const GameRow kEnhanceRows[] = {
    {"Bean flavours", [](const Settings& s) { return OnOff(s.bean_names); },
     [](Settings& s, int) { s.bean_names = !s.bean_names; }},
    {"Free camera", [](const Settings& s) { return OnOff(s.free_camera); },
     [](Settings& s, int) { s.free_camera = !s.free_camera; }},
    {"Cutscene bars", [](const Settings& s) { return std::string(s.cutscene_bars ? "Shown" : "Hidden"); },
     [](Settings& s, int) { s.cutscene_bars = !s.cutscene_bars; }},
    {"Skip logo movies", [](const Settings& s) { return OnOff(s.skip_intro); },
     [](Settings& s, int) { s.skip_intro = !s.skip_intro; }},
};

struct GamePage {
    const char* name;
    const GameRow* rows;
    int n;
};

#define GAME_PAGE(name, rows) {name, rows, int(std::size(rows))}
const GamePage kGamePages[] = {
    GAME_PAGE("Display", kDisplayRows),        GAME_PAGE("Graphics", kGraphicsRows),
    GAME_PAGE("Post-processing", kPostRows),   GAME_PAGE("Audio", kAudioRows),
    GAME_PAGE("Controls", kControlsRows),      GAME_PAGE("Accessibility", kAccessRows),
    GAME_PAGE("Enhancements", kEnhanceRows),
};
#undef GAME_PAGE

const GamePage* PageAt(int p) { return p >= 0 && p < int(std::size(kGamePages)) ? &kGamePages[p] : nullptr; }

}  // namespace

bool MenuOpen() { return g_open || g_start; }

void ToggleMenu() { g_open = !g_open; }

void OpenStartMenu() { g_start = true; }

bool StartMenuOpen() { return g_start; }

void InitUi() {
    std::lock_guard lock(UiLock());
    if (g_ui) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
                      ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4;
    ImGui_ImplWin32_Init(Window());
    g_ui = true;
    xbox_FramebufferWindowSetMsgHook(MsgHook);
    LOGI("options: F10 opens the panel, Alt+Enter flips windowed / borderless");
}

void DrawUi() {
    InputFrame();
    ImGuiIO& io = ImGui::GetIO();
    // ImGui's 13 px font at 720 lines, bigger with the window
    io.FontGlobalScale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    io.MouseDrawCursor = false;
    if (g_start) DrawPanel(true);
    else if (g_open) DrawPanel(false);
    if (Get().fps_counter) DrawFps();
    // xboxrecomp: the keys' pace a moment after the mouse wheel steps it, so a turn of the wheel is seen to do something
    if (GetTickCount64() - g_pace_note.load() < 2000 && !SneakOn()) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float sz = ImGui::GetFontSize() * 1.25f;
        ImVec2 p(12, Get().fps_counter ? 8 + sz * 1.3f : 8);
        char text[32];
        std::snprintf(text, sizeof text, "Keys: %s", kMoveModeNames[std::min<uint32_t>(Get().move_mode, kMoveModes - 1)]);
        dl->AddText(ImGui::GetFont(), sz, ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 200), text);
        dl->AddText(ImGui::GetFont(), sz, p, IM_COL32(255, 255, 255, 255), text);
    }
    if (SneakOn()) {  // xboxrecomp: the sneak toggle is on: a small mark, so it is not left on unknowingly
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float sz = ImGui::GetFontSize() * 1.25f;
        ImVec2 p(12, Get().fps_counter ? 8 + sz * 1.3f : 8);
        dl->AddText(ImGui::GetFont(), sz, ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 200), "Sneaking");
        dl->AddText(ImGui::GetFont(), sz, p, IM_COL32(170, 200, 255, 255), "Sneaking");
    }
}

}  // namespace app

// hle_input.c: the panel has the controller while it is open
extern "C" int hpps_MenuOpen(void) { return app::MenuOpen(); }

// hle_frontend.c: PC Settings's pages. Page -1 is the list of pages; its rows are the pages' names, no values.
// The strings stay good until the next call on this thread (the game thread asks for one row at a time)
extern "C" int app_GameOptRows(int page) {
    if (page < 0) return int(std::size(app::kGamePages));
    const app::GamePage* p = app::PageAt(page);
    return p ? p->n : 0;
}

extern "C" const char* app_GameOptLabel(int page, int row) {
    if (page < 0) return app::PageAt(row) ? app::kGamePages[row].name : "";
    const app::GamePage* p = app::PageAt(page);
    return p && row >= 0 && row < p->n ? p->rows[row].label : "";
}

// the size rows' choice (aspect, width, height) waits here until the page is left: a resolution change switches the
// screen's mode or resizes the window, which each step would otherwise do
namespace {
app::Settings g_pend;
int g_pend_page = -1;
}  // namespace

extern "C" const char* app_GameOptValue(int page, int row) {
    static thread_local std::string v;
    const app::GamePage* p = app::PageAt(page);
    if (!p || row < 0 || row >= p->n) return "";
    std::lock_guard lock(app::UiLock());
    const app::GameRow& r = p->rows[row];
    v = r.value(r.held && g_pend_page == page ? g_pend : app::Get());
    return v.c_str();
}

extern "C" void app_GameOptStep(int page, int row, int dir) {
    const app::GamePage* p = app::PageAt(page);
    if (!p || row < 0 || row >= p->n) return;
    std::lock_guard lock(app::UiLock());
    const app::GameRow& r = p->rows[row];
    if (r.held) {
        if (g_pend_page != page) g_pend = app::Get(), g_pend_page = page;
        r.step(g_pend, dir < 0 ? -1 : 1);
        return;
    }
    r.step(app::Get(), dir < 0 ? -1 : 1);
    app::Changed(r.display);
}

// a page left (B, the menu closed): its size choice applied
extern "C" void app_GameOptLeave(int page) {
    std::lock_guard lock(app::UiLock());
    if (g_pend_page != page) return;
    g_pend_page = -1;
    app::Settings& s = app::Get();
    if (s.aspect == g_pend.aspect && s.width == g_pend.width && s.height == g_pend.height) return;
    s.aspect = g_pend.aspect;
    s.width = g_pend.width;
    s.height = g_pend.height;
    app::Changed(true);
}
