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
// internal resolution: render lines, 0 follows the window
constexpr uint32_t kInternalValues[] = {0, 480, 720, 960, 1080, 1440, 1920, 2160};
const char* const kInternalNames[] = {"Follow window",  "480 (Xbox, 1x)", "720 (1.5x)", "960 (2x)",
                                      "1080 (2.25x)",   "1440 (3x)",      "1920 (4x)",  "2160 (4.5x, 4K)"};
const char* const kDisplayNames[] = {"Windowed", "Borderless", "Fullscreen"};
const char* const kAspectNames[] = {"4:3", "16:9", "Stretch"};

template <size_t N>
int IndexOf(const uint32_t (&values)[N], uint32_t v) {
    for (size_t i = 0; i < N; i++) {
        if (values[i] == v) return int(i);
    }
    return 0;
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

std::vector<std::pair<uint32_t, uint32_t>> ModesFor(AspectMode a) {
    static const std::pair<uint32_t, uint32_t> kCommon[] = {
        {640, 480},   {800, 600},   {1024, 768},  {1280, 960},  {1440, 1080}, {1600, 1200}, {1920, 1440},
        {2048, 1536}, {2880, 2160}, {1280, 720},  {1600, 900},  {1920, 1080}, {2560, 1440}, {3200, 1800},
        {3840, 2160}};
    uint32_t dw = GetSystemMetrics(SM_CXSCREEN), dh = GetSystemMetrics(SM_CYSCREEN);
    std::vector<std::pair<uint32_t, uint32_t>> m;
    for (auto [w, h] : Modes())
        if (Shaped(w, h, a)) m.emplace_back(w, h);
    for (auto [w, h] : kCommon)
        if (Shaped(w, h, a) && w <= dw && h <= dh) m.emplace_back(w, h);
    std::sort(m.begin(), m.end(), [](auto x, auto y) { return x.first * x.second > y.first * y.second; });
    m.erase(std::unique(m.begin(), m.end()), m.end());
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
        std::memcpy(&s.cutscene_bars, &d.cutscene_bars, sizeof(s.cutscene_bars));
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
        std::memcpy(&s.bean_names, &d.bean_names, sizeof(s.bean_names));
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
    const char* title = start ? "Harry Potter and the Philosopher's Stone - Settings###start" : "Options (F10)";
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
    if (s.aspect == AspectMode::Wide && ImGui::Checkbox("Cutscene bars", &s.cutscene_bars)) Changed();
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
    int ir = IndexOf(kInternalValues, s.internal_height);
    if (Combo("Internal resolution", &ir, kInternalNames, int(std::size(kInternalNames)))) {
        s.internal_height = kInternalValues[ir];
        Changed();
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
        // xboxrecomp: the console mixes to 5.1 (Dolby Digital out of the console); stereo folds it down
        int sp = s.speakers == 6 ? 1 : 0;
        const char* const kSpeakerNames[] = {"Stereo", "5.1 surround"};
        if (Combo("Speakers", &sp, kSpeakerNames, 2)) {
            s.speakers = sp ? 6 : 2;
            Changed();
        }
        ImGui::TextDisabled("Takes effect the next time the game starts.");
        ImGui::TextDisabled("5.1 needs Windows' speaker setup set to 5.1 (or a 5.1 HDMI receiver).");
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
