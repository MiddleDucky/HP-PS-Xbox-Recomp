/**
 * The debug trainer's game side (the trainer itself is hpps_trainer.exe,
 * src/trainer): a named shared block (trainer_shm.h) and Harry's per-frame
 * update, sub_00095370(entity), as the place the requests are carried out --
 * the title's own thread, between its frames.
 *
 * What is known of the title (found for this):
 *   [0x3565B0]           the player's entity ("Harry"); +0x28 its scene object,
 *                        +0xDC its controller
 *   object +0x40/+0x80   two world matrices, +0x2C the current one's index;
 *                        sub_0016B7C0(object, matrix, 0) sets it (the
 *                        PositionObjectImmediate script command's way)
 *   0x329B90 / 0x329B94  health / its maximum (floats)
 *   0x329DD5..7          the spells on the three buttons; sub_0014FEB0(slot,
 *                        spell) puts one on (off any other), sub_000342E0()
 *                        gives the player the abilities they make up
 *   0x329BD4             wizard cards: 101 counts (words); sub_0003E9A0(card 1..101)
 *                        gives one (each 20th new card adds a health bar)
 *   sub_00152160()       one more health bar: maximum and health +100, up to 600
 *   0x329B8A / 0x329B8C  beans / the bag's capacity (words: 50, 150 when bigger)
 *   0x329B98             the inventory's 17 item slots (words); a first card puts item 3 there
 *   0x1A2040             the view matrix (CDevice 0x1A13E0 + 0xC60)
 *   0x21B7BC             the frame's time step, seconds
 *   [0x356CC4]           the level in play (6 + index into the names at 0x21BB70);
 *                        sub_00146110(level) loads one (the StartLevel script command)
 *   sub_00025060(entity, name)  an entity's attribute by name, a pointer to its
 *                        value (its own in [+0x20], 16 bytes each, or the type's
 *                        shared default) or 0; enemies keep "CurrentHealth" (0x203D84)
 *   entities             +8 itself; the list through +0 / +4 (a link to the one before's +0)
 *                        holds only what is near Harry
 *   [0x329E98]           the level's own object: its scripts at [[+0x1C]+0x10] (0x34 bytes each, the
 *                        name first; [[+0x1C]+0x0C] of them), as sub_00025360(object, name) finds one;
 *                        sub_0014D1C0(object, script) starts one (Harry's update does, 0x9545A)
 *
 * Quidditch: the match is not a level of its own on the Xbox -- day5quidditch (24) has no
 * day5quid.hog on the disc. day5 plays it: its HPE53Script (run by the HPE53 trigger at the end of
 * following Ron) walks Harry into the stadium and starts the match. Level 24 here is day5 with
 * that script started.
 *
 * Insta-kill: the CurrentHealth each enemy's AI looks up as it starts (the
 * lookup is wrapped) is watched; when it drops (a hit) it is set to 0 and the
 * enemy's own logic finishes it. (The entity ring through +0 holds only what
 * is near Harry.)
 *
 * Checkpoints: the progress block (what a save holds) is handed to the
 * trainer as each level starts; restoring one writes it back and loads its
 * level.
 *
 * Noclip: Harry's update is skipped (no walking, falling, collision or
 * casting) and the left stick moves him along the camera's ground
 * directions, the right / left trigger up / down, A held 4x as fast.
 *
 * Undo: the first change the trainer makes keeps what it touches (cards,
 * health, beans, the bag, the buttons, the inventory slots) to put back.
 *
 * The debug report runs on a thread of its own (a hung title still answers):
 * every thread's native stack, the state below and a frame capture, into
 * the folder the trainer names; the trainer copies memory and logs itself.
 *
 * Built into the recompile with --exclude-manual naming this file.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"
#include "input/xinput_xbox.h"
#include "trainer_shm.h"

#define ARG(i) MEM32(g_esp + 4 + 4 * (i))

enum {
    kPlayer = 0x3565B0, kHealth = 0x329B90, kHealthMax = 0x329B94, kSlots = 0x329DD5,
    kView = 0x1A2040, kFrameTime = 0x21B7BC, kCards = 0x329BD4, kCardCount = 101, kBeans = 0x329B8A,
    kBeansMax = 0x329B8C, kBigBag = 150, kSmallBag = 50, kHealthCap = 600, kHealthBar = 100,
    kInventory = 0x329B98, kInventorySlots = 17, kLevel = 0x329E6E, kLevelNow = 0x356CC4,
    kCurrentHealthName = 0x203D84, kMaxWatched = 2048, kLevelObject = 0x329E98,
    kDay5 = 23, kQuidditch = 24,
};

/* what the first trainer change found, to put back (Undo) */
static struct {
    int kept;
    uint16_t cards[kCardCount], inventory[kInventorySlots];
    float health, health_max;
    uint16_t beans, beans_max;
    uint8_t slots[3];
} g_undo;
static uint8_t g_given[kCardCount];  /* cards the trainer gave */

extern void sub_0016B7C0(void);
extern void sub_0014FEB0(void);
extern void sub_000342E0(void);
extern void sub_0003E9A0(void);
extern void sub_00152160(void);
extern void sub_00146110(void);
extern void sub_00025060(void);
extern void sub_0014D1C0(void);
extern void hpps_StackDumpTo(const char *path);
extern void hpps_RequestCapture(void);
extern ptrdiff_t g_xbox_mem_offset;

/* hle_input.c: the pad as the game last read it */
extern XBOX_GAMEPAD hpps_last_pad;

static HppsTrainerShm *g_shm;

static HppsTrainerShm *shm(void)
{
    static int tried;
    if (!g_shm && !tried) {
        tried = 1;
        HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(HppsTrainerShm),
                                      HPPS_TRAINER_NAME);
        if (m) g_shm = (HppsTrainerShm *)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HppsTrainerShm));
        if (g_shm) {
            memset(g_shm, 0, sizeof(*g_shm));
            g_shm->version = HPPS_TRAINER_VERSION;
            g_shm->pid = GetCurrentProcessId();
            g_shm->mem_offset = (uint64_t)g_xbox_mem_offset;
            g_shm->magic = HPPS_TRAINER_MAGIC;
        }
    }
    return g_shm;
}

/* runs a recompiled function on this guest thread, the arguments pushed right to left and a return address,
 * as `call` leaves them (hle_d3d8.cpp's CallGuest) */
static uint32_t call_guest(void (*fn)(void), int n, const uint32_t *args)
{
    uint32_t s_esp = g_esp, s_ebx = g_ebx, s_esi = g_esi, s_edi = g_edi, s_ebp = g_ebp;
    uint32_t sp = s_esp;
    for (int i = n; i-- > 0;) {
        sp -= 4;
        MEM32(sp) = args[i];
    }
    sp -= 4;
    MEM32(sp) = 0;  /* return address */
    g_esp = sp;
    fn();
    g_esp = s_esp, g_ebx = s_ebx, g_esi = s_esi, g_edi = s_edi, g_ebp = s_ebp;
    return g_eax;
}

static uint32_t current_matrix(uint32_t obj)
{
    return obj + 0x40 + 64 * (MEM32(obj + 0x2C) & 1);
}

/* the object's matrix with its translation moved to p, through the title's setter */
static void place(uint32_t obj, const float p[3])
{
    uint32_t s_esp = g_esp;
    uint32_t m = (g_esp - 64) & ~15u;  /* scratch below the guest stack */
    g_esp = m;
    uint32_t cur = current_matrix(obj);
    for (int i = 0; i < 16; i++) MEM32(m + 4 * i) = MEM32(cur + 4 * i);
    MEMF(m + 48) = p[0], MEMF(m + 52) = p[1], MEMF(m + 56) = p[2];
    uint32_t a[3] = {obj, m, 0};
    call_guest(sub_0016B7C0, 3, a);
    g_esp = s_esp;
}

static float stick(short v)
{
    float f = v / 32767.0f;
    if (fabsf(f) < 0.24f) return 0;
    return f > 0 ? (f - 0.24f) / 0.76f : (f + 0.24f) / 0.76f;
}

static void fly(uint32_t obj, HppsTrainerShm *t)
{
    uint32_t cur = current_matrix(obj);
    float p[3] = {MEMF(cur + 48), MEMF(cur + 52), MEMF(cur + 56)};
    /* the camera's right and forward (the view matrix's first and third columns) along the ground */
    float r[3] = {MEMF(kView + 0), 0, MEMF(kView + 32)};
    float f[3] = {MEMF(kView + 8), 0, MEMF(kView + 40)};
    float rl = sqrtf(r[0] * r[0] + r[2] * r[2]), fl = sqrtf(f[0] * f[0] + f[2] * f[2]);
    if (rl > 1e-4f) r[0] /= rl, r[2] /= rl;
    if (fl > 1e-4f) f[0] /= fl, f[2] /= fl;
    XBOX_GAMEPAD g = hpps_last_pad;
    float x = stick(g.sThumbLX), y = stick(g.sThumbLY);
    float up = (g.bAnalogButtons[7] - g.bAnalogButtons[6]) / 255.0f;
    float dt = MEMF(kFrameTime);
    if (!(dt > 0 && dt < 0.25f)) dt = 1 / 60.0f;
    float speed = t->fly_speed > 0 ? t->fly_speed : 600.0f;
    if (g.bAnalogButtons[0] > 64) speed *= 4;
    float k = speed * dt;
    if (x == 0 && y == 0 && up == 0) return;
    p[0] += (r[0] * x + f[0] * y) * k;
    p[1] += up * k;
    p[2] += (r[2] * x + f[2] * y) * k;
    place(obj, p);
}

static void keep_for_undo(void)
{
    if (g_undo.kept) return;
    g_undo.kept = 1;
    for (int c = 0; c < kCardCount; c++) g_undo.cards[c] = MEM16(kCards + 2 * c);
    for (int i = 0; i < kInventorySlots; i++) g_undo.inventory[i] = MEM16(kInventory + 2 * i);
    g_undo.health = MEMF(kHealth), g_undo.health_max = MEMF(kHealthMax);
    g_undo.beans = MEM16(kBeans), g_undo.beans_max = MEM16(kBeansMax);
    for (int i = 0; i < 3; i++) g_undo.slots[i] = MEM8(kSlots + i);
}

static void put_spells(const uint8_t s[3])
{
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t a[2] = {i, s[i]};
        call_guest(sub_0014FEB0, 2, a);
    }
    call_guest(sub_000342E0, 0, NULL);
}

static void undo(void)
{
    if (!g_undo.kept) return;
    for (int c = 0; c < kCardCount; c++) MEM16(kCards + 2 * c) = g_undo.cards[c];
    for (int i = 0; i < kInventorySlots; i++) MEM16(kInventory + 2 * i) = g_undo.inventory[i];
    MEMF(kHealthMax) = g_undo.health_max;
    MEMF(kHealth) = g_undo.health < g_undo.health_max ? g_undo.health : g_undo.health_max;
    MEM16(kBeansMax) = g_undo.beans_max, MEM16(kBeans) = g_undo.beans;
    put_spells(g_undo.slots);
    memset(g_given, 0, sizeof g_given);
    g_undo.kept = 0;
    fprintf(stderr, "[TRAINER] undone: back to before the first trainer change\n");
}

/* the title's health bar step backwards: the maximum down a bar (not below one), health no more than it */
static void remove_bar(void)
{
    float m = MEMF(kHealthMax) - kHealthBar;
    if (m < kHealthBar) return;
    MEMF(kHealthMax) = m;
    if (MEMF(kHealth) > m) MEMF(kHealth) = m;
}

/* a card taken away; with none left the inventory's card item (3) goes too */
static void clear_card(int c)
{
    MEM16(kCards + 2 * c) = 0;
    g_given[c] = 0;
}

static void drop_card_item_if_empty(void)
{
    for (int c = 0; c < kCardCount; c++)
        if (MEM16(kCards + 2 * c)) return;
    for (int i = 0; i < kInventorySlots; i++)
        if (MEM16(kInventory + 2 * i) == 3) MEM16(kInventory + 2 * i) = 0;
}

static int guest_ptr(uint32_t p) { return p >= 0x80000000u && p < 0x84000000u; }

/* insta-kill: the watched entities, their CurrentHealth (0 none) and its last value */
static struct {
    uint32_t ent, hp, level;
    float last;
    int is_int;
} g_watch[kMaxWatched];
static int g_nwatch;
static SRWLOCK g_watch_lock = SRWLOCK_INIT;  /* the lookup also runs on the loading thread */

static float health_value(uint32_t hp, int *is_int)
{
    uint32_t u = MEM32(hp);
    /* a float has exponent bits; a small whole number does not */
    *is_int = u != 0 && u < 0x00800000u;
    return *is_int ? (float)u : MEMF(hp);
}

static void watch_add(uint32_t e, uint32_t hp);

/* every enemy's AI looks its CurrentHealth up as it starts (sub_00025060(entity, "CurrentHealth")): the answers
 * are the ones to watch */
extern void sub_00025060_gen(void);
void sub_00025060(void)
{
    uint32_t e = ARG(0), name = ARG(1);
    sub_00025060_gen();
    if (name != kCurrentHealthName || !guest_ptr(e) || e == MEM32(kPlayer)) return;
    uint32_t hp = g_eax, inst = MEM32(e + 0x20);
    if (!guest_ptr(hp) || !guest_ptr(inst) || hp < inst || hp >= inst + 0x4000) return;
    AcquireSRWLockExclusive(&g_watch_lock);
    watch_add(e, hp);
    ReleaseSRWLockExclusive(&g_watch_lock);
}

static void watch_add(uint32_t e, uint32_t hp)
{
    for (int i = 0; i < g_nwatch; i++)
        if (g_watch[i].hp == hp) {
            g_watch[i].ent = e, g_watch[i].level = MEM32(kLevelNow);
            g_watch[i].last = health_value(hp, &g_watch[i].is_int);
            return;
        }
    if (g_nwatch == kMaxWatched) return;
    g_watch[g_nwatch].ent = e;
    g_watch[g_nwatch].level = MEM32(kLevelNow);
    g_watch[g_nwatch].hp = hp;
    g_watch[g_nwatch].last = health_value(hp, &g_watch[g_nwatch].is_int);
    g_nwatch++;
}

/* entries of another level, or whose entity is gone (freed, reused), are dropped */
static int watch_valid(int i)
{
    uint32_t e = g_watch[i].ent, hp = g_watch[i].hp, inst = MEM32(e + 0x20);
    return g_watch[i].level == MEM32(kLevelNow) && MEM32(e + 8) == e && hp >= inst && hp < inst + 0x4000;
}

static void instakill(void)
{
    AcquireSRWLockExclusive(&g_watch_lock);
    int n = 0;
    for (int i = 0; i < g_nwatch; i++)
        if (watch_valid(i)) g_watch[n++] = g_watch[i];
    g_nwatch = n;
    for (int i = 0; i < g_nwatch; i++) {
        uint32_t e = g_watch[i].ent, hp = g_watch[i].hp;
        int is_int;
        float v = health_value(hp, &is_int);
        if (v > 0 && v < g_watch[i].last) {
            MEM32(hp) = 0;  /* 0 as a float and as a whole number */
            uint32_t name = MEM32(e + 0x0C);
            fprintf(stderr, "[TRAINER] insta-kill: '%.32s' %08X health %g -> 0\n",
                    guest_ptr(name) ? (const char *)XBOX_PTR(name) : "?", e, v);
            v = 0;
        }
        g_watch[i].last = v;
    }
    ReleaseSRWLockExclusive(&g_watch_lock);
}

/* the player's entity is new (a level loaded, or a reload of the same one) */
static int seen_changed(uint32_t ent)
{
    static uint32_t last;
    if (ent == last) return 0;
    last = ent;
    return 1;
}

/* The castle hubs (dayN, nightN) load the shared hogday.hog / hognight.hog (sub_001409F0); the levels entered from
 * a hub (the challenges, the library, the forest, the night 5 rooms) load neither and expect the hub's to be in:
 * night3library straight after a day level crashes building its objects. Such a level is reached through its hub
 * when the archive in is not that hub's kind. */
static uint32_t hub_of(uint32_t level)
{
    static const uint8_t hub[HPPS_LEVEL_LAST + 1] = {
        [6] = 6, [7] = 7, [8] = 8, [9] = 8, [10] = 10, [11] = 11, [12] = 11, [13] = 13, [14] = 13, [15] = 15,
        [16] = 15, [17] = 17, [18] = 17, [19] = 19, [20] = 19, [21] = 21, [22] = 21, [23] = 23, [24] = 23,
        [25] = 25, [26] = 25, [27] = 25, [28] = 25, [29] = 25, [30] = 25,
    };
    return level <= HPPS_LEVEL_LAST ? hub[level] : 0;
}

static int is_night(uint32_t level)
{
    const char *name = (const char *)XBOX_PTR(MEM32(0x21BB70 + 4 * (level - HPPS_LEVEL_FIRST)));
    return strncmp(name, "night", 5) == 0;
}

static uint32_t g_hub_in;   /* the last hub played: whose shared archive is in (0 none known) */
static uint32_t g_pending;  /* a level to load once its hub is up */
static int g_quidditch;     /* day5 asked for as level 24: start the match in it (2: day5's new Harry is up) */

static void load_level(uint32_t level);

/* the level object's script of that name (sub_00025360's search, the name from here), or 0 */
static uint32_t level_script(const char *name)
{
    uint32_t lo = MEM32(kLevelObject), set = lo ? MEM32(lo + 0x1C) : 0;
    if (!set) return 0;
    uint32_t n = MEM32(set + 0x0C), tab = MEM32(set + 0x10);
    for (uint32_t i = 0; i < n && i < 4096; i++) {
        uint32_t s = MEM32(tab + 0x34 * i);
        if (s && !_stricmp((const char *)XBOX_PTR(s), name)) return tab + 0x34 * i;
    }
    return 0;
}

/* starts the level object's script of that name, as the title's own code does */
static void start_script(const char *name)
{
    uint32_t script = level_script(name);
    if (!script) {
        fprintf(stderr, "[TRAINER] level %u has no script %s\n", MEM32(kLevelNow), name);
        return;
    }
    uint32_t a[2] = {MEM32(kLevelObject), script};
    call_guest(sub_0014D1C0, 2, a);
    fprintf(stderr, "[TRAINER] %s started (script %08X)\n", name, script);
}

/* in day5 with the match asked for: HPE53Script once Harry has had a second in the level (its Startup is done) */
static void quidditch_start(uint32_t level)
{
    static ULONGLONG since;
    if (g_quidditch != 2 || level != kDay5 || g_pending) {
        since = 0;
        return;
    }
    if (!since) since = GetTickCount64();
    if (GetTickCount64() - since < 1000) return;
    g_quidditch = 0;
    start_script("HPE53Script");
}

static void load_level_via_hub(uint32_t level)
{
    if (level < HPPS_LEVEL_FIRST || level > HPPS_LEVEL_LAST) return;
    g_quidditch = level == kQuidditch;
    if (g_quidditch) {
        fprintf(stderr, "[TRAINER] level %u is day5's Quidditch match: day5, then its HPE53Script\n", level);
        level = kDay5;
    }
    uint32_t hub = hub_of(level);
    if (hub != level && !(g_hub_in && is_night(g_hub_in) == is_night(hub))) {
        fprintf(stderr, "[TRAINER] level %u needs its hub's archive: hub %u first\n", level, hub);
        g_pending = level;
        load_level(hub);
        return;
    }
    load_level(level);
}

static void load_level(uint32_t level)
{
    if (level < HPPS_LEVEL_FIRST || level > HPPS_LEVEL_LAST) return;
    /* the level in play again: 5, as StartLevel (sub_00048FB0) asks for it; its own id loads it over itself and
     * the load crashes */
    if (level == MEM32(kLevelNow)) level = 5;
    uint32_t a[1] = {level};
    call_guest(sub_00146110, 1, a);
    fprintf(stderr, "[TRAINER] loading level %u\n", level);
}

static void snapshot(HppsTrainerShm *t, uint32_t level)
{
    for (uint32_t i = 0; i < HPPS_PROGRESS_SIZE; i++) t->snap_block[i] = MEM8(HPPS_PROGRESS_VA + i);
    t->snap_level = level;
    t->snap_seq++;
}

/* ── self-driven test runs ───────────────────────────────────────────────────
 * RECOMP_BOOT_CHECKPOINT=<file>  on Harry's first frame in any level: the checkpoint's progress is put back and
 *                                its level loaded (through its hub), as Restore does
 * RECOMP_AUTOPLAY=<file>         lines "<seconds> <command> [args]", the seconds counted from Harry's first frame
 *                                in the checkpoint's level (or in any level without one):
 *                                  god 0|1, noclip 0|1, instakill 0|1, tp x y z, capture, level <id>,
 *                                  dump <dir> (guest memory and progress, as the trainer's report), quit,
 *                                  captures <seconds> <count> (timed captures from a thread: cutscenes run no Harry),
 *                                  script <name> (start one of the level's scripts),
 *                                  diffsweep (start / end a difftest sweep, as F5: the HPPS_DIFFTEST build),
 *                                  quitafter <seconds> (from a thread)
 * Every command is logged as [AUTO]. */
extern void hpps_RequestCapture(void);
/* difftest.c, in the HPPS_DIFFTEST build only */
extern void hpps_DiffSweep(void) __attribute__((weak));

static struct {
    int state;  /* 0 not read yet, 1 waiting for the menu, 2 done, -1 none */
    uint32_t level;
    uint8_t block[HPPS_PROGRESS_SIZE];
} g_boot;

static void boot_read(void)
{
    g_boot.state = -1;
    const char *path = getenv("RECOMP_BOOT_CHECKPOINT");
    if (!path || !*path) return;
    FILE *f = fopen(path, "rb");
    uint32_t hd[4];
    if (f && fread(hd, sizeof hd, 1, f) == 1 && hd[0] == 0x4B435048u && hd[3] == HPPS_PROGRESS_SIZE &&
        fread(g_boot.block, 1, HPPS_PROGRESS_SIZE, f) == HPPS_PROGRESS_SIZE && hd[2] >= HPPS_LEVEL_FIRST &&
        hd[2] <= HPPS_LEVEL_LAST) {
        g_boot.level = hd[2];
        g_boot.state = 1;
        fprintf(stderr, "[AUTO] boot checkpoint %s: level %u\n", path, g_boot.level);
    } else {
        fprintf(stderr, "[AUTO] boot checkpoint %s: not a checkpoint\n", path);
    }
    if (f) fclose(f);
}

static void load_level_via_hub(uint32_t level);

/* the main menu's per-frame callback (the movie player's, sub_00164450's fourth argument) */
extern void sub_00023BD0_gen(void);
void sub_00023BD0(void)
{
    /* a level asked for from here loses to the title's own way on (it continues to the last save): the checkpoint
     * goes in on Harry's first frame instead (autoplay) */
    sub_00023BD0_gen();
}

typedef struct {
    float at;
    char cmd[16];
    char arg[240];
    int done;
} AutoStep;
static AutoStep g_auto[128];
static int g_nauto = -1;

static void auto_read(void)
{
    g_nauto = 0;
    const char *path = getenv("RECOMP_AUTOPLAY");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    char line[300];
    while (f && g_nauto < 128 && fgets(line, sizeof line, f)) {
        AutoStep *a = &g_auto[g_nauto];
        memset(a, 0, sizeof *a);
        if (line[0] == '#' || sscanf(line, "%f %15s %239[^\r\n]", &a->at, a->cmd, a->arg) < 2) continue;
        g_nauto++;
    }
    if (f) fclose(f);
    fprintf(stderr, "[AUTO] %d step(s) from %s\n", g_nauto, path);
}

static void dump_memory(const char *dir)
{
    char path[320];
    CreateDirectoryA(dir, NULL);
    static const struct { uint32_t va, size; const char *name; } parts[] = {
        {0x00000000u, 0x04000000u, "guest_00000000.bin"},
        {0x80000000u, 0x04000000u, "guest_80000000.bin"},
        {HPPS_PROGRESS_VA, HPPS_PROGRESS_SIZE, "progress_3253B0.bin"},
    };
    static uint8_t page[65536];
    for (int k = 0; k < 3; k++) {
        snprintf(path, sizeof path, "%s\\%s", dir, parts[k].name);
        FILE *f = fopen(path, "wb");
        if (!f) continue;
        for (uint32_t o = 0; o < parts[k].size; o += sizeof page) {
            uint32_t n = parts[k].size - o < sizeof page ? parts[k].size - o : sizeof page;
            SIZE_T r = 0;
            memset(page, 0, n);
            /* holes read as zero */
            if (!ReadProcessMemory(GetCurrentProcess(), (void *)XBOX_PTR(parts[k].va + o), page, n, &r))
                for (uint32_t q = 0; q < n; q += 4096)
                    ReadProcessMemory(GetCurrentProcess(), (void *)XBOX_PTR(parts[k].va + o + q), page + q, 4096, &r);
            fwrite(page, 1, n, f);
        }
        fclose(f);
    }
    fprintf(stderr, "[AUTO] memory written to %s\n", dir);
}

static void place(uint32_t obj, const float p[3]);

static float g_cap_every, g_quit_after;
static int g_cap_count;
static DWORD WINAPI timed_thread(LPVOID unused)
{
    (void)unused;
    ULONGLONG t0 = GetTickCount64(), next = 0;
    for (;;) {
        Sleep(50);
        ULONGLONG now = GetTickCount64();
        /* the first one an interval after "captures" (the thread may have started earlier, for "quitafter") */
        if (g_cap_count > 0 && !next) next = now + (ULONGLONG)(g_cap_every * 1000);
        if (g_cap_count > 0 && now >= next) {
            g_cap_count--;
            next += (ULONGLONG)(g_cap_every * 1000);
            hpps_RequestCapture();
            fprintf(stderr, "[AUTO] timed capture at %.2f s\n", (now - t0) / 1000.0);
        }
        if (g_quit_after > 0 && now - t0 >= (ULONGLONG)(g_quit_after * 1000)) {
            fflush(stdout), fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
    return 0;
}

static void autoplay(HppsTrainerShm *t, uint32_t level, uint32_t obj)
{
    /* in a level before the menu got to it (the title went on to the last save by itself): put it back now */
    if (!g_boot.state) boot_read();
    if (g_boot.state == 1) {
        g_boot.state = 2;
        for (uint32_t i = 0; i < HPPS_PROGRESS_SIZE; i++) MEM8(HPPS_PROGRESS_VA + i) = g_boot.block[i];
        fprintf(stderr, "[AUTO] progress from the checkpoint (in level %u), loading level %u\n", level, g_boot.level);
        load_level_via_hub(g_boot.level);
        return;
    }
    if (g_nauto < 0) auto_read();
    if (!g_nauto) return;
    static ULONGLONG start;
    if (!start) {
        /* the checkpoint's level, reached after its hub (a new Harry) */
        if (g_boot.state == 2 && (level != g_boot.level || g_pending)) return;
        start = GetTickCount64();
        fprintf(stderr, "[AUTO] clock starts in level %u\n", level);
    }
    float now = (GetTickCount64() - start) / 1000.0f;
    for (int i = 0; i < g_nauto; i++) {
        AutoStep *a = &g_auto[i];
        if (a->done || a->at > now) continue;
        a->done = 1;
        fprintf(stderr, "[AUTO] %7.2f s %s %s\n", now, a->cmd, a->arg);
        if (!strcmp(a->cmd, "god")) t->god = atoi(a->arg);
        else if (!strcmp(a->cmd, "noclip")) t->noclip = atoi(a->arg);
        else if (!strcmp(a->cmd, "instakill")) t->instakill = atoi(a->arg);
        else if (!strcmp(a->cmd, "capture")) hpps_RequestCapture();
        else if (!strcmp(a->cmd, "level")) load_level_via_hub((uint32_t)atoi(a->arg));
        else if (!strcmp(a->cmd, "dump")) dump_memory(a->arg);
        else if (!strcmp(a->cmd, "script")) start_script(a->arg);
        else if (!strcmp(a->cmd, "diffsweep") && hpps_DiffSweep) hpps_DiffSweep();
        else if (!strcmp(a->cmd, "captures") || !strcmp(a->cmd, "quitafter")) {
            static int started;
            if (a->cmd[0] == 'c') sscanf(a->arg, "%f %d", &g_cap_every, &g_cap_count);
            else g_quit_after = (float)atof(a->arg);
            if (!started++) CreateThread(NULL, 0, timed_thread, NULL, 0, NULL);
        }
        else if (!strcmp(a->cmd, "tp") && obj) {
            float p[3];
            if (sscanf(a->arg, "%f %f %f", &p[0], &p[1], &p[2]) == 3) place(obj, p);
        } else if (!strcmp(a->cmd, "quit")) {
            fflush(stdout), fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
}

extern void sub_00095370_gen(void);
/* debug (RECOMP_SPELL_PROBE=1): the spell buttons (0x329DD5..7) and the bytes round them as they change, to tell
 * which slot is which button and where the learnt spells are kept (2026-11-01) */
static void spell_probe(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_SPELL_PROBE") && *getenv("RECOMP_SPELL_PROBE") != '0';
    if (!on) return;
    static uint8_t last[64];
    static int first = 1;
    uint8_t now[64];
    for (int i = 0; i < 64; i++) now[i] = MEM8(0x329DC0 + i);
    if (!first && !memcmp(now, last, sizeof now)) return;
    first = 0;
    char line[64 * 3 + 1];
    for (int i = 0; i < 64; i++) sprintf(line + 3 * i, "%02X ", now[i]);
    fprintf(stderr, "[SPELLS] buttons %u %u %u | 329DC0: %s\n", now[0x15], now[0x16], now[0x17], line);
    memcpy(last, now, sizeof now);
}

/* [HARRY] (always on, a line only when something changes, as CoS's [HDIFF]): Harry's state ([harry+0xDC]+0x2564, the
 * one [CRAB] reads) at each change, with his position and the pad; and "stuck" when the left stick has been held
 * for over a second while he moved less than a unit, once a spell and again when he moves. For the ledge where
 * pushing through the wall's knock-off animation left Harry locked (2026-11-02): what state he is held in */
static void harry_trace(uint32_t ent)
{
    static uint32_t last_ent, last_state = 0xFFFFFFFFu, lines;
    static float anchor[3];
    static ULONGLONG held_since;
    static int stuck_told;
    uint32_t ctrl = MEM32(ent + 0xDC), obj = MEM32(ent + 0x28);
    if (!ctrl || !obj || lines > 20000) return;
    uint32_t cur = current_matrix(obj), state = MEM32(ctrl + 0x2564);
    float p[3] = {MEMF(cur + 48), MEMF(cur + 52), MEMF(cur + 56)};
    XBOX_GAMEPAD g = hpps_last_pad;
    ULONGLONG now = GetTickCount64();
    if (ent != last_ent) last_ent = ent, last_state = 0xFFFFFFFFu, held_since = 0;
    if (state != last_state) {
        fprintf(stderr, "[HARRY] %9.2f s state 0x%X -> 0x%X at %.1f %.1f %.1f | pad %04X stick %d %d | level %u\n",
                now / 1000.0, last_state, state, p[0], p[1], p[2], g.wButtons, g.sThumbLX, g.sThumbLY,
                MEM32(kLevelNow));
        last_state = state;
        lines++;
    }
    int pushing = g.sThumbLX * g.sThumbLX + g.sThumbLY * g.sThumbLY > 12000 * 12000;
    float dx = p[0] - anchor[0], dy = p[1] - anchor[1], dz = p[2] - anchor[2];
    int moved = dx * dx + dy * dy + dz * dz > 1.0f;
    if (!pushing || moved || !held_since) {
        if (stuck_told && (moved || !pushing)) {
            fprintf(stderr, "[HARRY] %9.2f s free again (%s), state 0x%X at %.1f %.1f %.1f\n", now / 1000.0,
                    moved ? "moved" : "stick let go", state, p[0], p[1], p[2]);
            lines++;
        }
        stuck_told = 0;
        anchor[0] = p[0], anchor[1] = p[1], anchor[2] = p[2];
        held_since = pushing ? now : 0;
        return;
    }
    if (!stuck_told && now - held_since > 1000) {
        stuck_told = 1;
        lines++;
        fprintf(stderr, "[HARRY] %9.2f s stuck: stick %d %d held %.1f s, moved under a unit, state 0x%X at %.1f %.1f "
                "%.1f | pad %04X | controller %08X\n", now / 1000.0, g.sThumbLX, g.sThumbLY, (now - held_since) / 1000.0,
                state, p[0], p[1], p[2], g.wButtons, ctrl);
    }
}

void sub_00095370(void)
{
    HppsTrainerShm *t = shm();
    uint32_t ent = ARG(0);
    if (ent && ent == MEM32(kPlayer)) spell_probe(), harry_trace(ent);
    if (!t || ent != MEM32(kPlayer) || !ent) {
        sub_00095370_gen();
        return;
    }
    static uint32_t seen;
    if (seen != ent) {
        seen = ent;
        fprintf(stderr, "[TRAINER] Harry's update: entity %08X object %08X\n", ent, MEM32(ent + 0x28));
    }
    uint32_t obj = MEM32(ent + 0x28);

    /* the day5 the match was asked for, not the one it may have been asked from: Harry's first update after a
     * load (none run while one does; the reloaded day5 can keep Harry's entity address) */
    static ULONGLONG last_update;
    ULONGLONG now_ms = GetTickCount64();
    if (g_quidditch == 1 && last_update && now_ms - last_update > 500 && MEM32(kLevelNow) == kDay5 && !g_pending)
        g_quidditch = 2;
    last_update = now_ms;

    /* a new level: forget the watched enemies, hand the trainer a checkpoint */
    static uint32_t level_seen, snap_now_done, warp_done, restore_done;
    uint32_t level = MEM32(kLevelNow);
    if (level != level_seen || seen_changed(ent)) {
        level_seen = level;
        if (level >= HPPS_LEVEL_FIRST && level <= HPPS_LEVEL_LAST) {
            if (hub_of(level) == level) g_hub_in = level;
            snapshot(t, level);
            /* already in the level a hub was asked for on the way to (the title got there itself) */
            if (g_pending == level) g_pending = 0;
            /* the hub a level needed is up: on to that level */
            if (g_pending && hub_of(g_pending) == level) {
                uint32_t to = g_pending;
                g_pending = 0;
                load_level(to);
            }
        }
    }
    t->level = level;
    if (t->snap_now_seq != snap_now_done) {
        snap_now_done = t->snap_now_seq;
        snapshot(t, level);
    }
    if (t->restore_seq != restore_done) {
        restore_done = t->restore_seq;
        for (uint32_t i = 0; i < HPPS_PROGRESS_SIZE; i++) MEM8(HPPS_PROGRESS_VA + i) = t->restore_block[i];
        fprintf(stderr, "[TRAINER] checkpoint restored\n");
        load_level_via_hub(t->restore_level);
    } else if (t->warp_seq != warp_done) {
        warp_done = t->warp_seq;
        load_level_via_hub(t->warp_level);
    }
    if (t->instakill) instakill();
    quidditch_start(level);
    autoplay(t, level, obj);

    if (t->god && MEMF(kHealthMax) > 0) MEMF(kHealth) = MEMF(kHealthMax);
    static uint32_t slots_done, tp_done;
    if (t->slots_seq != slots_done) {
        slots_done = t->slots_seq;
        keep_for_undo();
        uint8_t req[3] = {t->req_slots[0], t->req_slots[1], t->req_slots[2]};
        put_spells(req);
        fprintf(stderr, "[TRAINER] spells on the buttons: %u %u %u\n", MEM8(kSlots), MEM8(kSlots + 1),
                MEM8(kSlots + 2));
    }
    static uint32_t cards_done, bar_done, bars_max_done, beans_done, bag_done;
    if (t->cards_seq != cards_done) {
        cards_done = t->cards_seq;
        keep_for_undo();
        int n = 0;
        for (uint32_t c = 1; c <= kCardCount; c++) {
            if (MEM16(kCards + 2 * (c - 1))) continue;
            uint32_t a[1] = {c};
            call_guest(sub_0003E9A0, 1, a);
            g_given[c - 1] = 1;
            n++;
        }
        fprintf(stderr, "[TRAINER] %d wizard cards given, health max %.0f\n", n, MEMF(kHealthMax));
    }
    if (t->bar_seq != bar_done) {
        bar_done = t->bar_seq;
        keep_for_undo();
        call_guest(sub_00152160, 0, NULL);
    }
    if (t->bars_max_seq != bars_max_done) {
        bars_max_done = t->bars_max_seq;
        keep_for_undo();
        for (int i = 0; i < 8 && MEMF(kHealthMax) < kHealthCap; i++) call_guest(sub_00152160, 0, NULL);
    }
    if (t->bag_seq != bag_done) {
        bag_done = t->bag_seq;
        keep_for_undo();
        if ((int16_t)MEM16(kBeansMax) < kBigBag) MEM16(kBeansMax) = kBigBag;
    }
    if (t->beans_seq != beans_done) {
        beans_done = t->beans_seq;
        keep_for_undo();
        MEM16(kBeans) = MEM16(kBeansMax);
    }
    static uint32_t given_rm_done, all_rm_done, bar_rm_done, beans_set_done, bag_small_done, undo_done;
    if (t->cards_given_remove_seq != given_rm_done) {
        given_rm_done = t->cards_given_remove_seq;
        int n = 0;
        for (int c = 0; c < kCardCount; c++)
            if (g_given[c]) clear_card(c), n++;
        drop_card_item_if_empty();
        fprintf(stderr, "[TRAINER] %d given cards taken back\n", n);
    }
    if (t->cards_all_remove_seq != all_rm_done) {
        all_rm_done = t->cards_all_remove_seq;
        keep_for_undo();
        for (int c = 0; c < kCardCount; c++) clear_card(c);
        drop_card_item_if_empty();
    }
    if (t->bar_remove_seq != bar_rm_done) {
        bar_rm_done = t->bar_remove_seq;
        keep_for_undo();
        remove_bar();
    }
    if (t->beans_set_seq != beans_set_done) {
        beans_set_done = t->beans_set_seq;
        keep_for_undo();
        int v = t->beans_value, cap = (int16_t)MEM16(kBeansMax);
        MEM16(kBeans) = (uint16_t)(v < 0 ? 0 : v > cap ? cap : v);
    }
    if (t->bag_small_seq != bag_small_done) {
        bag_small_done = t->bag_small_seq;
        keep_for_undo();
        MEM16(kBeansMax) = kSmallBag;
        if ((int16_t)MEM16(kBeans) > kSmallBag) MEM16(kBeans) = kSmallBag;
    }
    if (t->undo_seq != undo_done) {
        undo_done = t->undo_seq;
        undo();
    }
    if (t->tp_seq != tp_done) {
        tp_done = t->tp_seq;
        float p[3] = {t->tp_pos[0], t->tp_pos[1], t->tp_pos[2]};
        if (obj) place(obj, p);
    }

    int noclip = t->noclip && obj;
    if (noclip) {
        fly(obj, t);
        g_esp += 4;  /* the update's ret */
    } else {
        sub_00095370_gen();
    }

    t->noclip_on = (uint8_t)noclip;
    t->player = ent;
    if (obj) {
        uint32_t cur = current_matrix(obj);
        t->pos[0] = MEMF(cur + 48), t->pos[1] = MEMF(cur + 52), t->pos[2] = MEMF(cur + 56);
    }
    t->health = MEMF(kHealth), t->health_max = MEMF(kHealthMax);
    for (int i = 0; i < 3; i++) t->slots[i] = MEM8(kSlots + i);
    uint16_t cards = 0;
    for (int c = 0; c < kCardCount; c++) cards += MEM16(kCards + 2 * c) != 0;
    t->cards = cards;
    uint16_t given = 0;
    for (int c = 0; c < kCardCount; c++) given += g_given[c];
    t->cards_given = given;
    t->undo_ready = (uint8_t)g_undo.kept;
    t->beans = (int16_t)MEM16(kBeans), t->beans_max = (int16_t)MEM16(kBeansMax);
    t->frame++;
}

static void write_state_to(FILE *f, HppsTrainerShm *t)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "time %04u-%02u-%02u %02u:%02u:%02u, %.1f s since boot\n", st.wYear, st.wMonth, st.wDay, st.wHour,
            st.wMinute, st.wSecond, GetTickCount64() / 1000.0);
    fprintf(f, "guest -> host offset %+lld\n", (long long)g_xbox_mem_offset);
    fprintf(f, "Harry's updates %u (last player %08X)%s\n", t->frame, t->player, t->noclip_on ? ", noclip" : "");
    uint32_t pl = MEM32(kPlayer);
    fprintf(f, "player entity [0x3565B0] %08X", pl);
    if (pl) {
        uint32_t obj = MEM32(pl + 0x28);
        fprintf(f, " name '%.15s' object %08X controller %08X", (const char *)XBOX_PTR(MEM32(pl + 0x0C)), obj,
                MEM32(pl + 0xDC));
        if (MEM32(pl + 0xDC)) fprintf(f, " state 0x%X", MEM32(MEM32(pl + 0xDC) + 0x2564));
        if (obj) {
            uint32_t cur = current_matrix(obj);
            fprintf(f, " position %.2f %.2f %.2f", MEMF(cur + 48), MEMF(cur + 52), MEMF(cur + 56));
        }
    }
    fprintf(f, "\nlevel word [0x329E6E] %d\n", (int16_t)MEM16(kLevel));
    fprintf(f, "health %.1f / %.1f, beans %d / %d, cards %u (trainer gave %u)\n", MEMF(kHealth), MEMF(kHealthMax),
            (int16_t)MEM16(kBeans), (int16_t)MEM16(kBeansMax), t->cards, t->cards_given);
    fprintf(f, "spell buttons %u %u %u\ninventory slots", MEM8(kSlots), MEM8(kSlots + 1), MEM8(kSlots + 2));
    for (int i = 0; i < kInventorySlots; i++) fprintf(f, " %d", (int16_t)MEM16(kInventory + 2 * i));
    XBOX_GAMEPAD g = hpps_last_pad;
    fprintf(f, "\npad buttons %04X analog", g.wButtons);
    for (int i = 0; i < 8; i++) fprintf(f, " %u", g.bAnalogButtons[i]);
    fprintf(f, " sticks %d %d %d %d\n", g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY);
    fprintf(f, "trainer: noclip %u god %u insta-kill %u undo kept %d, level now %u (%d watched)\n", t->noclip, t->god,
            t->instakill, g_undo.kept, MEM32(kLevelNow), g_nwatch);
}

static void write_state(const char *path, HppsTrainerShm *t)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    write_state_to(f, t);
    fclose(f);
}

/* F10 > Debug > Mark this moment: the trainer report's game state (Harry's entity, position and controller, the
 * level, health, inventory, the pad) into the run log under the mark, so "stuck here" comes with where and how */
void hpps_MarkState(int mark)
{
    HppsTrainerShm *t = shm();
    fprintf(stderr, "[MARK] %d state:\n", mark);
    if (t) write_state_to(stderr, t);
    else fprintf(stderr, "  (no trainer state: shared memory unavailable)\n");
    fprintf(stderr, "[MARK] %d state end\n", mark);
    fflush(stderr);
}

static DWORD WINAPI report_thread(LPVOID arg)
{
    HppsTrainerShm *t = (HppsTrainerShm *)arg;
    for (;; Sleep(100)) {
        uint32_t seq = t->report_seq;
        if (seq == t->report_done) continue;
        char dir[260], path[300];
        memcpy(dir, (const char *)t->report_dir, sizeof dir);
        dir[sizeof dir - 1] = 0;
        hpps_RequestCapture();
        snprintf(path, sizeof path, "%s\\stacks.txt", dir);
        hpps_StackDumpTo(path);
        snprintf(path, sizeof path, "%s\\game_state.txt", dir);
        write_state(path, t);
        fprintf(stderr, "[TRAINER] debug report written to %s\n", dir);
        t->report_done = seq;
    }
    return 0;
}

/* Debug, F6 (toggle): Harry kept at the camera. Cutscenes run no Harry update, so this thread writes the position
 * into both of his object's world matrices. A test of whether faces in a cutscene depend on how far they are from
 * Harry (2026-10-20: eyelids and hair wrong in the Quidditch cutscenes, where Harry is far off on his broom). */
static DWORD WINAPI __attribute__((unused)) harry_cam_thread(LPVOID unused)
{
    (void)unused;
    int on = 0;
    float home[3] = {0, 0, 0};
    uint32_t home_obj = 0;
    for (;;) {
        Sleep(20);
        uint32_t harry = MEM32(kPlayer), obj = guest_ptr(harry) ? MEM32(harry + 0x28) : 0;
        if (obj && !guest_ptr(obj)) obj = 0;
        if (GetAsyncKeyState(VK_F6) & 1) {
            on = !on;
            if (on && obj) {
                home_obj = obj;
                for (int i = 0; i < 3; i++) home[i] = MEMF(obj + 0x40 * (MEM32(obj + 0x2C) + 1) + 0x30 + 4 * i);
            } else if (!on && home_obj && home_obj == obj) {
                for (int k = 1; k <= 2; k++)
                    for (int i = 0; i < 3; i++) MEMF(obj + 0x40 * k + 0x30 + 4 * i) = home[i];
            }
            fprintf(stderr, "[F6] Harry at the camera: %s (Harry was at %.0f %.0f %.0f)\n", on ? "on" : "off",
                    home[0], home[1], home[2]);
        }
        if (!on || !obj || obj != home_obj) continue;
        /* camera position from the view matrix (row vectors): c = -t * R^T */
        float v[16], c[3];
        for (int i = 0; i < 16; i++) v[i] = MEMF(kView + 4 * i);
        for (int j = 0; j < 3; j++) c[j] = -(v[12] * v[j * 4 + 0] + v[13] * v[j * 4 + 1] + v[14] * v[j * 4 + 2]);
        for (int k = 1; k <= 2; k++)
            for (int i = 0; i < 3; i++) MEMF(obj + 0x40 * k + 0x30 + 4 * i) = c[i];
    }
    return 0;
}

/* at boot, so the trainer finds the block before Harry exists */
void hpps_TrainerInit(void)
{
    HppsTrainerShm *t = shm();
#ifndef HPPS_RELEASE
    CloseHandle(CreateThread(NULL, 0, harry_cam_thread, NULL, 0, NULL));  /* debug key, not for players */
#endif
    if (!t) return;
    fprintf(stderr, "[TRAINER] shared block %s ready\n", HPPS_TRAINER_NAME);
    CloseHandle(CreateThread(NULL, 0, report_thread, t, 0, NULL));
}

/* A bean's flavour as Harry picks it up (F10 Enhancements, "Announce bean flavours on pickup"). The pickup (0x41E52:
 * the bean count [0x329B8A] up by one, within the bag) has sub_000116B0 pick one of 17 message names at random
 * (0x31F320 + 32 * n, copied at start from GUI_PICKUP_BEANn / SV1_PICKUP_BEANn) and hands it to sub_0004F6B0, the
 * game's message by name: the flavour's text, or Harry's line with its subtitle. Off, that one call (its return
 * address 0x41E8B) returns at once; the bean is counted as before and every other message is untouched */
extern void sub_0004F6B0_gen(void);
extern int hpps_BeanNames(void);
void sub_0004F6B0(void)
{
    if (MEM32(g_esp) == 0x00041E8Bu && !hpps_BeanNames()) {
        g_esp += 4;  /* the return address the call pushed; the caller takes its two arguments off */
        return;
    }
    sub_0004F6B0_gen();
}
