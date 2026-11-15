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
 * bodies of sub_0001F200, sub_0001C380, sub_000307F0, sub_00030F00, sub_00030510, sub_0001F5D0, sub_0001FEB0
 * (recomp_0000.c), sub_00168D70, sub_00169A40 and sub_00169F70 (recomp_0004.c) are renamed _gen by hand.
 * RECOMP_QUIT_ITEM=0 leaves both menus as they were.
 *
 * PC Settings (after HP:CoS's Options > PC Settings): the F10 settings as rows of the game's own menus, in its font.
 * The title's Options is front-end screen 0 ([0x325344]; the main menu's row 2 goes there): input sub_0001F5D0,
 * drawing sub_0001FEB0, rows 0..7 in [0x325334] (Extras, flying, camera, rumble, subtitles, three volumes; wrapping),
 * row i at an arc of x (120 94 82 77 76 93 120 142, 12 less in mode 11) and y 114 + 36 i (the 36.0 and 114.0 at
 * 0x1FB5C4 / 0x1FB5C0, read by this drawing and screen 6's only), each one string "label: value" (sub_00169A40) in a
 * colour and scale eased as the main menu's. "PC Settings" goes first, at row 0's place; the game's rows close up to
 * a 32 step from 146 while the screen draws. It is lit as row -1 of [0x325334]: the step acts on rows 0..7 only, and
 * its own up and down from -1 land on 7 and 0, which is the wrap with the new row in it. In the pause menu it is index
 * 10 (Quit To Desktop now 11) but on screen right after GUI_CONT_SAVEGAME (row 1): rows 2..9 are drawn a row lower,
 * up / down step 1 -> 10 -> 2 and back, and every row's colour eases by its distance on screen, not by index (the
 * game's own easing counts indices); the rows close up to 28. A opens it: a list of pages
 * (app_GameOptRows(-1), scda/app/menu.cpp: Display, Graphics, ...), then a page's rows with their values, drawn in
 * place of the game's rows under the page's name; up and down choose, left and right or A change a value, B goes back
 * a level. The game sees none of those presses (its step is not run). RECOMP_SETTINGS_ITEM=0 leaves it out.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"
#include "edition.h"

#define ARG(i) MEM32(g_esp + 4 + 4 * (i))

enum {
    kRow = 0x325334, kRows = 4, kQuitRow = 4, kMode = 0x32E210, kBusy = 0x32536D,
    kPadState = 0x325364, kDown = 0x356BD0, kStickY = 0x356BA8, kUp = 0x356BB0, kConfirmA = 0x356C14,
    kConfirmB = 0x356BE4, kBack = 0x356C74, kSndMove = 0x322DA0, kSndConfirm = 0x322D9C, kSndBack = 0x322DA4,
    kFrameTime = 0x322D48, kKeyYes = 0x1FB580, kKeyNo = 0x1FB578,
    kPState = 0x32E638, kPList = 2, kPSure = 0x10, kPRow = 0x32E644, kPYes = 0x32E7F8, kPDt = 0x32E62C,
    kPRows = 10, kPPcRow = 10, kPQuitRow = 11, kPStep = 0x1FB530, kKeySelect = 0x2118E0,
    /* PC Settings */
    kScreenNow = 0x325344, kOptScreen = 0, kOptRows = 8, kTransIn = 0x32536C, kOptStep = 0x1FB5C4, kOptY0 = 0x1FB5C0,
    kLeft = 0x356CA4, kRight = 0x356C18, kStickX = 0x356C6C,
};
static const float kPStepPx = 28.0f;                   /* the pause's rows while it draws: twelve above the hint */
static const float kOStepPx = 32.0f, kOY0Px = 146.0f;  /* the title's Options rows while it draws: a row lower */

extern void sub_0001F200_gen(void);
extern void sub_0001C380_gen(void);
extern void sub_000307F0_gen(void);
extern void sub_00030F00_gen(void);
extern void sub_00168D70_gen(void);
extern void sub_00169A40_gen(void);
extern void sub_00030510_gen(void);
extern void sub_00169F70_gen(void);
extern void sub_0001F5D0_gen(void);
extern void sub_0001FEB0_gen(void);
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
/* rows 10 (PC Settings) and 11 (Quit To Desktop): their easing scale; every row's colour, by its place on screen */
static float g_pscale[2] = {1.0f, 1.0f};
enum { kPSlots = 12 };
static float g_vcol[kPSlots][4];
static int g_vcol_init;
static float g_last_x, g_last_y;    /* the last text position the game set, as it set it */
static int g_opt_draw;              /* the title's Options is drawing */

/* one menu's PC Settings: closed, its list of pages (-1), or a page */
enum { kPcClosed = -2, kPcList = -1 };
typedef struct {
    int page, sel, top, list_sel;
    int in_pause;  /* the pause menu's: its own sounds (the front end's are not loaded in the game) */
} PcMenu;
static PcMenu g_opm = {kPcClosed, 0, 0, 0, 0}, g_ppm = {kPcClosed, 0, 0, 0, 1};
static int g_opt_entered;           /* the title's Options has run since the main menu last did */
static int settings_on(void);

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
    g_opt_entered = 0;  /* Options, opened from here, starts on PC Settings */
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

/* the pause menu's list is drawing, with the last position at one of the game's rows (x 90, 128 in mode 11) */
static int pause_rows_drawing(void)
{
    return g_pause_draw && MEM32(kPState) == kPList && (g_last_x == 90.0f || g_last_x == 128.0f);
}

/* the game's row (0..9) at y as the game places it (90 + 28 i while the pause draws), or -1 */
static int pause_row_at(float y)
{
    for (int i = 0; i < kPRows; i++)
        if (y == 90.0f + kPStepPx * (float)i) return i;
    return -1;
}

/* a pause row's place on screen: 0 Continue, 1 Save Game, 2 PC Settings (10), 3..10 the game's rows 2..9, 11 Quit
 * To Desktop (11). With no PC Settings row, Quit To Desktop is at 10. */
static int pause_slot(int r)
{
    if (!settings_on()) return r == kPQuitRow ? kPRows : r;
    if (r == kPPcRow) return 2;
    if (r == kPQuitRow) return kPQuitRow;
    return r >= 2 ? r + 1 : r;
}

/* a row's colour, eased toward its target a frame at a time: lit white, else bluer the further it is from the lit
 * row ON SCREEN (the game's own easing counts indices, and index 10 is not next to rows 9 or 11 any more) */
static const float *pause_ease_col(int slot)
{
    if (!g_vcol_init) {
        for (int i = 0; i < kPSlots; i++)
            g_vcol[i][0] = 0.2f, g_vcol[i][1] = 0.64f, g_vcol[i][2] = 0.99f, g_vcol[i][3] = 0.95f;
        g_vcol_init = 1;
    }
    int sel = pause_slot((int32_t)MEM32(kPRow)), lit = sel == slot;
    float d = (float)(sel > slot ? sel - slot : slot - sel);
    float target[4] = {0.2f - d / 60.0f, 0.64f - d * 0.05f, 0.99f - d * 0.04f, 0.95f - d * 0.05f};
    float *c = g_vcol[slot];
    for (int k = 0; k < 4; k++) {
        float t = lit ? 1.0f : target[k];
        if (t < 0.1f) t = 0.1f;
        if (c[k] < t) c[k] = c[k] + 0.04f > t ? t : c[k] + 0.04f;
        else if (c[k] > t) c[k] = c[k] - 0.04f < t ? t : c[k] - 0.04f;
    }
    return c;
}

/* text position (x, y) */
void sub_00168D70(void)
{
    float x = bf(ARG(0)), y = bf(ARG(1));
    g_last_x = x, g_last_y = y;
    /* the title's rows a row higher while the menu draws them (not the copyright line) */
    if (g_in_menu && !(x == 320.0f && y == 423.0f)) ARG(1) = fb(y - row_step());
    /* the pause menu's "select" hint below rows 10 and 11 */
    if (g_pause_draw && x == 320.0f && y == 416.0f) ARG(1) = fb(430.0f);
    /* rows 2..9 a row lower: PC Settings goes in after row 1 (GUI_CONT_SAVEGAME) */
    if (pause_rows_drawing() && settings_on() && g_ppm.page == kPcClosed) {
        int i = pause_row_at(y);
        if (i >= 2) ARG(1) = fb(y + kPStepPx);
    }
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
static void options_rows(float last_x);

/* y is one of the rows' places (n of them from y0, step apart) */
static int on_row(float y, float y0, float step, int n)
{
    for (int i = 0; i < n; i++)
        if (y == y0 + step * (float)i) return 1;
    return 0;
}

/* text draw (format, ...) */
void sub_00169A40(void)
{
    if (g_in_menu && g_ask && !(g_last_x == 320.0f && g_last_y == 423.0f)) {
        g_esp += 4;  /* the rows give way to the question: their ret, undrawn */
        return;
    }
    if (g_pause_draw && g_pause_ask && g_last_x == 320.0f && g_last_y == 200.0f)
        ARG(0) = ask_text();  /* the box's title (GUI_SUREQUIT) */
    if (pause_rows_drawing() && settings_on() && g_ppm.page == kPcClosed) {
        int i = pause_row_at(g_last_y);
        if (i >= 0) {
            const float *c = pause_ease_col(pause_slot(i));
            uint32_t a[4] = {fb(c[0]), fb(c[1]), fb(c[2]), fb(c[3])};
            call_guest(sub_00168D90, 4, a);
        }
    }
    int pause_last = g_pause_draw && MEM32(kPState) == kPList && g_last_y == 90.0f + kPStepPx * (kPRows - 1) &&
                     (g_last_x == 90.0f || g_last_x == 128.0f);
    int opt_last = g_opt_draw && g_last_y == kOY0Px + kOStepPx * (kOptRows - 1);
    /* a page of settings is up: the game's rows are left out (their ret, undrawn), ours go after the last */
    int hide = (g_pause_draw && g_ppm.page != kPcClosed && MEM32(kPState) == kPList &&
                (g_last_x == 90.0f || g_last_x == 128.0f) && on_row(g_last_y, 90.0f, kPStepPx, kPRows)) ||
               (g_opt_draw && g_opm.page != kPcClosed && on_row(g_last_y, kOY0Px, kOStepPx, kOptRows));
    if (pause_last || opt_last) {
        float x = g_last_x;
        uint32_t s_esp = g_esp;
        if (!hide) sub_00169A40_gen();
        else g_esp += 4;
        uint32_t ret_esp = g_esp;
        g_esp = s_esp;
        g_last_x = x;
        if (pause_last) pause_row();
        else options_rows(x);
        g_esp = ret_esp;
        return;
    }
    if (hide) {
        g_esp += 4;
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

/* ── PC Settings: the rows, the presses, the pages ──────────────────────────────────────────────────────────────── */

/* renderer/app/menu.cpp: the settings' pages (page -1 the list of pages) */
extern int app_GameOptRows(int page);
extern const char *app_GameOptLabel(int page, int row);
extern const char *app_GameOptValue(int page, int row);
extern void app_GameOptStep(int page, int row, int dir);
extern void app_GameOptLeave(int page);  /* a page's held choice (the window's size and shape) applied */

static int settings_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SETTINGS_ITEM");
        on = !(e && *e == '0');
    }
    return on;
}

static void pc_close(PcMenu *m)
{
    if (m->page >= 0) app_GameOptLeave(m->page);
    m->page = kPcClosed;
    m->sel = m->top = m->list_sel = 0;
}

static void pc_open(PcMenu *m)
{
    m->page = kPcList;
    m->sel = m->top = m->list_sel = 0;
    fprintf(stderr, "[MENU] PC Settings\n");
}

/* a frame's words for the settings' rows: each draw its own guest string, written again the next frame */
enum { kSlots = 48, kSlotChars = 64 };
static uint32_t g_slot_va[kSlots];
static int g_slot_next;

static uint32_t slot_text(const char *s)
{
    int i = g_slot_next++ % kSlots;
    if (!g_slot_va[i]) g_slot_va[i] = xbox_HeapAlloc(kSlotChars, 16);
    uint32_t va = g_slot_va[i];
    if (!va) return 0;
    int n = 0;
    for (; s && s[n] && n < kSlotChars - 1; n++) MEM8(va + (uint32_t)n) = (uint8_t)s[n];
    MEM8(va + (uint32_t)n) = 0;
    return va;
}

static uint32_t pc_text(void)
{
    static uint32_t va;
    return guest_text("PC Settings", &va);
}

/* the text draw with our words as an argument, not the format: a '%' in them is drawn as it is */
static void draw_words(const float col[4], float scale, float x, float y, uint32_t text)
{
    static uint32_t fmt_va;
    uint32_t fmt = guest_text("%s", &fmt_va);
    if (!fmt || !text) return;
    uint32_t c[4] = {fb(col[0]), fb(col[1]), fb(col[2]), fb(col[3])};
    call_guest(sub_00168D90, 4, c);
    uint32_t sc[1] = {fb(scale)};
    call_guest(sub_00168E60, 1, sc);
    uint32_t pos[2] = {fb(x), fb(y)};
    call_guest(sub_00168D70_gen, 2, pos);
    uint32_t t[2] = {fmt, text};
    call_guest(sub_00169A40_gen, 2, t);
}

static int pressed_left(void) { return pad(kLeft) != 0 || pad(kStickX) == -1; }
static int pressed_right(void) { return pad(kRight) != 0 || pad(kStickX) == 1; }

/* a menu sound: 0 move, 1 select, 2 back. The front end's (0x322DA0, 9C, A4) on the title; in the pause menu its own
 * table (sub_000306C0(i): [0x32E8BC + 4 i]; the step plays 0 moving, 1 choosing a row, 2 ([0x32E8C4]) going back) */
static void pc_sound(const PcMenu *m, int kind)
{
    if (m->in_pause) {
        uint32_t a[1] = {(uint32_t)kind};
        call_guest(sub_000306C0, 1, a);
        return;
    }
    sound(kind == 0 ? kSndMove : kind == 1 ? kSndConfirm : kSndBack);
}

/* the presses on the list of pages or a page (shown rows at once) */
static void pc_input(PcMenu *m, int shown)
{
    int n = app_GameOptRows(m->page);
    int down = pressed_down(), up = pressed_up(), left = pressed_left(), right = pressed_right();
    if (up && !down) {
        if (m->sel > 0) m->sel--, pc_sound(m, 0);
    } else if (down) {
        if (m->sel < n - 1) m->sel++, pc_sound(m, 0);
    }
    if (m->page >= 0 && (left || right) && !(left && right)) {
        app_GameOptStep(m->page, m->sel, left ? -1 : 1);
        pc_sound(m, 0);
    }
    if (pad(kConfirmA) != 0) {
        pc_sound(m, 1);
        if (m->page == kPcList) {
            m->list_sel = m->sel;
            m->page = m->sel;
            m->sel = m->top = 0;
        } else {
            app_GameOptStep(m->page, m->sel, 1);
        }
    } else if (pad(kBack) != 0) {
        pc_sound(m, 2);
        if (m->page >= 0) {
            app_GameOptLeave(m->page);
            m->page = kPcList;
            m->sel = m->list_sel;
            m->top = 0;
        } else {
            pc_close(m);
            return;
        }
    }
    n = app_GameOptRows(m->page);
    if (m->sel >= n) m->sel = n - 1;
    if (m->sel < 0) m->sel = 0;
    if (m->sel < m->top) m->top = m->sel;
    if (m->sel >= m->top + shown) m->top = m->sel - shown + 1;
}

/* a row's colour and scale, eased as the menus' rows (lit white and growing to top, 1.15 in mode 11; else bluer the
 * further it is) */
typedef struct { float col[4], scale; } Ease;
static void ease_row(Ease *e, int lit, float d, float top)
{
    float target[4] = {0.2f - d * 0.02f, 0.64f - d * 0.0833333f, 0.99f - d * 0.0666667f, 0.95f - d * 0.1f};
    for (int k = 0; k < 4; k++) {
        float t = lit ? 1.0f : target[k];
        if (t < 0.1f) t = 0.1f;
        e->col[k] = ease(e->col[k], t);
    }
    float step = bf(MEM32(kFrameTime)) * 0.01f;
    if (call_guest(sub_0002A7C0, 0, NULL) == 11 && top > 1.15f) top = 1.15f;
    if (step <= 0.0f) step = 0.02f;
    e->scale += lit ? step : -step;
    if (e->scale > top) e->scale = top;
    if (e->scale < 1.0f) e->scale = 1.0f;
}

/* a page's (or the list's) rows: the page's name at y0, then its rows (shown at once) step apart; labels at x, values
 * at vx. A setting that waits for the next start (its label ends in '*') says so under the rows while it is chosen.
 * The lit row grows to 1.2, not the menus' 1.4, and the values start 320 after the labels: the longest label
 * ("Vibration strength") lit stays clear of them (1.4 with values 270 after ran "Internal resolution" into "4K 2160p",
 * 2026-11-14); rows fade with distance up to two away only, so a long page stays readable to its end */
enum { kValueGap = 300, kPagePullLeft = 24 };
static Ease g_page_ease[2][16];
static void pc_draw(const PcMenu *m, Ease *ease_rows, float x, float vx, float y0, float step, int shown)
{
    static const float head[4] = {1.0f, 0.86f, 0.45f, 1.0f}, note[4] = {0.6f, 0.7f, 0.85f, 0.95f};
    int n = app_GameOptRows(m->page);
    set_align(0, 1);
    uint32_t sh[2] = {4, 5};
    call_guest(sub_00168DF0, 2, sh);
    char title[kSlotChars];
    if (m->page >= 0)
        snprintf(title, sizeof title, "PC Settings: %s", app_GameOptLabel(kPcList, m->page));
    else
        snprintf(title, sizeof title, "PC Settings");
    if (n > shown) {
        size_t l = strlen(title);
        snprintf(title + l, sizeof title - l, "   %d of %d", m->sel + 1, n);
    }
    draw_words(head, 1.0f, x, y0, slot_text(title));
    int restart = 0;
    for (int i = 0; i < shown && m->top + i < n && i < 16; i++) {
        int r = m->top + i;
        Ease *e = &ease_rows[i];
        int d = r > m->sel ? r - m->sel : m->sel - r;
        ease_row(e, r == m->sel, (float)(d > 2 ? 2 : d), 1.2f);
        char words[kSlotChars];
        snprintf(words, sizeof words, "%s", app_GameOptLabel(m->page, r));
        size_t len = strlen(words);
        if (len && words[len - 1] == '*') {
            words[--len] = 0;
            while (len && words[len - 1] == ' ') words[--len] = 0;
            if (r == m->sel) restart = 1;
        }
        float y = y0 + step * (float)(i + 1);
        draw_words(e->col, e->scale, x, y, slot_text(words));
        if (m->page >= 0) draw_words(e->col, e->scale, vx, y, slot_text(app_GameOptValue(m->page, r)));
    }
    if (restart) {
        float y = y0 + step * (float)(shown + 1);
        draw_words(note, 0.85f, x, y, slot_text("Takes effect the next time the game starts"));
    }
}

/* ── The title's Options ── */

/* the input step */
void sub_0001F5D0(void)
{
    if (!settings_on()) {
        sub_0001F5D0_gen();
        return;
    }
    if (!g_opt_entered) {  /* Options opened: PC Settings first */
        g_opt_entered = 1;
        pc_close(&g_opm);
        MEM32(kRow) = (uint32_t)-1;
    }
    int idle = !MEM8(kTransIn) && !MEM8(kBusy);  /* the step's own test (0x1F603): no screen change under way */
    if (g_opm.page != kPcClosed) {
        if (idle) pc_input(&g_opm, kOptRows);
        if (g_opm.page == kPcClosed) MEM32(kRow) = (uint32_t)-1;  /* back on the PC Settings row */
        g_esp += 4;  /* the step's ret */
        return;
    }
    if ((int32_t)MEM32(kRow) == -1 && idle && pad(kConfirmA) != 0) {
        sound(kSndConfirm);
        pc_open(&g_opm);
        g_esp += 4;
        return;
    }
    int before = (int32_t)MEM32(kRow), down = pressed_down(), up = pressed_up();
    sub_0001F5D0_gen();
    int after = (int32_t)MEM32(kRow);
    /* the step wrapped past its last or first row: the PC Settings row comes in between (from -1 the step itself
     * goes to 0 or 7) */
    if (MEM32(kScreenNow) == kOptScreen && before >= 0 &&
        ((down && before == kOptRows - 1 && after == 0) || (up && !down && before == 0 && after == kOptRows - 1)))
        MEM32(kRow) = (uint32_t)-1;
}

/* the drawing: the game's rows a row lower and closer while it runs */
void sub_0001FEB0(void)
{
    if (!settings_on()) {
        sub_0001FEB0_gen();
        return;
    }
    uint32_t step = MEM32(kOptStep), y0 = MEM32(kOptY0);
    MEM32(kOptStep) = fb(kOStepPx);
    MEM32(kOptY0) = fb(kOY0Px);
    g_opt_draw = 1;
    g_slot_next = 0;
    sub_0001FEB0_gen();
    g_opt_draw = 0;
    MEM32(kOptStep) = step;
    MEM32(kOptY0) = y0;
}

/* after the game's last row (row 7, x as the game placed it): "PC Settings" at row 0's old place, or a page */
static Ease g_opt_pc_ease = {{0.2f, 0.64f, 0.99f, 0.95f}, 1.0f};
static void options_rows(float last_x)
{
    float x0 = 120.0f - (last_x == 130.0f ? 12.0f : 0.0f);  /* row 0's x; row 7's is 142 (130 in mode 11) */
    if (g_opm.page != kPcClosed) {
        pc_draw(&g_opm, g_page_ease[0], 90.0f, 90.0f + kValueGap, 114.0f, kOStepPx, kOptRows);
        return;
    }
    int sel = (int32_t)MEM32(kRow);
    ease_row(&g_opt_pc_ease, sel == -1, (float)(sel + 1), 1.4f);
    set_align(0, 1);
    uint32_t sh[2] = {4, 5};
    call_guest(sub_00168DF0, 2, sh);
    draw_text(g_opt_pc_ease.col, g_opt_pc_ease.scale, x0, 114.0f, pc_text());
}

/* ── The pause menu ── */

/* the pause menu's input step */
void sub_000307F0(void)
{
    if (!enabled()) {
        sub_000307F0_gen();
        return;
    }
    uint32_t state = MEM32(kPState);
    if (state != kPList && g_ppm.page != kPcClosed) pc_close(&g_ppm);
    if (state == kPList && g_ppm.page != kPcClosed) {
        /* a page is up: its presses, not the pause's */
        pc_input(&g_ppm, kPQuitRow);
        if (g_ppm.page == kPcClosed) MEM32(kPRow) = kPPcRow;
        g_esp += 4;  /* the step's ret */
        return;
    }
    int row = (int32_t)MEM32(kPRow);
    if (state != kPSure) g_pause_ask = 0;
    int confirm = pad(kConfirmA) != 0, down = pressed_down(), up = pressed_up();
    if (state == kPSure && g_pause_ask && confirm && MEM32(kPYes) == 1) {
        uint32_t a[1] = {1};
        call_guest(sub_000306C0, 1, a);  /* the box's own click */
        quit_to_desktop();
    }
    if (state == kPList && row == kPPcRow && confirm && settings_on()) {
        uint32_t a[1] = {1};
        call_guest(sub_000306C0, 1, a);  /* the click the step gives a row it opens */
        pc_open(&g_ppm);
        g_esp += 4;
        return;
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
        } else if (after == kPPcRow) {
            MEM32(kPRow) = kPRows - 1;  /* up from it: GUI_QUIT (PC Settings is up after Save Game) */
        }
    } else if (row == kPPcRow) {
        /* the step's own +1 / -1 from index 10: 11 or 9; on screen it sits between rows 1 and 2 */
        if (after == kPPcRow + 1) MEM32(kPRow) = 2;
        else if (after == kPPcRow - 1) MEM32(kPRow) = 1;
    } else if (settings_on() && !confirm && ((row == 1 && after == 2) || (row == 2 && after == 1))) {
        MEM32(kPRow) = kPPcRow;  /* down from Save Game or up from row 2: PC Settings between them */
    } else if (!confirm && ((up && row == 0 && after == kPRows - 1) || (down && !up && row == kPRows - 1 && after == 0))) {
        /* the step wrapped past the last or first row: our rows come in between */
        MEM32(kPRow) = kPQuitRow;
    }
}

/* the pause menu's drawing: rows closer while it runs */
void sub_00030F00(void)
{
    if (!enabled()) {
        sub_00030F00_gen();
        return;
    }
    uint32_t step = MEM32(kPStep), row = MEM32(kPRow);
    MEM32(kPStep) = fb(kPStepPx);
    if (g_ppm.page != kPcClosed) MEM32(kPRow) = (uint32_t)-1;  /* none of the game's rows lit under a page */
    g_pause_draw = 1;
    g_slot_next = 0;
    sub_00030F00_gen();
    g_pause_draw = 0;
    MEM32(kPStep) = step;
    MEM32(kPRow) = row;
}

/* one of our rows (10 PC Settings, 11 Quit To Desktop) under GUI_QUIT, drawn as the pause menu's loop draws its rows
 * (0x310F7), and the hint when it is lit */
static void pause_one(int r, uint32_t text)
{
    int k = r - kPPcRow, sel = (int32_t)MEM32(kPRow), lit = sel == r, slot = pause_slot(r);
    const float *col = pause_ease_col(slot);
    float dt = bf(MEM32(kPDt));
    if (dt == 0.0f) dt = 1.0f / 30.0f;
    float top = call_guest(sub_0002A7C0, 0, NULL) == 11 ? 1.2f : 1.4f;
    g_pscale[k] += lit ? dt * 4.0f : -dt * 4.0f;
    if (g_pscale[k] > top) g_pscale[k] = top;
    if (g_pscale[k] < 1.0f) g_pscale[k] = 1.0f;
    if (!text) return;
    set_align(0, 1);
    uint32_t sh[2] = {4, 5};
    call_guest(sub_00168DF0, 2, sh);
    draw_text(col, g_pscale[k], g_last_x, 90.0f + kPStepPx * (float)slot, text);
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

/* after the game's last row (GUI_QUIT): our two rows, or a page of settings in place of them all */
static void pause_row(void)
{
    if (g_ppm.page != kPcClosed) {
        /* 24 left of the game's rows: a lit "3840 x 2160" ran past the box's right edge (capture 195510) */
        float x = g_last_x - kPagePullLeft;
        pc_draw(&g_ppm, g_page_ease[1], x, x + kValueGap, 90.0f, kPStepPx, kPQuitRow);
        return;
    }
    if (settings_on()) pause_one(kPPcRow, pc_text());
    pause_one(kPQuitRow, quit_text());
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
#if HPPS_NTSC
    /* NTSC-US: the title runs edition mode 2 ([0x32E20C] via sub_0002A7B0: the digit in the disc's disk.txt, "2" on
     * NTSC-US, "1" on PAL, read as the title starts), whose start-up branch knows English
     * (stores 0) and Spanish (console language 5, stores 4) only, and the disc carries speech for just those two
     * (movies/box_english, box_spanish, q_eng_us, q_spa). The value stored is mode 2's own for each */
    static const char *const names[] = {"english", "spanish"};
    static const uint32_t index[] = {0, 4};
#else
    static const char *const names[] = {"english", "french", "german", "spanish", "italian", "dutch", "swedish"};
    static const uint32_t index[] = {1, 2, 3, 4, 5, 6, 7};
#endif
    const char *e = getenv("RECOMP_LANGUAGE");
    if (MEM32(g_esp) == 0x00151D15u && e && *e) {
        for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            const char *a = e, *b = names[i];
            while (*a && (*a | 0x20) == *b) a++, b++;
            if (!*a && !*b) {
                fprintf(stderr, "[PORT] language: %s (index %u, the console's gave %u)\n", names[i], index[i],
                        MEM32(g_esp + 4));
                MEM32(g_esp + 4) = index[i];
                break;
            }
        }
    }
    sub_0002A790_gen();
}

/* ── Key names in the game's text ───────────────────────────────────────────────────────────────────────────────
 *
 * The game's text (sub_0002A9F0 -> sub_00177A00, by key; nothing else reads the tables) writes buttons as tags in the
 * PS2's names, which the text drawing (sub_00050550, per draw) swaps for a sprite from the platform's table: on the
 * Xbox (0x211FB8, in the tag order of 0x211F28) [[CROSS ICON]] xbox_a, [[SQUARE ICON]] xbox_x, [[TRIANGLE ICON]]
 * xbox_y, [[CIRCLE ICON]] xbox_b, [[R1 ICON]] xbox_r_shoulder (the right trigger: "the target button"), [[L1 ICON]]
 * xbox_button_black (Camera Reset), [[SELECT ICON]] xbox_button_back, [[R2 ICON]] xbox_button_white ("hold down ...
 * to look around"). The save, controller and display messages name buttons in words: "Press A to continue or B to
 * delete", "press START", "PRESS A TO CONTINUE".
 *
 * With key prompts on (app_PromptsKbm: F10 Controls, Button prompts, the device used last) a tag or a button word
 * comes back as the bound key in quotes ("Press 'Space' to continue", "the target button 'Ctrl'"), as HP:CoS's and
 * HP:PoA's prompts do; the game's text uses ', and has no [ ] of its own. A, B, X, Y and BACK keep their tags: the
 * renderer draws their sprites as the bound key (d3d12.cpp's kPromptButtons, captured from these texts), an icon in
 * the button's colour, as PoA keeps its icons. So do legends, text that starts with a button ("[[CROSS ICON]] CLOSE
 * BOOK", "[[SELECT ICON]] Exit Remembrall"): the word sits at a set place after the sprite. A button with no key
 * bound keeps its sprite or word.
 *
 * Each such text is given as a copy of its own on the game's heap (the tables are left as they are), with room for
 * the longest key names, and the copy is written again in place each Swap when the device or a binding changes, so a
 * tutorial box that keeps its pointer while it is up switches with the device. The original is kept host side: a
 * level's tables are freed and loaded again, and a copy's source address may then hold something else.
 * Built with --exclude-manual like the rest of this file; gen's sub_0002A9F0 renamed _gen by hand until a regen. */
extern int app_PromptsKbm(void);
extern const char *app_ButtonKeyLabel(int pad_input);

enum { kLabelMax = 16, kPromptCopies = 256, kQuoted = kLabelMax + 2 };

static const struct { const char *tag; int input, icon; } kPromptTags[] = {
    {"[[CROSS ICON]]", 0, 1},  {"[[CIRCLE ICON]]", 1, 1}, {"[[SQUARE ICON]]", 2, 1}, {"[[TRIANGLE ICON]]", 3, 1},
    {"[[R1 ICON]]", 7, 0},     {"[[L1 ICON]]", 4, 0},     {"[[R2 ICON]]", 5, 0},     {"[[SELECT ICON]]", 13, 1},
    {"[[START ICON]]", 12, 0},
};
static const struct { const char *word; int input; } kPromptWords[] = {
    {"A", 0}, {"B", 1}, {"X", 2}, {"Y", 3}, {"START", 12}, {"BACK", 13},
};

static struct { uint32_t src, copy, cap; char *orig; } g_prompt[kPromptCopies];
static int g_prompt_n, g_prompt_next;
static char g_prompt_sig[256];  /* what the copies hold: "pad", or the key names */

static int lower_c(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int alnum_c(int c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

/* out[0..m) ends with w (lower case), at a word's start */
static int ends_with_word(const char *out, size_t m, const char *w)
{
    size_t l = strlen(w);
    if (m < l) return 0;
    for (size_t k = 0; k < l; k++)
        if (lower_c((unsigned char)out[m - l + k]) != w[k]) return 0;
    return m == l || !alnum_c((unsigned char)out[m - l - 1]);
}

/* the button tag at s, its kPromptTags index, -1 none */
static int prompt_tag(const char *s, size_t *len)
{
    if (s[0] != '[' || s[1] != '[') return -1;
    for (int t = 0; t < (int)(sizeof(kPromptTags) / sizeof(kPromptTags[0])); t++) {
        size_t l = strlen(kPromptTags[t].tag);
        if (!strncmp(s, kPromptTags[t].tag, l)) return *len = l, t;
    }
    return -1;
}

/* a legend: a button first (after any spaces) */
static int prompt_legend(const char *s)
{
    while (*s == ' ') s++;
    return s[0] == '[' && s[1] == '[';
}

/* src's text with the buttons named as keys (keys) or as it is, into out (size cap); the number of buttons it has */
static int prompt_rewrite(const char *src, char *out, size_t cap, int keys)
{
    int upper = 1, buttons = 0, named = 0;
    for (const char *p = src; *p; p++)
        if (*p >= 'a' && *p <= 'z') upper = 0;
    const int legend = prompt_legend(src);
    size_t m = 0;
    for (size_t i = 0; src[i];) {
        int input = -1, keep = 0;
        size_t len = 0;
        int t = prompt_tag(src + i, &len);
        if (t >= 0) {
            input = kPromptTags[t].input;
            keep = kPromptTags[t].icon || legend;
        } else if (i == 0 || !alnum_c((unsigned char)src[i - 1])) {
            /* a button word after "press", or after "or" once a button was named ("Press A ... or B to delete") */
            for (unsigned w = 0; w < sizeof(kPromptWords) / sizeof(kPromptWords[0]); w++) {
                size_t l = strlen(kPromptWords[w].word);
                if (strncmp(src + i, kPromptWords[w].word, l) || alnum_c((unsigned char)src[i + l])) continue;
                if (ends_with_word(out, m, "press ") || ends_with_word(out, m, "pressing ") ||
                    (named && ends_with_word(out, m, "or "))) {
                    input = kPromptWords[w].input;
                    len = l;
                }
                break;
            }
        }
        if (input < 0) {
            if (m + 1 < cap) out[m++] = src[i];
            i++;
            continue;
        }
        buttons++;
        const char *name = keys && !keep ? app_ButtonKeyLabel(input) : "";
        if (!*name) {  /* the controller's, an icon, or unbound: as the game has it */
            for (size_t k = 0; k < len && m + 1 < cap; k++) out[m++] = src[i + k];
        } else {
            if (m + 1 < cap) out[m++] = '\'';
            for (int k = 0; name[k] && k < kLabelMax && m + 1 < cap; k++)
                out[m++] = (char)(upper && name[k] >= 'a' && name[k] <= 'z' ? name[k] - 32 : name[k]);
            if (m + 1 < cap) out[m++] = '\'';
        }
        named = 1;
        i += len;
    }
    out[m] = 0;
    return buttons;
}

/* "pad" with the controller's prompts, else the key names now */
static void prompt_sig(char *sig, size_t size)
{
    static const int kInputs[] = {0, 1, 2, 3, 4, 5, 7, 12, 13};
    if (!app_PromptsKbm()) {
        snprintf(sig, size, "pad");
        return;
    }
    int sl = snprintf(sig, size, "keys|");
    for (unsigned b = 0; b < sizeof(kInputs) / sizeof(kInputs[0]) && sl < (int)size; b++)
        sl += snprintf(sig + sl, size - (size_t)sl, "%s|", app_ButtonKeyLabel(kInputs[b]));
}

static void prompt_write(int slot, int keys)
{
    char buf[4096];
    uint32_t cap = g_prompt[slot].cap < sizeof(buf) ? g_prompt[slot].cap : (uint32_t)sizeof(buf);
    prompt_rewrite(g_prompt[slot].orig, buf, cap, keys);
    memcpy((void *)XBOX_PTR(g_prompt[slot].copy), buf, strlen(buf) + 1);
}

/* each Swap (hle_d3d8.cpp) and each lookup: the copies follow the device and the bindings */
void hpps_PromptRefresh(void)
{
    char sig[256];
    prompt_sig(sig, sizeof(sig));
    if (!strcmp(sig, g_prompt_sig)) return;
    snprintf(g_prompt_sig, sizeof(g_prompt_sig), "%s", sig);
    int keys = strcmp(sig, "pad") != 0;
    for (int i = 0; i < g_prompt_n; i++) prompt_write(i, keys);
    fprintf(stderr, "[PROMPTS] %d texts now %s\n", g_prompt_n, keys ? "name the keys" : "show the buttons");
}

static uint32_t prompt_text(uint32_t text)
{
    if (!text) return text;
    const char *s = (const char *)XBOX_PTR(text);
    size_t n = strnlen(s, 2048);
    if (n >= 2048) return text;
    char probe[4096];
    int buttons = prompt_rewrite(s, probe, sizeof(probe), 0);
    if (!buttons) return text;
    hpps_PromptRefresh();
    for (int i = 0; i < g_prompt_n; i++)
        if (g_prompt[i].src == text && !strcmp(g_prompt[i].orig, s)) return g_prompt[i].copy;
    uint32_t cap = (uint32_t)(n + (size_t)buttons * kQuoted + 1);
    uint32_t copy = xbox_HeapAlloc(cap, 16);
    char *orig = (char *)malloc(n + 1);
    if (!copy || !orig) return free(orig), text;
    memcpy(orig, s, n + 1);
    /* full: the oldest is no longer kept up to date (its copy stays where it is, for whoever holds it) */
    int slot = g_prompt_n < kPromptCopies ? g_prompt_n++ : (g_prompt_next++ % kPromptCopies);
    free(g_prompt[slot].orig);  /* NULL for a new slot */
    g_prompt[slot].src = text;
    g_prompt[slot].copy = copy;
    g_prompt[slot].cap = cap;
    g_prompt[slot].orig = orig;
    prompt_write(slot, strcmp(g_prompt_sig, "pad") != 0);
    return copy;
}

extern void sub_0002A9F0_gen(void);
void sub_0002A9F0(void)
{
    static int off = -1;  /* RECOMP_KEY_TEXT=0 leaves the text as the game has it */
    if (off < 0) off = getenv("RECOMP_KEY_TEXT") && getenv("RECOMP_KEY_TEXT")[0] == '0';
    sub_0002A9F0_gen();
    if (!off) g_eax = prompt_text(g_eax);
}
