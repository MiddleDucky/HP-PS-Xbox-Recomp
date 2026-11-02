/**
 * "Quit To Desktop" on the main menu and the pause menu: on a PC the desktop stands in for the dashboard.
 *
 * The main menu's input is sub_0001F200 and its drawing sub_0001C380: four rows (New Game, Continue, Options,
 * Extras -- shown as Language on the Xbox), the chosen one in [0x325334] (0..3, wrapping). Row i is drawn at
 * sub_00168D70(x, y) = (400 - 15 i, 295 + 37 i) -- (392 - 15 i, 310 + 33 i) when [0x32E210] is 2 (11 changes y
 * only) -- in a colour and scale that ease toward their targets each frame (lit: white, growing to 1.4 / 1.15;
 * others bluer the further they are). A fifth row, index 4, goes after them and leaves the way the title's own
 * dashboard launches do: XLaunchNewImage(NULL, NULL) (sub_00153FC4) -> HalReturnToFirmware, which ends the
 * process.
 *
 * The four rows move up a row (sub_00168D70 while the menu draws) so the five end where the four did, above the
 * copyright line. While the fifth row is chosen the title's input step does not run; down from the last row or up
 * from the first picks it, as the title's own wrap would. Choosing it asks first: the rows (the text draw
 * sub_00169A40, a printf) give way to "Quit To Desktop?" over the game's GUI_YES / GUI_NO in the rows' style,
 * No lit; up / down picks, A or START answers, B (as the options' accept-changes screen, 0x1FB80) is No.
 *
 * The pause menu is input sub_000307F0 and drawing sub_00030F00, state [0x32E638] (2 the rows, 0x10 the "sure
 * quit" box with Yes / No in [0x32E7F8], left / right toggles, A answers); rows 0..9 from the key table at
 * 0x211720 (GUI_QUIT the last), chosen in [0x32E644], wrapping; row i at y 90 + 32 i (the 32.0 at 0x1FB530, which
 * nothing else in the drawing reads) in an easing colour (lit white; else r 0.2 - d/60, g 0.64 - 0.05 d,
 * b 0.99 - 0.04 d, a 0.95 - 0.05 d) and scale (4 dt a frame, to 1.4 / 1.2 in mode 11), and a "select" hint at
 * (320, 416). Row 10, "Quit To Desktop", goes below GUI_QUIT the way the main menu's fifth row does (the input
 * step's own jump table ignores an index past 9; up from it is the step's own decrement); the rows close up to a
 * 30 step while the pause draws and the hint moves to 430, clear of it, its button sprite (sub_00030510's
 * sub_00169F70, 270-318 x 390-438 for the A) down with it. Choosing it opens the game's own box,
 * titled "Quit To Desktop?"; its Yes ends the process, No goes back to the row.
 *
 * Built into the recompile with --exclude-manual port/src/hle_frontend.c (regen.sh); until a regen the generated
 * bodies of sub_0001F200, sub_0001C380, sub_000307F0, sub_00030F00, sub_00030510 (recomp_0000.c), sub_00168D70,
 * sub_00169A40 and sub_00169F70 (recomp_0004.c) are renamed _gen by hand. RECOMP_QUIT_ITEM=0 leaves both menus as they were.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

#define ARG(i) MEM32(g_esp + 4 + 4 * (i))

enum {
    kRow = 0x325334, kRows = 4, kQuitRow = 4, kMode = 0x32E210, kBusy = 0x32536D,
    kPadState = 0x325364, kDown = 0x356BD0, kStickY = 0x356BA8, kUp = 0x356BB0, kConfirmA = 0x356C14,
    kConfirmB = 0x356BE4, kBack = 0x356C74, kSndMove = 0x322DA0, kSndConfirm = 0x322D9C, kSndBack = 0x322DA4,
    kFrameTime = 0x322D48, kKeyYes = 0x1FB580, kKeyNo = 0x1FB578,
    kPState = 0x32E638, kPList = 2, kPSure = 0x10, kPRow = 0x32E644, kPYes = 0x32E7F8, kPDt = 0x32E62C,
    kPRows = 10, kPQuitRow = 10, kPStep = 0x1FB530, kKeySelect = 0x2118E0,
};

extern void sub_0001F200_gen(void);
extern void sub_0001C380_gen(void);
extern void sub_000307F0_gen(void);
extern void sub_00030F00_gen(void);
extern void sub_00168D70_gen(void);
extern void sub_00169A40_gen(void);
extern void sub_00030510_gen(void);
extern void sub_00169F70_gen(void);
extern void sub_00168D90(void);
extern void sub_00168E60(void);
extern void sub_00168DC0(void);
extern void sub_00168DF0(void);
extern void sub_00163AF0(void);
extern void sub_00163BF0(void);
extern void sub_0002A7C0(void);
extern void sub_0002A9F0(void);
extern void sub_000306C0(void);
extern void sub_00030510(void);
extern void sub_00153FC4(void);
extern void __stdcall xbox_HalReturnToFirmware(unsigned long routine);
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

static int g_in_menu;               /* the title's rows are being drawn: they move up a row */
static float g_col[4] = {0.2f, 0.64f, 0.99f, 0.95f}, g_scale = 1.0f;  /* the fifth row's easing colour / scale */
static int g_ask, g_ask_yes;        /* the main menu asks; Yes is lit */
static float g_ask_col[2][4] = {{0.2f, 0.64f, 0.99f, 0.95f}, {0.2f, 0.64f, 0.99f, 0.95f}}, g_ask_scale[2] = {1, 1};
static int g_pause_draw;            /* the pause menu is drawing */
static int g_pause_ask;             /* the pause menu's box is up for row 10 */
static int g_pause_hint;            /* the pause menu's hint is drawing its button */
static float g_pcol[4] = {0.2f, 0.64f, 0.99f, 0.95f}, g_pscale = 1.0f;  /* row 10's easing colour / scale */
static float g_last_x, g_last_y;    /* the last text position the game set, as it set it */

static int enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_QUIT_ITEM");
        on = !(e && *e == '0');
    }
    return on;
}

/* a recompiled function on this guest thread, arguments pushed right to left and a return address; esp and the
 * callee-saved registers as before whoever pops */
static uint32_t call_guest(void (*fn)(void), int n, const uint32_t *args)
{
    uint32_t s_esp = g_esp, s_ebx = g_ebx, s_esi = g_esi, s_edi = g_edi, s_ebp = g_ebp;
    uint32_t sp = s_esp;
    for (int i = n; i-- > 0;) {
        sp -= 4;
        MEM32(sp) = args[i];
    }
    sp -= 4;
    MEM32(sp) = 0;
    g_esp = sp;
    fn();
    g_esp = s_esp, g_ebx = s_ebx, g_esi = s_esi, g_edi = s_edi, g_ebp = s_ebp;
    return g_eax;
}

static uint32_t fb(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static float bf(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* the pad's per-button record the title's menus read (word +0x10 of the 24-byte entry the index names) */
static int16_t pad(uint32_t index_var)
{
    uint32_t tbl = MEM32(MEM32(kPadState) + 0x10);
    return (int16_t)MEM16(tbl + MEM32(index_var) * 24 + 0x10);
}

static int pressed_down(void) { return pad(kDown) != 0 || pad(kStickY) == -1; }
static int pressed_up(void) { return pad(kUp) != 0 || pad(kStickY) == 1; }
static int pressed_confirm(void) { return pad(kConfirmA) != 0 || pad(kConfirmB) != 0; }

static void sound(uint32_t var)
{
    uint32_t a[2] = {MEM32(var), 0};
    call_guest(sub_00163BF0, 1, a);
    call_guest(sub_00163AF0, 2, a);
}

static void quit_to_desktop(void)
{
    fprintf(stderr, "[MENU] Quit To Desktop\n");
    uint32_t a[2] = {0, 0};
    call_guest(sub_00153FC4, 2, a);  /* XLaunchNewImage(NULL, NULL): does not come back */
    xbox_HalReturnToFirmware(2);
}

static int wide_mode(void) { return MEM32(kMode) == 2 || MEM32(kMode) == 11; }
static float row_step(void) { return wide_mode() ? 33.0f : 37.0f; }

static uint32_t guest_text(const char *s, uint32_t *va)
{
    if (!*va) {
        size_t n = strlen(s) + 1;
        *va = xbox_HeapAlloc((uint32_t)n, 16);
        if (*va) memcpy((void *)XBOX_PTR(*va), s, n);
    }
    return *va;
}

static uint32_t quit_text(void)
{
    static uint32_t va;
    return guest_text("Quit To Desktop", &va);
}

static uint32_t ask_text(void)
{
    static uint32_t va;
    return guest_text("Quit To Desktop?", &va);
}

/* a line of the game's text (sub_0002A9F0, by key) */
static uint32_t game_text(uint32_t key)
{
    uint32_t a[1] = {key};
    return call_guest(sub_0002A9F0, 1, a);
}

static float ease(float cur, float target)
{
    /* up 0.04 a frame, down 0.02, as the title's rows */
    if (cur < target) return cur + 0.04f > target ? target : cur + 0.04f;
    if (cur > target) return cur - 0.02f < target ? target : cur - 0.02f;
    return cur;
}

static void set_align(uint32_t a0, uint32_t a1)
{
    uint32_t align[4] = {a0, a1, 0, 0};
    call_guest(sub_00168DC0, 4, align);
}

static void draw_text(const float col[4], float scale, float x, float y, uint32_t text)
{
    uint32_t c[4] = {fb(col[0]), fb(col[1]), fb(col[2]), fb(col[3])};
    call_guest(sub_00168D90, 4, c);
    uint32_t sc[1] = {fb(scale)};
    call_guest(sub_00168E60, 1, sc);
    uint32_t pos[2] = {fb(x), fb(y)};
    call_guest(sub_00168D70_gen, 2, pos);
    uint32_t t[1] = {text};
    call_guest(sub_00169A40_gen, 1, t);
}

/* the main menu's input step */
void sub_0001F200(void)
{
    if (!enabled()) {
        sub_0001F200_gen();
        return;
    }
    if (g_ask) {
        if (pressed_down() || pressed_up()) {
            g_ask_yes = !g_ask_yes;
            sound(kSndMove);
        } else if (pressed_confirm() && !MEM8(kBusy)) {
            sound(kSndConfirm);
            if (g_ask_yes) quit_to_desktop();
            g_ask = 0;
        } else if (pad(kBack) != 0) {
            sound(kSndBack);
            g_ask = 0;
        }
        g_esp += 4;  /* the step's ret */
        return;
    }
    if ((int32_t)MEM32(kRow) != kQuitRow) {
        int before = (int32_t)MEM32(kRow), down = pressed_down(), up = pressed_up();
        sub_0001F200_gen();
        int after = (int32_t)MEM32(kRow);
        /* the title wrapped past its last or first row: the fifth row comes in between */
        if ((down && before == kRows - 1 && after == 0) || (up && !down && before == 0 && after == kRows - 1))
            MEM32(kRow) = kQuitRow;
        return;
    }
    if (pressed_down()) {
        MEM32(kRow) = 0;
        sound(kSndMove);
    } else if (pressed_up()) {
        MEM32(kRow) = kRows - 1;
        sound(kSndMove);
    } else if (pressed_confirm() && !MEM8(kBusy)) {
        sound(kSndConfirm);
        g_ask = 1;
        g_ask_yes = 0;
    }
    g_esp += 4;  /* the step's ret */
}

/* text position (x, y) */
void sub_00168D70(void)
{
    float x = bf(ARG(0)), y = bf(ARG(1));
    g_last_x = x, g_last_y = y;
    /* the title's rows a row higher while the menu draws them (not the copyright line) */
    if (g_in_menu && !(x == 320.0f && y == 423.0f)) ARG(1) = fb(y - row_step());
    /* the pause menu's "select" hint below row 10 */
    if (g_pause_draw && x == 320.0f && y == 416.0f) ARG(1) = fb(430.0f);
    sub_00168D70_gen();
}

/* the hint's button sprite (x0, y0, x1, y1, ...) */
void sub_00030510(void)
{
    g_pause_hint = g_pause_draw;
    sub_00030510_gen();
    g_pause_hint = 0;
}

/* a 2D sprite (x0, y0, x1, y1, ...): the pause hint's button goes down to its text at 430 */
void sub_00169F70(void)
{
    if (g_pause_hint) {
        ARG(1) = fb(bf(ARG(1)) + 14.0f);
        ARG(3) = fb(bf(ARG(3)) + 14.0f);
    }
    sub_00169F70_gen();
}

static void pause_row(void);

/* text draw (format, ...) */
void sub_00169A40(void)
{
    if (g_in_menu && g_ask && !(g_last_x == 320.0f && g_last_y == 423.0f)) {
        g_esp += 4;  /* the rows give way to the question: their ret, undrawn */
        return;
    }
    if (g_pause_draw && g_pause_ask && g_last_x == 320.0f && g_last_y == 200.0f)
        ARG(0) = ask_text();  /* the box's title (GUI_SUREQUIT) */
    if (g_pause_draw && MEM32(kPState) == kPList && g_last_y == 90.0f + 30.0f * (kPRows - 1) &&
        (g_last_x == 90.0f || g_last_x == 128.0f)) {
        float x = g_last_x;
        uint32_t s_esp = g_esp;
        sub_00169A40_gen();
        uint32_t ret_esp = g_esp;
        g_esp = s_esp;
        g_last_x = x;
        pause_row();
        g_esp = ret_esp;
        return;
    }
    sub_00169A40_gen();
}

/* one of the main menu's row places (index i of five, shifted as the rows are), its colour and scale eased */
static void menu_row(int i, uint32_t text, float col[4], float *scale, int lit, float d)
{
    float target[4] = {0.2f - d * 0.02f, 0.64f - d * 0.0833333f, 0.99f - d * 0.0666667f, 0.95f - d * 0.1f};
    for (int k = 0; k < 4; k++) col[k] = ease(col[k], lit ? 1.0f : target[k]);
    float step = bf(MEM32(kFrameTime)) * 0.01f, top = MEM32(kMode) == 2 ? 1.15f : 1.4f;
    *scale += lit ? step : -step;
    if (*scale > top) *scale = top;
    if (*scale < 1.0f) *scale = 1.0f;
    float x = (MEM32(kMode) == 2 ? 392.0f : 400.0f) - 15.0f * i;
    float y = (wide_mode() ? 310.0f : 295.0f) + row_step() * (i - 1);
    set_align(0, 1);  /* the rows' alignment (0x1C46D) */
    draw_text(col, *scale, x, y, text);
}

/* the main menu's drawing */
void sub_0001C380(void)
{
    if (!enabled()) {
        sub_0001C380_gen();
        return;
    }
    uint32_t s_esp = g_esp;
    g_in_menu = 1;
    sub_0001C380_gen();
    g_in_menu = 0;
    uint32_t ret_esp = g_esp;
    g_esp = s_esp;

    int sel = (int32_t)MEM32(kRow);
    uint32_t text = quit_text();
    if (g_ask) {
        /* in the rows' places: the question, then Yes and No */
        static const float white[4] = {1, 1, 1, 1};
        uint32_t q = ask_text(), yes = game_text(kKeyYes), no = game_text(kKeyNo);
        if (q) {
            set_align(0, 1);
            draw_text(white, 1.0f, (MEM32(kMode) == 2 ? 392.0f : 400.0f) - 15.0f,
                      (wide_mode() ? 310.0f : 295.0f), q);
        }
        if (yes) menu_row(2, yes, g_ask_col[0], &g_ask_scale[0], g_ask_yes, 1);
        if (no) menu_row(3, no, g_ask_col[1], &g_ask_scale[1], !g_ask_yes, 1);
    } else if (text) {
        /* the fifth row, as the title's loop would draw row 4 (0x1C4B0) */
        float d = (float)(sel > kQuitRow ? sel - kQuitRow : kQuitRow - sel);
        menu_row(kQuitRow, text, g_col, &g_scale, sel == kQuitRow, d);
    }
    if (g_ask || text) {
        /* back as the title left them (the copyright line's settings, 0x1C82D) */
        set_align(1, 0);
        uint32_t col0[4] = {fb(0.75f), fb(0.75f), 0, fb(1.0f)};
        call_guest(sub_00168D90, 4, col0);
        uint32_t sc0[1] = {fb(0.9f)};
        call_guest(sub_00168E60, 1, sc0);
    }
    g_esp = ret_esp;
}

/* the pause menu's input step */
void sub_000307F0(void)
{
    if (!enabled()) {
        sub_000307F0_gen();
        return;
    }
    uint32_t state = MEM32(kPState);
    int row = (int32_t)MEM32(kPRow);
    if (state != kPSure) g_pause_ask = 0;
    int confirm = pad(kConfirmA) != 0, down = pressed_down(), up = pressed_up();
    if (state == kPSure && g_pause_ask && confirm && MEM32(kPYes) == 1) {
        uint32_t a[1] = {1};
        call_guest(sub_000306C0, 1, a);  /* the box's own click */
        quit_to_desktop();
    }
    sub_000307F0_gen();
    if (state != kPList || MEM32(kPState) != kPList) return;
    int after = (int32_t)MEM32(kPRow);
    if (row == kPQuitRow) {
        if (confirm) {
            /* as GUI_QUIT opens the box (0x30BA7), No lit; the step has clicked */
            MEM32(kPYes) = 0;
            MEM32(kPState) = kPSure;
            g_pause_ask = 1;
        } else if (after == kPQuitRow + 1) {
            MEM32(kPRow) = 0;  /* down from it wraps to the first row */
        }
    } else if (!confirm && ((up && row == 0 && after == kPRows - 1) || (down && !up && row == kPRows - 1 && after == 0))) {
        MEM32(kPRow) = kPQuitRow;  /* the step wrapped past the last or first row: row 10 comes in between */
    }
}

/* the pause menu's drawing: rows 30 apart while it runs */
void sub_00030F00(void)
{
    if (!enabled()) {
        sub_00030F00_gen();
        return;
    }
    uint32_t step = MEM32(kPStep);
    MEM32(kPStep) = fb(30.0f);
    g_pause_draw = 1;
    sub_00030F00_gen();
    g_pause_draw = 0;
    MEM32(kPStep) = step;
}

/* row 10 under GUI_QUIT, drawn as the pause menu's loop draws its rows (0x310F7), and the hint when it is lit */
static void pause_row(void)
{
    int sel = (int32_t)MEM32(kPRow), lit = sel == kPQuitRow;
    float d = (float)(sel > kPQuitRow ? sel - kPQuitRow : kPQuitRow - sel);
    float target[4] = {0.2f - d / 60.0f, 0.64f - d * 0.05f, 0.99f - d * 0.04f, 0.95f - d * 0.05f};
    for (int k = 0; k < 4; k++) {
        float t = lit ? 1.0f : target[k];
        if (g_pcol[k] < t) g_pcol[k] = g_pcol[k] + 0.04f > t ? t : g_pcol[k] + 0.04f;
        else if (g_pcol[k] > t) g_pcol[k] = g_pcol[k] - 0.04f < t ? t : g_pcol[k] - 0.04f;
    }
    float dt = bf(MEM32(kPDt));
    if (dt == 0.0f) dt = 1.0f / 30.0f;
    float top = call_guest(sub_0002A7C0, 0, NULL) == 11 ? 1.2f : 1.4f;
    g_pscale += lit ? dt * 4.0f : -dt * 4.0f;
    if (g_pscale > top) g_pscale = top;
    if (g_pscale < 1.0f) g_pscale = 1.0f;
    uint32_t text = quit_text();
    if (!text) return;
    set_align(0, 1);
    uint32_t sh[2] = {4, 5};
    call_guest(sub_00168DF0, 2, sh);
    draw_text(g_pcol, g_pscale, g_last_x, 90.0f + 30.0f * kPQuitRow, text);
    if (lit) {
        /* the hint the loop's end gives rows 0..5 and 9 (0x319FC) */
        static const float hint[4] = {0.183f, 0.596f, 0.85f, 1.0f};
        set_align(0, 1);
        uint32_t c[4] = {fb(hint[0]), fb(hint[1]), fb(hint[2]), fb(hint[3])};
        call_guest(sub_00168D90, 4, c);
        uint32_t sc[1] = {fb(1.0f)};
        call_guest(sub_00168E60, 1, sc);
        call_guest(sub_00030510, 0, NULL);
        uint32_t pos[2] = {fb(320.0f), fb(430.0f)};
        call_guest(sub_00168D70_gen, 2, pos);
        uint32_t t[1] = {game_text(kKeySelect)};
        if (t[0]) call_guest(sub_00169A40_gen, 1, t);
    }
}

/* ── The language the game starts in ────────────────────────────────────────────────────────────────────────────
 *
 * As it starts, the title takes the console's language (XGetLanguage, sub_00154283, in 0x151C8D: 1 English, 3 German,
 * 4 French, 5 Spanish, 6 Italian; others English) and sets it with sub_0002A790 (called from 0x151D10). The index
 * goes to [0x32E210]: 1 English, 2 French, 3 German, 4 Spanish, 5 Italian, 6 Dutch, 7 Swedish - the seven this disc
 * carries speech for, which the main menu's Language row offers (its flag list, 0x1EE56; its own call is 0x1C8C0).
 * The launcher's Language (hpps.toml language, RECOMP_LANGUAGE) replaces the console's at that start-up call, so
 * Dutch and Swedish, which no Xbox dashboard has, can be chosen too; the menu's row still changes it.
 * Built with --exclude-manual like the rest of this file; gen's sub_0002A790 renamed _gen by hand until a regen. */
extern void sub_0002A790_gen(void);
void sub_0002A790(void)
{
    static const char *const names[] = {"english", "french", "german", "spanish", "italian", "dutch", "swedish"};
    const char *e = getenv("RECOMP_LANGUAGE");
    if (MEM32(g_esp) == 0x00151D15u && e && *e) {
        for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            const char *a = e, *b = names[i];
            while (*a && (*a | 0x20) == *b) a++, b++;
            if (!*a && !*b) {
                fprintf(stderr, "[PORT] language: %s (index %u, the console's gave %u)\n", names[i], i + 1,
                        MEM32(g_esp + 4));
                MEM32(g_esp + 4) = i + 1;
                break;
            }
        }
    }
    sub_0002A790_gen();
}
