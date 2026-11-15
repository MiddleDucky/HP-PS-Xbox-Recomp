/* hpps.exe: what the launcher starts. The game itself is one DLL per edition, both built from the same sources
 * (CMakeLists.txt): hpps_pal.dll for the PAL Philosopher's Stone, hpps_ntsc.dll for the NTSC-US Sorcerer's Stone.
 * game\default.xbe says which, by the launcher's own check (gamefiles_DetectEdition); its hpps_GameMain (main.c)
 * then runs the game in this process. This exe and the DLLs sit in bin\; game\ is found from the working folder, as
 * the game finds it (the launcher starts this in its own folder, which holds game\ and saves\). No default.xbe, or
 * one of neither edition, gets the PAL DLL: the game's own edition check (main.c) says what is wrong with it. */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#include "../launcher/gamefiles.h"

int main(void)
{
    WCHAR dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    WCHAR *slash = (n && n < MAX_PATH) ? wcsrchr(dir, L'\\') : NULL;
    if (slash)
        slash[1] = 0;
    else
        dir[0] = 0;

    WCHAR game[MAX_PATH], path[MAX_PATH];
    if (!GetFullPathNameW(L"game", MAX_PATH, game, NULL))
        lstrcpyW(game, L"game");
    const WCHAR *name = gamefiles_DetectEdition(game) == GAME_EDITION_NTSC ? L"hpps_ntsc.dll" : L"hpps_pal.dll";
    swprintf_s(path, MAX_PATH, L"%ls%ls", dir, name);

    /* the full path, and the DLL's own folder searched for what it imports (unicorn.dll in a difftest build) */
    HMODULE dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    int (*game_main)(void) = dll ? (int (*)(void))(void *)GetProcAddress(dll, "hpps_GameMain") : NULL;
    if (!game_main) {
        DWORD err = GetLastError();
        WCHAR msg[768];
        swprintf_s(msg, 768,
                   L"%ls could not be %ls (error %lu).\n\n"
                   L"It is the game for the edition in the 'game' folder, and belongs in the bin folder with hpps.exe:\n%ls\n\n"
                   L"Unpacking the download again restores it.",
                   name, dll ? L"started" : L"loaded", err, path);
        MessageBoxW(NULL, msg, L"Harry Potter", MB_ICONERROR);
        return 1;
    }
    return game_main();
}
