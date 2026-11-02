// hpps_launcher.exe: picks 4:3 or 16:9 and the graphics settings, writes them to hpps.toml beside it and starts
// hpps.exe with the environment it needs (StartGame). Behind the page the title as text over a coloured backdrop (no
// game art). Dear ImGui over Direct3D 11. A disc image in game\ (game\*.iso / *.xiso, no
// game\default.xbe) is unpacked there first, with a progress page (xiso.c).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cfloat>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "../single_instance.h"
#include "gamefiles.h"
#include "font/title_font.inc"  // Magic School One (FontMesa), the title's font
#include "xiso.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// ── settings: hpps.toml, the keys the game reads (scda/app/app.cpp) ─────────────────────────────────────────────

struct Settings {
    bool wide = false;            // aspect '16:9', otherwise '4:3'
    int display = 0;              // 'windowed', 'borderless', 'fullscreen'
    uint32_t width = 1920, height = 1080;
    uint32_t internal_height = 0;
    uint32_t msaa = 0, aniso = 0;
    bool smaa = false, vsync = true;
    bool show = true;             // show_launcher: false starts the game without this page
    int fps = 0;                  // fps_limit: 0 the game's, 1 30 fps, 2 'unlocked'
    int colorblind = 0;           // 'off', 'protanopia', 'deuteranopia', 'tritanopia'
    uint32_t cb_strength = 100;
    bool kbm = true;              // keyboard_mouse
    uint32_t mouse_sens = 50;
    bool vibration = true, invert_x = false, invert_y = false;
    bool controller = true;  // the in-game F10 option of the same name
    int language = 0;        // the language the game starts in (kLanguageKeys)
};

const char* const kColorBlindKeys[] = {"off", "protanopia", "deuteranopia", "tritanopia"};
const char* const kColorBlindNames[] = {"Off", "Protanopia (red-weak)", "Deuteranopia (green-weak)", "Tritanopia (blue-weak)"};

const char* const kLanguageKeys[] = {"english", "french", "german", "spanish", "italian", "dutch", "swedish"};
const char* const kLanguageNames[] = {"English", "Fran\xC3\xA7" "ais", "Deutsch", "Espa\xC3\xB1ol", "Italiano", "Nederlands", "Svenska"};

const char* const kDisplayKeys[] = {"windowed", "borderless", "fullscreen"};
const char* const kDisplayNames[] = {"Windowed", "Borderless", "Fullscreen"};
constexpr uint32_t kInternalValues[] = {0, 480, 720, 960, 1080, 1440, 1920, 2160};
const char* const kInternalNames[] = {"Follow window",  "480 (Xbox, 1x)", "720 (1.5x)", "960 (2x)",
                                      "1080 (2.25x)",   "1440 (3x)",      "1920 (4x)",  "2160 (4.5x, 4K)"};
constexpr uint32_t kMsaaValues[] = {0, 2, 4, 8};
const char* const kMsaaNames[] = {"Off", "2x", "4x", "8x"};
constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"Game's", "2x", "4x", "8x", "16x"};

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 1) - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string Trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
}

// hpps.toml as its lines, so a save keeps the comments and every key this page does not touch
std::vector<std::string> g_lines;

std::string Value(const std::string& key) {
    for (const auto& l : g_lines) {
        std::string t = Trim(l);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos || Trim(t.substr(0, eq)) != key) continue;
        std::string v = Trim(t.substr(eq + 1));
        if (v.size() >= 2 && (v[0] == '\'' || v[0] == '"')) v = v.substr(1, v.size() - 2);
        return v;
    }
    return {};
}

void SetValue(const std::string& key, const std::string& v) {
    for (auto& l : g_lines) {
        std::string t = Trim(l);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq != std::string::npos && Trim(t.substr(0, eq)) == key) {
            l = key + " = " + v;
            return;
        }
    }
    g_lines.push_back(key + " = " + v);
}

std::string TomlPath() { return Narrow(ExeDir()) + "hpps.toml"; }

Settings Load() {
    Settings s;
    g_lines.clear();
    std::ifstream f(TomlPath());
    for (std::string l; std::getline(f, l);) g_lines.push_back(l);
    auto u = [](const std::string& v, uint32_t d) { return v.empty() ? d : uint32_t(std::strtoul(v.c_str(), nullptr, 10)); };
    s.wide = Value("aspect") == "16:9";
    std::string dm = Value("display_mode");
    for (int i = 0; i < 3; i++)
        if (dm == kDisplayKeys[i]) s.display = i;
    s.width = u(Value("width"), s.width);
    s.height = u(Value("height"), s.height);
    s.internal_height = u(Value("internal_height"), 0);
    s.msaa = u(Value("msaa"), 0);
    s.aniso = u(Value("anisotropic_filtering"), 0);
    s.smaa = Value("smaa") == "true";
    s.vsync = Value("vsync") != "false";
    s.show = Value("show_launcher") != "false";
    s.fps = Value("fps_limit") == "unlocked" ? 2 : u(Value("fps_limit"), 0) == 30 ? 1 : 0;
    std::string cb = Value("colorblind");
    for (int i = 0; i < 4; i++)
        if (cb == kColorBlindKeys[i]) s.colorblind = i;
    s.cb_strength = std::min(u(Value("colorblind_strength"), 100), 100u);
    s.kbm = Value("keyboard_mouse") != "false";
    s.mouse_sens = std::clamp(u(Value("mouse_sensitivity"), 50), 1u, 100u);
    s.vibration = Value("vibration") != "false";
    s.controller = Value("controller") != "false";
    s.invert_x = Value("invert_camera_x") == "true";
    s.invert_y = Value("invert_camera_y") == "true";
    std::string lang = Value("language");
    for (int i = 0; i < 7; i++)
        if (lang == kLanguageKeys[i]) s.language = i;
    return s;
}

void Save(const Settings& s) {
    auto q = [](const char* v) { return std::string("'") + v + "'"; };
    SetValue("display_mode", q(kDisplayKeys[s.display]));
    SetValue("width", std::to_string(s.width));
    SetValue("height", std::to_string(s.height));
    SetValue("aspect", q(s.wide ? "16:9" : "4:3"));
    SetValue("internal_height", std::to_string(s.internal_height));
    SetValue("msaa", std::to_string(s.msaa));
    SetValue("smaa", s.smaa ? "true" : "false");
    SetValue("anisotropic_filtering", std::to_string(s.aniso));
    SetValue("vsync", s.vsync ? "true" : "false");
    SetValue("show_launcher", s.show ? "true" : "false");
    SetValue("start_menu", "false");  // this page instead of the in-game one
    SetValue("fps_limit", s.fps == 2 ? "'unlocked'" : s.fps == 1 ? "30" : "0");
    SetValue("colorblind", q(kColorBlindKeys[s.colorblind]));
    SetValue("colorblind_strength", std::to_string(s.cb_strength));
    SetValue("keyboard_mouse", s.kbm ? "true" : "false");
    SetValue("mouse_sensitivity", std::to_string(s.mouse_sens));
    SetValue("vibration", s.vibration ? "true" : "false");
    SetValue("controller", s.controller ? "true" : "false");
    SetValue("invert_camera_x", s.invert_x ? "true" : "false");
    SetValue("invert_camera_y", s.invert_y ? "true" : "false");
    SetValue("language", q(kLanguageKeys[s.language]));
    std::ofstream f(TomlPath(), std::ios::trunc);
    for (const auto& l : g_lines) f << l << "\n";
}

// sizes of the chosen shape, biggest first: the monitor's own and the common ones that fit the desktop
bool Shaped(uint32_t w, uint32_t h, bool wide) {
    float r = float(w) / float(h), want = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    return r > want - 0.02f && r < want + 0.02f;
}

std::vector<std::pair<uint32_t, uint32_t>> Sizes(bool wide) {
    static const std::pair<uint32_t, uint32_t> kCommon[] = {
        {640, 480},   {800, 600},   {1024, 768},  {1280, 960},  {1440, 1080}, {1600, 1200}, {1920, 1440},
        {2048, 1536}, {2880, 2160}, {1280, 720},  {1600, 900},  {1920, 1080}, {2560, 1440}, {3200, 1800},
        {3840, 2160}};
    uint32_t dw = GetSystemMetrics(SM_CXSCREEN), dh = GetSystemMetrics(SM_CYSCREEN);
    std::vector<std::pair<uint32_t, uint32_t>> m;
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(nullptr, i, &dm); i++)
        if (dm.dmPelsWidth >= 640 && Shaped(dm.dmPelsWidth, dm.dmPelsHeight, wide))
            m.emplace_back(dm.dmPelsWidth, dm.dmPelsHeight);
    for (auto [w, h] : kCommon)
        if (Shaped(w, h, wide) && w <= dw && h <= dh) m.emplace_back(w, h);
    std::sort(m.begin(), m.end(), [](auto a, auto b) { return a.first * a.second > b.first * b.second; });
    m.erase(std::unique(m.begin(), m.end()), m.end());
    return m;
}

// the size kept when it has the shape, otherwise the biggest that has
void FitSize(Settings& s) {
    if (s.width && s.height && Shaped(s.width, s.height, s.wide)) return;
    auto m = Sizes(s.wide);
    if (m.empty()) return;
    s.width = m.front().first;
    s.height = m.front().second;
}

// logs\runs keeps the newest 10 runs (each a game log and a kernel log), as CoS's launcher does: the newest ones are
// the ones a problem report needs. Names sort by time
static void PruneLogs(const std::wstring& runs) {
    for (const wchar_t* kind : {L"hpps_run_*.log", L"xbox_kernel_*.log"}) {
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((runs + kind).c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do names.push_back(fd.cFileName);
        while (FindNextFileW(f, &fd));
        FindClose(f);
        std::sort(names.begin(), names.end());
        // this run's pair is made after: keep 9 so 10 are left
        for (size_t k = 0; k + 9 < names.size(); k++) DeleteFileW((runs + names[k]).c_str());
    }
}

// hpps.exe beside this one, with the environment the game expects (debug.env beside it can add to it)
std::string FilesLogLine();

bool StartGame() {
    // one started from elsewhere while this page was open: the log below would be truncated under it
    if (hpps_GameRunning()) {
        hpps_ReportAlreadyRunning();
        return true;
    }
    const std::pair<const wchar_t*, const wchar_t*> kEnv[] = {
        {L"RECOMP_TITLE_KEVENTS", L"1"}, {L"RECOMP_VBLANK", L"1"},       {L"RECOMP_AC97_READY", L"1"},
        {L"RECOMP_DSP_ACK", L"0x834D0810"}, {L"RECOMP_PB_EXEC", L"1"}, {L"RECOMP_FB_WINDOW", L"1"},
        {L"RECOMP_USB", L"1"},           {L"RECOMP_START_MENU", L"0"},  {L"RECOMP_ACK_SLEEP_US", L"250"},
        {L"RECOMP_HEAP_RECLAIM", L"1"},
    };
    for (auto [k, v] : kEnv)
        if (!GetEnvironmentVariableW(k, nullptr, 0)) SetEnvironmentVariableW(k, v);
    // hpps.exe refuses to start without it (single_instance.h)
    SetEnvironmentVariableW(HPPS_LAUNCHER_ENV, L"1");
    std::wstring dir = ExeDir(), exe = dir + L"hpps.exe";
    std::wstring cmd = L"\"" + exe + L"\"";
    // the game's console output (its [CRASH] reports among it) to logs\runs\hpps_run_<date_time>.log: a windowed
    // launcher has no console to hand down. Every run, as CoS's launcher does (with debug_logs gating it, a problem
    // met with the setting off left nothing to look at); debug_logs now only adds extra traces. The kernel's log
    // beside it under the same time (XBOX_KERNEL_LOG), and the name for F10's Debug tab (HPPS_RUN_LOG)
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04u%02u%02u_%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
             now.wSecond);
    CreateDirectoryW((dir + L"logs").c_str(), nullptr);
    CreateDirectoryW((dir + L"logs\\runs").c_str(), nullptr);
    PruneLogs(dir + L"logs\\runs\\");
    std::wstring run_log = L"logs\\runs\\hpps_run_" + std::wstring(stamp) + L".log";
    std::wstring kernel_log = L"logs\\runs\\xbox_kernel_" + std::wstring(stamp) + L".log";
    SetEnvironmentVariableW(L"HPPS_RUN_LOG", run_log.c_str());
    SetEnvironmentVariableW(L"XBOX_KERNEL_LOG", kernel_log.c_str());
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileW((dir + run_log).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (log != INVALID_HANDLE_VALUE) {
        // the game folder's state first, so a log sent in says whether the files were whole
        std::string files = FilesLogLine();
        DWORD put = 0;
        WriteFile(log, files.data(), DWORD(files.size()), &put, nullptr);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = si.hStdError = log;
    }
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, dir.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// ── Disc image ──────────────────────────────────────────────────────────────────────────────────────────────────────
// game\ holds a disc image rather than its files: unpacked into game\ on a thread of its own before the game can
// start. Stopped or failed, the next start carries on where it stopped (files already whole are kept).

struct Unpack {
    XisoProgress p{};
    HANDLE thread = nullptr;
    bool ok = false;
    std::wstring image, dir;
    std::string name;  // the image's file name, for the page
};
Unpack g_unpack;

DWORD WINAPI UnpackThread(void*) {
    g_unpack.ok = xiso_Extract(g_unpack.image.c_str(), g_unpack.dir.c_str(), &g_unpack.p);
    return 0;
}

bool StartUnpack() {
    WCHAR image[MAX_PATH];
    g_unpack.dir = ExeDir() + L"game";
    if (!xiso_Find(g_unpack.dir.c_str(), image)) return false;
    g_unpack.image = image;
    g_unpack.name = Narrow(g_unpack.image.substr(g_unpack.image.find_last_of(L"\\/") + 1));
    g_unpack.thread = CreateThread(nullptr, 0, UnpackThread, nullptr, 0, nullptr);
    return g_unpack.thread != nullptr;
}

void DrawUnpack() {
    ImGuiIO& io = ImGui::GetIO();
    float sc = io.FontGlobalScale;
    float pw = 560 * sc;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.97f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(pw, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::Begin("##unpack", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Unpacking the game files");
    ImGui::TextWrapped("From %s into the game folder. This happens once; the disc image is left as it is and can "
                       "be deleted afterwards.", g_unpack.name.c_str());
    ImGui::Spacing();
    double done = double(g_unpack.p.done), total = double(g_unpack.p.total);
    char text[64];
    std::snprintf(text, sizeof text, "%.2f of %.2f GB", done / 1e9, total / 1e9);
    ImGui::ProgressBar(total > 0 ? float(done / total) : 0.0f, ImVec2(-FLT_MIN, 28 * sc), text);
    ImGui::Spacing();
    bool cancelling = g_unpack.p.cancel != 0;
    if (cancelling) ImGui::BeginDisabled();
    if (ImGui::Button("Cancel", ImVec2(160 * sc, 36 * sc))) InterlockedExchange(&g_unpack.p.cancel, 1);
    if (cancelling) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("The next start carries on where it stopped.");
    ImGui::End();
}

// ── Game files ──────────────────────────────────────────────────────────────────────────────────────────────────────
// game\ checked against the disc's files (gamefiles.h). Every byte is read once, on a fresh start (no game_check.txt
// made under this list) or after unpacking, and again only from the page's Verify and Repair buttons; every other
// start looks at the sizes alone (a few ms) and never reads them all on its own. The full check's result, the damaged
// files by name, is kept in game_check.txt beside the launcher and reported on later starts until a Verify or Repair
// clears it; a damaged or missing file is named on the page and in the run log (2026-10-29: a player's Burrow had no
// music, and nothing said whether their files were whole; from the CoS port).

struct Check {
    GameCheck c{};
    HANDLE thread = nullptr;
    BOOL contents = FALSE;
    BOOL finished = FALSE;  // ran to the end (not cancelled, nothing failed)
    bool ran = false;       // a check has finished this session
    std::string date;       // when the contents were last checked, "" never
};
Check g_check;

std::wstring GameDir() { return ExeDir() + L"game"; }
std::wstring StampPath() { return ExeDir() + L"game_check.txt"; }

std::string Today() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char d[16];
    std::snprintf(d, sizeof d, "%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
    return d;
}

// the last full check, when it was made under this list
struct Stamp {
    bool valid = false;
    std::string date;
    std::vector<std::string> damaged;  // its files with wrong contents (missing and wrong-size ones a size check finds)
};

Stamp ReadStamp() {
    Stamp st;
    std::ifstream f(Narrow(StampPath()));
    char want[32];
    std::snprintf(want, sizeof want, "list %08lX", (unsigned long)gamefiles_ListId());
    std::string l;
    if (!std::getline(f, l) || Trim(l) != want) return st;
    while (std::getline(f, l)) {
        l = Trim(l);
        if (l.rfind("date ", 0) == 0) st.date = l.substr(5);
        else if (l.rfind("damaged ", 0) == 0) st.damaged.push_back(l.substr(8));
    }
    st.valid = !st.date.empty();
    return st;
}

void WriteStamp() {
    std::ofstream f(Narrow(StampPath()), std::ios::trunc);
    char list[32];
    std::snprintf(list, sizeof list, "list %08lX", (unsigned long)gamefiles_ListId());
    f << list << "\n"
      << "date " << g_check.date << "\n";
    for (int k = 0; k < g_check.c.listed; k++)
        if (g_check.c.kind[k] == GAMEFILE_CONTENT) f << "damaged " << gamefiles_Name(g_check.c.index[k]) << "\n";
    f << "# hpps_launcher.exe's last full read of the game folder. Later starts check the sizes only; the launcher's\n"
         "# Verify game files button reads everything again (or delete this file to do it at the next start).\n";
}

DWORD WINAPI CheckThread(void*) {
    g_check.finished = gamefiles_Check(GameDir().c_str(), g_check.contents, &g_check.c);
    return 0;
}

bool StartCheck(bool contents) {
    g_check.c = GameCheck{};
    g_check.contents = contents;
    g_check.finished = FALSE;
    g_check.thread = CreateThread(nullptr, 0, CheckThread, nullptr, 0, nullptr);
    return g_check.thread != nullptr;
}

bool FilesWhole() {
    return g_check.c.missing + g_check.c.wrong_size + g_check.c.wrong_content == 0;
}

// after a check: a full one recorded; a size check gets the damage the last full one found added, the stamp kept
void FinishCheck(const Stamp* st = nullptr) {
    g_check.ran = true;
    if (!g_check.finished) return;
    if (g_check.contents) {
        g_check.date = Today();
        WriteStamp();
    } else if (st) {
        for (const std::string& name : st->damaged)
            gamefiles_AddResult(&g_check.c, gamefiles_Find(name.c_str()), GAMEFILE_CONTENT);
    }
}

const char* KindName(int kind) {
    return kind == GAMEFILE_MISSING ? "missing" : kind == GAMEFILE_SIZE ? "wrong size" : "damaged";
}

// the run log's first line
std::string FilesLogLine() {
    char head[200];
    if (!g_check.ran) return "[FILES] game folder not checked\r\n";
    if (!g_check.finished) {
        return "[FILES] game folder check stopped: " + Narrow(g_check.c.error) + "\r\n";
    }
    std::snprintf(head, sizeof head, "[FILES] game folder: %d files listed, contents checked %s%s: ", gamefiles_Count(),
                  g_check.date.empty() ? "never" : g_check.date.c_str(), g_check.contents ? "" : ", sizes now");
    std::string line = head;
    if (FilesWhole()) return line + "all match the disc\r\n";
    std::snprintf(head, sizeof head, "%d missing, %d wrong size, %d damaged:", g_check.c.missing, g_check.c.wrong_size,
                  g_check.c.wrong_content);
    line += head;
    for (int k = 0; k < g_check.c.listed; k++)
        line += std::string(" ") + gamefiles_Name(g_check.c.index[k]) + " (" + KindName(g_check.c.kind[k]) + ")";
    return line + "\r\n";
}

void DrawCheck() {
    ImGuiIO& io = ImGui::GetIO();
    float sc = io.FontGlobalScale;
    float pw = 560 * sc;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.97f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(pw, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::Begin("##check", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Checking the game files");
    ImGui::TextWrapped("Every file in the game folder is compared with the game disc's, so a damaged or missing one "
                       "is found before you play. This happens once.");
    ImGui::Spacing();
    double done = double(g_check.c.done), total = double(g_check.c.total);
    char text[64];
    std::snprintf(text, sizeof text, "%.2f of %.2f GB", done / 1e9, total / 1e9);
    ImGui::ProgressBar(total > 0 ? float(done / total) : 0.0f, ImVec2(-FLT_MIN, 28 * sc), text);
    ImGui::TextDisabled("%s", gamefiles_Name(g_check.c.current));
    ImGui::Spacing();
    bool cancelling = g_check.c.cancel != 0;
    if (cancelling) ImGui::BeginDisabled();
    if (ImGui::Button("Skip", ImVec2(160 * sc, 36 * sc))) InterlockedExchange(&g_check.c.cancel, 1);
    if (cancelling) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("It runs again at the next start.");
    ImGui::End();
}

enum class FilesAction { None, Verify, Repair };

// the page's game files part: a line when all is well, the files named and what to do when not
FilesAction DrawFiles(float sc) {
    FilesAction act = FilesAction::None;
    if (g_check.ran && g_check.finished && !FilesWhole()) {
        int bad = g_check.c.missing + g_check.c.wrong_size + g_check.c.wrong_content;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.45f, 0.4f, 1));
        if (g_check.c.missing == gamefiles_Count())
            ImGui::TextWrapped("No game files found. Put the game's files, or its disc image (.iso), in the 'game' "
                               "folder beside the launcher.");
        else
            ImGui::TextWrapped("%d game file%s %s damaged or missing. The game may crash, or lose sound or "
                               "pictures, where it uses them:", bad, bad == 1 ? "" : "s", bad == 1 ? "is" : "are");
        ImGui::PopStyleColor();
        if (g_check.c.missing != gamefiles_Count()) {
            int shown = std::min(g_check.c.listed, 6);
            for (int k = 0; k < shown; k++)
                ImGui::BulletText("%s (%s)", gamefiles_Name(g_check.c.index[k]), KindName(g_check.c.kind[k]));
            if (bad > shown) ImGui::TextDisabled("...and %d more (all of them in the run log, with debug logs on)", bad - shown);
            WCHAR image[MAX_PATH];
            if (xiso_FindAny(GameDir().c_str(), image)) {
                std::wstring w(image);
                std::string name = Narrow(w.substr(w.find_last_of(L"\\/") + 1));
                if (ImGui::Button(("Repair from " + name).c_str(), ImVec2(-FLT_MIN, 34 * sc))) act = FilesAction::Repair;
            } else {
                ImGui::TextWrapped("Copy them again from your disc or game dump, or put the disc image (.iso) in the "
                                   "'game' folder and the launcher writes them again from it.");
            }
        }
    }
    if (ImGui::Button("Verify game files", ImVec2(200 * sc, 0))) act = FilesAction::Verify;
    ImGui::SameLine();
    if (!g_check.ran) ImGui::TextDisabled("Game files not checked.");
    else if (!g_check.finished) ImGui::TextDisabled("Skipped: runs again next start.");
    else if (FilesWhole())
        ImGui::TextDisabled("All match the disc (checked %s).", g_check.date.c_str());
    else if (g_check.contents) ImGui::TextDisabled("Read through just now.");
    else ImGui::TextDisabled("Sizes checked; contents %s.", g_check.date.c_str());
    return act;
}

// ── Direct3D 11 ─────────────────────────────────────────────────────────────────────────────────────────────────────

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

void MakeRtv() {
    ID3D11Texture2D* bb = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
}

bool InitD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    // xboxrecomp: d3d11 and what it loads (dxgi) from System32 only, not the exe's folder: a dxgi.dll left beside the
    // exes (DXVK's, put there for the game) was taken by Wine's own d3d11, which then called DXGID3D10CreateDevice, a
    // function DXVK's dxgi lacks: the launcher crashed before its window opened (2026-10-29, the CoS port). Not linked,
    // so nothing loads it before this.
    HMODULE d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
                              reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")))
                        : nullptr;
    if (!create || FAILED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                                 &g_swap, &g_dev, &fl, &g_ctx)))
        return false;
    MakeRtv();
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (g_swap && wp != SIZE_MINIMIZED) {
                if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
                g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
                MakeRtv();
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── the page ────────────────────────────────────────────────────────────────────────────────────────────────────────

template <size_t N>
int IndexOf(const uint32_t (&values)[N], uint32_t v) {
    for (size_t i = 0; i < N; i++)
        if (values[i] == v) return int(i);
    return 0;
}


ImFont* g_title_font = nullptr;  // Magic School One, for the title (WinMain)

// xboxrecomp: a coloured backdrop with the title as plain text in Magic School One, so the page says which game it starts
// without carrying any of the game's artwork (2026-10-29: the picture made from the title's textures is gone, in every
// build, for copyright)
void DrawBackground() {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float ww = io.DisplaySize.x, wh = io.DisplaySize.y;
    // Gryffindor scarlet, lighter at the top
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(ww, wh), IM_COL32(124, 10, 2, 255), IM_COL32(124, 10, 2, 255), IM_COL32(48, 4, 4, 255), IM_COL32(48, 4, 4, 255));
    ImFont* f = g_title_font ? g_title_font : ImGui::GetFont();
    // "Harry Potter" on its own line, bigger; the rest of the title under it on one line, made smaller to fit if need be.
    // Magic School One draws small for its size, so both are bigger than a book face would be
    struct Line {
        const char* text;
        float size;
    };
    float big = std::min(wh * 0.15f, ww * 0.13f), rest = big * 0.62f;
    float rest_w = f->CalcTextSizeA(rest, FLT_MAX, 0, "and the Philosopher's Stone").x;
    if (rest_w > ww * 0.92f) rest *= ww * 0.92f / rest_w;
    const Line lines[2] = {{"Harry Potter", big}, {"and the Philosopher's Stone", rest}};
    float y = wh * 0.05f;
    for (const Line& l : lines) {
        ImVec2 t = f->CalcTextSizeA(l.size, FLT_MAX, 0, l.text);
        ImVec2 p((ww - t.x) * 0.5f, y);
        dl->AddText(f, l.size, ImVec2(p.x + 2, p.y + 2), IM_COL32(0, 0, 0, 200), l.text);
        dl->AddText(f, l.size, p, IM_COL32(222, 186, 92, 255), l.text);
        y += l.size * 0.9f;
    }
}

// returns true when Play is pressed (settings saved)
bool DrawPage(Settings& s, std::string& error, FilesAction& files) {
    ImGuiIO& io = ImGui::GetIO();
    float sc = io.FontGlobalScale;
    float pw = 560 * sc;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.97f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(pw, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::Begin("##page", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);

    // the shape, two big choices
    float bw = (pw - ImGui::GetStyle().WindowPadding.x * 2 - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    for (int i = 0; i < 2; i++) {
        bool on = s.wide == (i == 1);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.55f, 0.15f, 1.0f));
        if (ImGui::Button(i ? "16:9  widescreen" : "4:3  original", ImVec2(bw, 44 * sc))) {
            s.wide = i == 1;
            FitSize(s);
        }
        if (on) ImGui::PopStyleColor();
        if (!i) ImGui::SameLine();
    }
    ImGui::Spacing();
    ImGui::PushItemWidth(pw * 0.5f);

    ImGui::Combo("Display mode", &s.display, kDisplayNames, 3);
    {
        auto sizes = Sizes(s.wide);
        std::string cur = std::to_string(s.width) + " x " + std::to_string(s.height);
        if (ImGui::BeginCombo(s.display == 0 ? "Window size" : "Resolution", cur.c_str())) {
            for (auto [w, h] : sizes) {
                std::string name = std::to_string(w) + " x " + std::to_string(h);
                if (ImGui::Selectable(name.c_str(), w == s.width && h == s.height)) {
                    s.width = w;
                    s.height = h;
                }
            }
            ImGui::EndCombo();
        }
    }
    int ir = IndexOf(kInternalValues, s.internal_height);
    if (ImGui::Combo("Internal resolution", &ir, kInternalNames, int(std::size(kInternalNames))))
        s.internal_height = kInternalValues[ir];
    int ms = IndexOf(kMsaaValues, s.msaa);
    if (ImGui::Combo("MSAA", &ms, kMsaaNames, int(std::size(kMsaaNames)))) s.msaa = kMsaaValues[ms];
    int an = IndexOf(kAnisoValues, s.aniso);
    if (ImGui::Combo("Anisotropic filtering", &an, kAnisoNames, int(std::size(kAnisoNames)))) s.aniso = kAnisoValues[an];
    ImGui::Checkbox("SMAA", &s.smaa);
    ImGui::SameLine(0, 30 * sc);
    ImGui::Checkbox("Vsync", &s.vsync);
    const char* const kFps[] = {"Game's rate (60 fps)", "30 fps", "Unlocked (experimental)"};
    ImGui::Combo("Frame rate", &s.fps, kFps, 3);
    ImGui::Combo("Language", &s.language, kLanguageNames, 7);
    ImGui::Combo("Colour-blind filter", &s.colorblind, kColorBlindNames, 4);
    if (s.colorblind) {
        int st = int(s.cb_strength);
        if (ImGui::SliderInt("Filter strength %", &st, 0, 100)) s.cb_strength = uint32_t(st);
    }
    ImGui::Checkbox("Keyboard and mouse", &s.kbm);
    if (s.kbm) {
        ImGui::SameLine(0, 30 * sc);
        ImGui::SetNextItemWidth(pw * 0.3f);
        int ms = int(s.mouse_sens);
        if (ImGui::SliderInt("Mouse sensitivity", &ms, 1, 100)) s.mouse_sens = uint32_t(ms);
    }
    ImGui::Checkbox("Use controller", &s.controller);
    ImGui::SameLine(0, 30 * sc);
    ImGui::Checkbox("Vibration", &s.vibration);
    ImGui::SameLine(0, 30 * sc);
    ImGui::Checkbox("Invert camera X", &s.invert_x);
    ImGui::SameLine(0, 30 * sc);
    ImGui::Checkbox("Invert camera Y", &s.invert_y);
    ImGui::TextDisabled("Key bindings, button remapping and stick settings: F10 > Controls in the game.");
    ImGui::PopItemWidth();

    ImGui::Spacing();
    files = DrawFiles(sc);
    ImGui::Spacing();
    bool skip = !s.show;
    if (ImGui::Checkbox("Don't show this at the next startup", &skip)) s.show = !skip;
    if (skip) ImGui::TextDisabled("Play starts the game directly. Hold Shift while starting, or use F10 in the game, to bring this back.");
    ImGui::Spacing();
    bool play = false;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    if (ImGui::Button("Play", ImVec2(bw, 40 * sc))) play = true;
    ImGui::PopStyleColor(2);
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(bw, 40 * sc))) PostQuitMessage(0);
    if (!error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error.c_str());
    ImGui::TextDisabled("Saved to hpps.toml. F10 in the game changes the picture settings too.");
    ImGui::End();

    if (play) Save(s);
    return play;
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    // xboxrecomp: a game already running: say so and show it, no settings page (its changes would not apply)
    if (hpps_GameRunning()) {
        hpps_ReportAlreadyRunning();
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // xboxrecomp: a disc image to unpack first shows its progress page even with the settings page turned off
    bool unpacking = StartUnpack();
    // xboxrecomp: then the game files: read through on a fresh start (a page with its progress), otherwise their sizes
    // only, here (a few ms)
    bool checking = false;
    if (!unpacking) {
        Stamp st = ReadStamp();
        if (!st.valid) {
            checking = StartCheck(true);  // a fresh start: every byte, once
        } else {
            g_check.date = st.date;
            g_check.contents = FALSE;
            g_check.finished = gamefiles_Check(GameDir().c_str(), FALSE, &g_check.c);
            FinishCheck(&st);
        }
    }
    // the page was turned off: straight to the game with the saved settings, unless Shift is held
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (!unpacking && !checking && FilesWhole() && !Load().show && !shift && StartGame()) {
        CoUninitialize();
        return 0;
    }
    SetProcessDPIAware();
    // xboxrecomp: the title image icon (launcher.rc) on the window and taskbar
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)),
                   nullptr, nullptr, L"hpps_launcher", nullptr};
    RegisterClassExW(&wc);
    // 4:3 like the art, a good part of the screen
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ch = std::clamp(sh * 2 / 3, 600, 1200), cw = ch * 4 / 3;
    RECT rc{0, 0, cw, ch};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Harry Potter and the Philosopher's Stone", style, CW_USEDEFAULT,
                              CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    if (!InitD3D(hwnd)) {
        MessageBoxW(hwnd, L"Direct3D 11 could not start.", L"Launcher", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.FontGlobalScale = std::max(1.0f, float(ch) / 600.0f);
    // xboxrecomp: the page's own font first (the default), then the title's, Magic School One, built into the exe
    // (font/title_font.inc); the default font stands in should it fail
    io.Fonts->AddFontDefault();
    {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;  // the exe's own bytes, not the atlas's to free
        g_title_font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kTitleFont), int(sizeof kTitleFont),
                                                      64.0f, &cfg);
    }
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6;
    ImGui::GetStyle().FrameRounding = 4;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    Settings s = Load();
    FitSize(s);
    std::string error;
    bool started = false;
    for (MSG msg{}; !started;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) goto done;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawBackground();
        if (unpacking) {
            DrawUnpack();
            if (WaitForSingleObject(g_unpack.thread, 0) == WAIT_OBJECT_0) {
                CloseHandle(g_unpack.thread);
                g_unpack.thread = nullptr;
                unpacking = false;
                if (!g_unpack.ok)
                    error = Narrow(g_unpack.p.error) + " The game can't start until it is unpacked; open the launcher "
                            "again to carry on.";
                else
                    checking = StartCheck(true);  // what was written, read back against the list
            }
        } else if (checking) {
            DrawCheck();
            if (WaitForSingleObject(g_check.thread, 0) == WAIT_OBJECT_0) {
                CloseHandle(g_check.thread);
                g_check.thread = nullptr;
                checking = false;
                FinishCheck();
                // the page was turned off: on to the game as before, when every file is whole
                if (g_check.finished && FilesWhole() && !s.show && !shift && StartGame()) started = true;
            }
        } else if (FilesAction files = FilesAction::None; DrawPage(s, error, files)) {
            WCHAR image[MAX_PATH];
            if (xiso_Find((ExeDir() + L"game").c_str(), image))  // stopped part-way: not playable yet
                error = "The game files are not unpacked yet. Open the launcher again to carry on.";
            else if (StartGame()) started = true;
            else error = "hpps.exe could not be started (it must sit beside the launcher).";
        } else if (files == FilesAction::Verify) {
            error.clear();
            checking = StartCheck(true);
        } else if (files == FilesAction::Repair) {
            // the damaged ones removed, then the image unpacked over the folder: only what is not whole is written
            WCHAR image[MAX_PATH];
            error.clear();
            if (xiso_FindAny(GameDir().c_str(), image)) {
                gamefiles_RemoveDamaged(GameDir().c_str(), &g_check.c);
                g_unpack = Unpack{};
                g_unpack.dir = GameDir();
                g_unpack.image = image;
                g_unpack.name = Narrow(g_unpack.image.substr(g_unpack.image.find_last_of(L"\\/") + 1));
                g_unpack.thread = CreateThread(nullptr, 0, UnpackThread, nullptr, 0, nullptr);
                unpacking = g_unpack.thread != nullptr;
            }
        }
        ImGui::Render();
        const float black[4] = {0, 0, 0, 1};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, black);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }
done:
    // closed while unpacking: stop it (the file being written is removed; the next start carries on)
    if (g_unpack.thread) {
        InterlockedExchange(&g_unpack.p.cancel, 1);
        WaitForSingleObject(g_unpack.thread, INFINITE);
        CloseHandle(g_unpack.thread);
    }
    // closed while checking: stopped, no stamp written, so the next start checks again
    if (g_check.thread) {
        InterlockedExchange(&g_check.c.cancel, 1);
        WaitForSingleObject(g_check.thread, INFINITE);
        CloseHandle(g_check.thread);
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    DestroyWindow(hwnd);
    CoUninitialize();
    return 0;
}
