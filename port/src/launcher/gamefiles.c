// gamefiles.c - checking the game folder against the known-good list of the disc's files (gamefiles.h)
//
// Two lists, one per edition the port runs (game_files_pal.inc, game_files_ntsc.inc, made by tools/make_game_files.py
// from folders that play the whole game); the folder's default.xbe says which one applies (gamefiles_DetectEdition).
// An edition's files are the same bytes in every copy, so anything else is damage (a bad copy, a disc read error, a
// failed download) or a file someone changed. A sizes-only pass costs one directory lookup a file and runs on every
// start; reading every byte (3.1 GB PAL, 1.9 GB NTSC-US) runs once, after unpacking, and when asked.
#include "gamefiles.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    const char* path;
    uint32_t size, crc;
} GameFile;

static const GameFile kFilesPal[] = {
#include "game_files_pal.inc"
};
static const GameFile kFilesNtsc[] = {
#include "game_files_ntsc.inc"
};

static int g_edition = GAME_EDITION_PAL;
static const GameFile* kFiles = kFilesPal;
static int g_count = (int)(sizeof kFilesPal / sizeof kFilesPal[0]);
#define FILE_COUNT g_count

// other good copies of a file, beside the list's (the lists are generated; these are kept by hand).
// PAL: the list's default.xbe is the commonly shared one, patched in one byte (0x145489, guest 0x155489: XAPI's
// start-up check's jge 7D made jmp EB, so a failed media check no longer returns to the dashboard); the disc's own,
// unpatched XBE is the same edition and plays the same (the code runs from the recompile; the XBE gives its data and
// passes the edition check).
// NTSC-US: the disc's own XBE is the list's; it already has that jmp (guest 0x154F29), so no patched copy goes round.
static const struct {
    int edition;
    GameFile f;
} kAlso[] = {
    {GAME_EDITION_PAL, {"default.xbe", 3264512u, 0x3D0FF840u}},  // the retail DVD's, untouched (2026-11-01)
    // NTSC-US retail, untouched: the list's copy with 0x144F29 (guest 0x154F29) back to jge 7D, SHA-256 54f278e0...;
    // a player's launcher called it damaged (2026-11-16)
    {GAME_EDITION_NTSC, {"default.xbe", 3264512u, 0xCA860F30u}},
};

static BOOL AlsoGood(int i, uint32_t crc) {
    for (size_t k = 0; k < sizeof kAlso / sizeof kAlso[0]; k++)
        if (kAlso[k].edition == g_edition && !_stricmp(kAlso[k].f.path, kFiles[i].path) &&
            kAlso[k].f.size == kFiles[i].size && kAlso[k].f.crc == crc)
            return TRUE;
    return FALSE;
}

int gamefiles_DetectEdition(const WCHAR* dir) {
    WCHAR path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%ls\\default.xbe", dir);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return GAME_EDITION_NONE;
    static uint8_t x[0x10000];  // the image header, certificate and section headers sit in the first pages
    DWORD got = 0;
    BOOL ok = ReadFile(h, x, sizeof x, &got, NULL);
    CloseHandle(h);
#define XBE32(o) (*(const uint32_t*)(x + (o)))
    if (!ok || got < 0x180 || XBE32(0) != 0x48454258u) return GAME_EDITION_OTHER;  // "XBEH"
    uint32_t base = XBE32(0x104), cert = XBE32(0x118) - base, sh = XBE32(0x120) - base;
    if (cert + 12 > got || sh + 12 > got) return GAME_EDITION_OTHER;
    uint32_t tid = XBE32(cert + 8), sections = XBE32(0x11C), text_va = XBE32(sh + 4), text_size = XBE32(sh + 8);
#undef XBE32
    // the two share title ID, size and section count; NTSC-US's code is 32 bytes longer (main.c's edition check)
    if (tid != 0x45410037u || sections != 13 || text_va != 0x11000u) return GAME_EDITION_OTHER;
    if (text_size == 1570580u) return GAME_EDITION_PAL;
    if (text_size == 1570612u) return GAME_EDITION_NTSC;
    return GAME_EDITION_OTHER;
}

void gamefiles_SetEdition(int edition) {
    g_edition = edition == GAME_EDITION_NTSC ? GAME_EDITION_NTSC : GAME_EDITION_PAL;
    if (g_edition == GAME_EDITION_NTSC) {
        kFiles = kFilesNtsc;
        g_count = (int)(sizeof kFilesNtsc / sizeof kFilesNtsc[0]);
    } else {
        kFiles = kFilesPal;
        g_count = (int)(sizeof kFilesPal / sizeof kFilesPal[0]);
    }
}

int gamefiles_Edition(void) { return g_edition; }

#define READ_BUF   (4u << 20)

// CRC-32 as zlib computes it (reflected, polynomial 0xEDB88320), eight bytes a step
static uint32_t g_crc[8][256];

static void CrcInit(void) {
    if (g_crc[0][1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int t = 1; t < 8; t++) g_crc[t][i] = (g_crc[t - 1][i] >> 8) ^ g_crc[0][g_crc[t - 1][i] & 0xFF];
}

static uint32_t Crc(uint32_t crc, const uint8_t* p, size_t n) {
    crc = ~crc;
    for (; n >= 8; p += 8, n -= 8) {
        uint32_t a, b;
        memcpy(&a, p, 4);
        memcpy(&b, p + 4, 4);
        a ^= crc;
        crc = g_crc[7][a & 0xFF] ^ g_crc[6][(a >> 8) & 0xFF] ^ g_crc[5][(a >> 16) & 0xFF] ^ g_crc[4][a >> 24] ^
              g_crc[3][b & 0xFF] ^ g_crc[2][(b >> 8) & 0xFF] ^ g_crc[1][(b >> 16) & 0xFF] ^ g_crc[0][b >> 24];
    }
    while (n--) crc = g_crc[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

int gamefiles_Count(void) { return FILE_COUNT; }

const char* gamefiles_Name(int i) { return i >= 0 && i < FILE_COUNT ? kFiles[i].path : ""; }

DWORD gamefiles_ListId(void) {
    CrcInit();
    uint32_t c = 0;
    for (int i = 0; i < FILE_COUNT; i++) {
        c = Crc(c, (const uint8_t*)kFiles[i].path, strlen(kFiles[i].path));
        c = Crc(c, (const uint8_t*)&kFiles[i].size, 4);
        c = Crc(c, (const uint8_t*)&kFiles[i].crc, 4);
    }
    for (size_t k = 0; k < sizeof kAlso / sizeof kAlso[0]; k++)
        if (kAlso[k].edition == g_edition) c = Crc(c, (const uint8_t*)&kAlso[k].f.crc, 4);
    return c;
}

static void PathOf(const WCHAR* dir, int i, WCHAR* out) {
    WCHAR name[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, kFiles[i].path, -1, name, MAX_PATH);
    swprintf_s(out, MAX_PATH, L"%ls\\%ls", dir, name);
}

static void Note(GameCheck* c, int i, int kind) {
    if (kind == GAMEFILE_MISSING) c->missing++;
    else if (kind == GAMEFILE_SIZE) c->wrong_size++;
    else c->wrong_content++;
    if (c->listed < GAMEFILES_LISTED) {
        c->index[c->listed] = i;
        c->kind[c->listed++] = kind;
    }
}

// the file's CRC-32 into *crc; FALSE when it could not be read through (c->error says why, unless cancelled)
static BOOL ReadCrc(const WCHAR* path, uint32_t size, uint8_t* buf, GameCheck* c, uint32_t* crc) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    uint32_t v = 0;
    BOOL ok = TRUE;
    for (uint32_t left = size; ok && left;) {
        DWORD want = left < READ_BUF ? left : READ_BUF, got = 0;
        if (c->cancel) ok = FALSE;
        else if (!ReadFile(h, buf, want, &got, NULL) || got != want) ok = FALSE;
        else {
            v = Crc(v, buf, got);
            left -= got;
            InterlockedAdd64(&c->done, got);
        }
    }
    CloseHandle(h);
    *crc = v;
    return ok;
}

BOOL gamefiles_Check(const WCHAR* dir, BOOL contents, GameCheck* c) {
    c->done = 0;
    c->total = 0;
    c->missing = c->wrong_size = c->wrong_content = c->listed = 0;
    c->error[0] = 0;
    CrcInit();
    for (int i = 0; i < FILE_COUNT; i++) c->total += contents ? kFiles[i].size : 1;
    uint8_t* buf = contents ? (uint8_t*)malloc(READ_BUF) : NULL;
    if (contents && !buf) {
        wcscpy_s(c->error, sizeof c->error / sizeof c->error[0], L"Out of memory.");
        return FALSE;
    }
    BOOL ok = TRUE;
    for (int i = 0; ok && i < FILE_COUNT; i++) {
        WCHAR path[MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA fad;
        c->current = i;
        if (c->cancel) {
            wcscpy_s(c->error, sizeof c->error / sizeof c->error[0], L"The check was cancelled.");
            ok = FALSE;
            break;
        }
        PathOf(dir, i, path);
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad) || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            Note(c, i, GAMEFILE_MISSING);
            InterlockedAdd64(&c->done, contents ? kFiles[i].size : 1);
            continue;
        }
        if (fad.nFileSizeHigh || fad.nFileSizeLow != kFiles[i].size) {
            Note(c, i, GAMEFILE_SIZE);
            InterlockedAdd64(&c->done, contents ? kFiles[i].size : 1);
            continue;
        }
        if (!contents) {
            InterlockedAdd64(&c->done, 1);
            continue;
        }
        uint32_t crc = 0;
        LONG64 before = c->done;
        if (!ReadCrc(path, kFiles[i].size, buf, c, &crc)) {
            if (c->cancel) {
                wcscpy_s(c->error, sizeof c->error / sizeof c->error[0], L"The check was cancelled.");
                ok = FALSE;
            } else {
                // there but unreadable (a bad sector, a file another program holds): as damaged as a wrong byte
                Note(c, i, GAMEFILE_CONTENT);
                InterlockedExchange64(&c->done, before + kFiles[i].size);
            }
        } else if (crc != kFiles[i].crc && !AlsoGood(i, crc)) {
            Note(c, i, GAMEFILE_CONTENT);
        }
    }
    free(buf);
    return ok;
}

int gamefiles_Find(const char* name) {
    for (int i = 0; i < FILE_COUNT; i++)
        if (!_stricmp(kFiles[i].path, name)) return i;
    return -1;
}

void gamefiles_AddResult(GameCheck* c, int i, int kind) {
    if (i < 0 || i >= FILE_COUNT) return;
    for (int k = 0; k < c->listed; k++)
        if (c->index[k] == i) return;
    if (c->listed == GAMEFILES_LISTED) return;  // the counts could not tell a repeat from a new one
    Note(c, i, kind);
}

void gamefiles_RemoveDamaged(const WCHAR* dir, const GameCheck* c) {
    for (int k = 0; k < c->listed; k++) {
        if (c->kind[k] == GAMEFILE_MISSING) continue;
        WCHAR path[MAX_PATH];
        PathOf(dir, c->index[k], path);
        SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);  // copies off a disc can come read-only
        DeleteFileW(path);
    }
}
