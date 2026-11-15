/* xboxrecomp: one copy of the game at a time. hpps.exe holds a named mutex for its whole life; a second hpps.exe,
 * or the launcher while one runs, says so, brings the running game's window forward and starts nothing. A second
 * copy would share saves\, shadercache\ and hpps_run.log with the first (and the launcher would truncate its log). */
#ifndef HPPS_SINGLE_INSTANCE_H
#define HPPS_SINGLE_INSTANCE_H

#include <wchar.h>
#include <windows.h>

#include "edition.h"

#define HPPS_RUNNING_MUTEX L"Local\\hpps_game_running"
#define HPPS_GAME_WINDOW_CLASS "XboxRecompFramebuffer" /* fb_present.c */

/* the message, then the running game's window to the front (after the box: a fullscreen game would cover it) */
static inline void hpps_ReportAlreadyRunning(void)
{
    MessageBoxW(NULL, HPPS_GAME_TITLE_W L" is already running.",
                HPPS_GAME_TITLE_W, MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    HWND w = FindWindowA(HPPS_GAME_WINDOW_CLASS, NULL);
    if (w) {
        if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
        SetForegroundWindow(w);
    }
}

/* the game: takes the mutex for the rest of the process; 0 when another copy already has it */
static inline int hpps_ClaimSingleInstance(void)
{
    HANDLE m = CreateMutexW(NULL, FALSE, HPPS_RUNNING_MUTEX);
    return !(m && GetLastError() == ERROR_ALREADY_EXISTS);
}

/* whether a game holds the mutex now */
static inline int hpps_GameRunning(void)
{
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, HPPS_RUNNING_MUTEX);
    if (!m) return 0;
    CloseHandle(m);
    return 1;
}

/* xboxrecomp: hpps.exe runs only from hpps_launcher.exe, which sets this in the game's environment (with its
 * RECOMP_* settings, which a bare start would lack); test scripts set it themselves */
#define HPPS_LAUNCHER_ENV L"HPPS_LAUNCHER"

static inline int hpps_StartedByLauncher(void)
{
    return GetEnvironmentVariableW(HPPS_LAUNCHER_ENV, NULL, 0) != 0;
}

/* hpps.exe started on its own: says to use the launcher and offers to open it (in the folder above bin\, where
 * this exe is; beside it in a folder from before bin\) */
static inline void hpps_OfferLauncher(void)
{
    const wchar_t *title = HPPS_GAME_TITLE_W;
    int r = MessageBoxW(NULL, L"Please start the game with hpps_launcher.exe.\n\nOpen the launcher now?", title,
                        MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    if (r != IDYES) return;
    wchar_t dir[MAX_PATH], exe[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t *slash = n && n < MAX_PATH ? wcsrchr(dir, L'\\') : NULL;
    if (!slash) return;
    slash[1] = 0;
    lstrcpyW(exe, dir);
    lstrcatW(exe, L"hpps_launcher.exe");
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES && slash > dir) {
        wchar_t *up = slash - 1;  /* the folder above */
        while (up > dir && *up != L'\\') up--;
        if (*up == L'\\') {
            up[1] = 0;
            lstrcpyW(exe, dir);
            lstrcatW(exe, L"hpps_launcher.exe");
        }
    }
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        MessageBoxW(NULL, L"hpps_launcher.exe was not found.", title, MB_OK | MB_ICONWARNING | MB_TOPMOST);
    }
}

#endif
