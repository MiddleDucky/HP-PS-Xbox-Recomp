/* The game edition a build is for. One source tree builds both: hpps.exe from the PAL (Europe) Philosopher's Stone and,
 * with HPPS_NTSC=1 (CMakeLists.txt's hpps_ntsc target), hpps_ntsc.exe from the NTSC-US Sorcerer's Stone. The two XBEs
 * are separate links of the same source; the hand-written code keeps the PAL addresses and the NTSC target compiles
 * copies translated by port/tools/edition/pal2ntsc.py. Only what really differs between the discs is under HPPS_NTSC:
 * names, the edition check, the languages. */
#ifndef HPPS_EDITION_H
#define HPPS_EDITION_H

#ifndef HPPS_NTSC
#define HPPS_NTSC 0
#endif

#if HPPS_NTSC
#define HPPS_GAME_TITLE "Harry Potter and the Sorcerer's Stone"
#define HPPS_EDITION_NAME "NTSC-US"
#else
#define HPPS_GAME_TITLE "Harry Potter and the Philosopher's Stone"
#define HPPS_EDITION_NAME "PAL"
#endif
/* the launcher serves both editions and has no HPPS_NTSC: its messages name the series only */
#ifdef HPPS_LAUNCHER
#undef HPPS_GAME_TITLE
#define HPPS_GAME_TITLE "Harry Potter"
#endif
#define HPPS_WIDEN2(s) L##s
#define HPPS_WIDEN(s) HPPS_WIDEN2(s)
#define HPPS_GAME_TITLE_W HPPS_WIDEN(HPPS_GAME_TITLE)

#endif
