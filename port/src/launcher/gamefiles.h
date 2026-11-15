// gamefiles.h - checking the game folder against the known-good list of the disc's files (gamefiles.c,
// game_files_pal.inc / game_files_ntsc.inc from tools/make_game_files.py), for hpps_launcher.exe
#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { GAMEFILE_OK, GAMEFILE_MISSING, GAMEFILE_SIZE, GAMEFILE_CONTENT };

// The editions the port runs: the PAL (Europe) Philosopher's Stone (hpps_pal.dll) and the NTSC-US Sorcerer's Stone
// (hpps_ntsc.dll), both loaded by hpps.exe. NONE: no default.xbe; OTHER: one that is neither.
enum { GAME_EDITION_NONE, GAME_EDITION_OTHER, GAME_EDITION_PAL, GAME_EDITION_NTSC };

// dir\default.xbe's edition, from its header
int gamefiles_DetectEdition(const WCHAR* dir);
// the list the calls below use: NTSC-US's, otherwise PAL's (the default)
void gamefiles_SetEdition(int edition);
int gamefiles_Edition(void);  // GAME_EDITION_PAL or GAME_EDITION_NTSC

#define GAMEFILES_LISTED 32  // the files named in a report; the counts cover all of them

typedef struct {
    volatile LONG64 done;    // bytes read (or, for a sizes-only check, files looked at) so far
    LONG64 total;            // bytes of all the listed files (files, for a sizes-only check)
    volatile LONG cancel;    // set from another thread: stops at the next block
    volatile LONG current;   // the file being read, an index for gamefiles_Name
    int missing, wrong_size, wrong_content;
    int listed;
    int index[GAMEFILES_LISTED];  // the first of them, in list order
    int kind[GAMEFILES_LISTED];   // GAMEFILE_*
    WCHAR error[320];        // why the check could not finish (cancelled, a read failing)
} GameCheck;

int gamefiles_Count(void);
const char* gamefiles_Name(int i);  // its path in the game folder, "EAMusic\\burrow.mus"
DWORD gamefiles_ListId(void);       // CRC of the list itself: a check recorded under another list is stale

// Checks dir against the list: every file there, its size, and with contents its CRC-32 too. Files the list does not
// name are left alone (saves, the player's own additions). FALSE when it could not finish (c->error); TRUE with the
// counts filled in otherwise, all zero when the folder matches.
BOOL gamefiles_Check(const WCHAR* dir, BOOL contents, GameCheck* c);

// The list entry for a path ("EAMusic\\burrow.mus", any case), -1 when it is not listed.
int gamefiles_Find(const char* name);

// Adds a result found another way (a damaged file recorded by an earlier full check) to c, unless c already has that
// file among the ones it names.
void gamefiles_AddResult(GameCheck* c, int i, int kind);

// Deletes the files the last check found damaged (wrong size or content) so an unpack writes them again. Only the
// ones in c->index: the counts above GAMEFILES_LISTED wait for the next round.
void gamefiles_RemoveDamaged(const WCHAR* dir, const GameCheck* c);

#ifdef __cplusplus
}
#endif
