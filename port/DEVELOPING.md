# Developing the HP: Philosopher's Stone port

Everything needed to go from a fresh clone to a built, running and packaged game. Players want
[README.txt](README.txt) instead.

The port is a static recompilation of the Xbox title (`default.xbe`, XDK 5659) built on the
**xboxrecomp** toolkit (repository root: `src/`, `tools/`, `templates/`, see
[README.xboxrecomp.md](../README.xboxrecomp.md)), with the game project in `port/`.
Rendering is the D3D8 HLE → Direct3D 12 renderer from
[SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp) (MIT, `port/src/scda/LICENSE.SCDA`),
adapted to this title.

**Nothing from the game is in this repository**: no game files, no disassembly, no recompiled code, no
artwork. You need your own copy of the Xbox disc; the pipeline regenerates everything else from it.


## 1. Layout

```
<repo>/
  game/                   YOUR disc files (default.xbe, cut/, movies/, resource/) - ignored by git
  src/ tools/ templates/  xboxrecomp toolkit: runtime libraries (kernel, D3D, audio, input, video, USB...)
                          and the Python pipeline (xbe_parser, disasm, func_id, abi_analysis, recomp)
  cmake/ include/ docs/ tests/ LICENSES/   toolkit
  port/
    CMakeLists.txt        the game: hpps.exe (PAL), hpps_ntsc.exe (NTSC-US, see 3.1), hpps_launcher.exe, hpps_trainer.exe
    regen.sh [ntsc]       XBE -> port/src/recomp/gen/*.c or gen_ntsc/*.c (generated, ignored by git)
    icall_seeds.json      indirect-call targets the disassembler can't find by itself (regen.sh input)
    src/                  hand-written code
      main.c              startup: single instance / launcher check, debug.env, settings, XBE load, kernel
      hle_*.c             overrides of recompiled game/XDK functions (see 6.2)
      scda/               renderer (gpu/d3d12.cpp, shaders.cpp, pipeline_cache.cpp), XDK D3D8 HLE
                          (xdk/hle_d3d8.cpp), settings + F10 panel + input (app/)
      launcher/           hpps_launcher.exe (Dear ImGui over D3D11)
      trainer/            hpps_trainer.exe, the debug trainer
      recomp/             recomp_types.h etc.; gen/ is generated
      difftest.c          Unicorn-based original-vs-lifted comparison (HPPS_DIFFTEST builds)
    res/base_shaders.bin  shader bytecode shipped with releases (see 5)
    tools/                package_release.sh, xbx_to_ico.py, check_tls_refs.py, memtool, warp
    third_party/imgui/    Dear ImGui (MIT)
    README.txt            the players' readme (shipped with releases)
```


## 2. Setup (Windows)

Builds are native Windows builds with MinGW-w64 GCC from **MSYS2, UCRT64 environment**.

1. Install MSYS2 (`winget install --id MSYS2.MSYS2 -e`, or msys2.org).
2. In the **MSYS2 UCRT64** terminal (type commands; pasted text can carry invisible characters):
   ```
   pacman -Syu
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
                      mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-capstone
   ```
   (`pacman -Syu` may close the window; open it again and repeat it once.) GCC 16.2 is what the port has been
   built and checked with.
3. Copy the disc's files into `<repo>/game/` so that `<repo>/game/default.xbe` exists.

From PowerShell, any MSYS2 command runs as
`$env:MSYSTEM="UCRT64"; C:\msys64\usr\bin\bash.exe -lc "cd /d/<path-to-repo> && <command>"`.

If files were copied from another machine with a clock ahead of this one, Ninja loops with
"manifest 'build.ninja' still dirty"; reset the future timestamps (any file dated later than now) and configure
again.


## 3. Regenerate the recompiled code

```
bash port/regen.sh          # from the repo root, in MSYS2 UCRT64; a few minutes
```

It disassembles `game/default.xbe` (seeded with `port/icall_seeds.json`), identifies library functions, analyses
calling conventions and writes `port/src/recomp/gen/recomp_000N.c` + headers.

Every hand-written override file is passed with `--exclude-manual`: a `sub_XXXXXXXX` defined in one of them keeps
the generated body under `sub_XXXXXXXX_gen`, which the override calls when it wants the original behaviour.
**A new override file must be added to regen.sh** (and its `.c` is picked up by CMake's source glob). Never hand-edit
`gen/`: regenerate. A hook that has to sit *inside* a generated function (a debug trace at one instruction) goes in
`port/tools/patch_gen.py`, which regen.sh runs last; it finds its spot by a unique label and fails loudly if the
label moves.

If the game jumps through a pointer to code the disassembler never saw (an `ICALL` failure in the log), add the
target to `icall_seeds.json` and regenerate. `port/tools/scan_code_ptrs.py` lists code pointers found in data.

### 3.1 Two editions: PAL and NTSC-US

One source tree runs two discs: the PAL (Europe) *Philosopher's Stone* (`game/default.xbe` -> `gen/` -> `hpps.exe`)
and the NTSC-US *Sorcerer's Stone* (`game/ntsc/default.xbe` -> `gen_ntsc/` -> `hpps_ntsc.exe`). The two XBEs are
separate links of the same source: same title ID, size, sections and signature key; NTSC's `.text` is 32 bytes longer
and its functions sit in blocks shifted by -0x5E0 .. +0x8E0; data after `.text` moves +0x20, except two runs in
`.data`'s uninitialised part (+0x24 from 0x322CC8, +0x28 from 0x322D48 to 0x331AA0). The game even takes its edition
behaviour from the disc: `disk.txt` holds `1` (PAL) or `2` (NTSC-US), which picks the start-up language table.

```
bash port/regen.sh          # PAL first: it also leaves port/tools/edition/pal_functions.json
bash port/regen.sh ntsc     # NTSC-US: remakes the address map, then gen_ntsc/ from translated overrides
```

**Write every hand-written address as PAL's.** `port/tools/edition/pal2ntsc.py` maps PAL to NTSC (functions matched
with their address operands masked, call and pointer votes, the data runs, and an exact table of every operand the two
codes share), and the build compiles `hpps_ntsc` from copies of the game sources it translates into
`build-*/ntsc_src/`: a PAL fix reaches NTSC-US by rebuilding. It reads `sub_`/`loc_` names, `0x` literals and bare
`(00XXXXXX` macro arguments (`FALLBACK`, `HLE`). What it can't place stops the build (`--strict`): decide it in
`port/tools/edition/rules.txt` (`keep` a constant, `map` an address found by hand), and rerun `regen.sh ntsc` when an
override file changes which functions it defines. Only what really differs goes under `#if HPPS_NTSC`
(`src/edition.h`): names, the edition check, the languages (NTSC-US: English and Spanish, stored as 0 and 4).

`hpps_launcher.exe` serves both: it reads `game\default.xbe` to tell them apart, checks the folder against that disc's
list (`launcher/game_files_pal.inc` / `game_files_ntsc.inc`), shows its title and languages, and starts `hpps.exe` or
`hpps_ntsc.exe`.


## 4. Build

```
cd port
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-win -j$(nproc)
```

Outputs in `port/build-win/`: `hpps.exe`, `hpps_launcher.exe`, `hpps_trainer.exe`.

* The exe icon is made at configure time from `game/default.xbe`'s title image (`$$XTIMAGE`) by
  `tools/xbx_to_ico.py`. Without the XBE, and always in release builds (`HPPS_NO_ART`), the port's own icon
  `res/app.ico` is used instead: a wand and sparkle drawn from shapes by `tools/make_icon.py`, original artwork.
  `-DHPPS_GAME_XBE=<path>` points at another XBE.
* The launcher shows no game art in any build: the title as text (Windows' Georgia) over a black backdrop. The
  picture it used to load (`launcher_bg.png`, made from the title's textures) was removed on 2026-10-29.
* Generated code is compiled at **-O2** on purpose: GCC 16 at -O3 hoisted the address of a thread-local array
  out of a loop, reading the TLS template instead of the thread's copy. The post-build step
  `tools/check_tls_refs.py` fails the build on any such access ("58 thread local variables, no global address
  reads" is the good result).
* CMake options: `HPPS_RELEASE` (players' build: hardening, SSE4.1, no debug keys), `HPPS_NO_ART` (no game
  artwork inside the exes: no icon, no launcher picture, the launcher shows the title as text in Windows' Georgia;
  package_release.sh always sets it, so nothing of Warner Bros.' is in a release), `HPPS_DIFFTEST` (see 7.4),
  `HPPS_CALL_PROFILE` (per-target call counts, F7 dumps).


## 5. Run, test, release

**Run folder.** The exe looks for `game\default.xbe` in its own folder. Make a run folder (e.g. `port/run/`,
ignored by git) with the exes and a junction to the game files:
```
mkdir port\run
mklink /J port\run\game game          (cmd, from the repo root; no admin needed)
copy port\build-win\*.exe port\run\
```
It creates `hpps.toml` (settings), `saves\` (Xbox UDATA/TDATA, one folder per game, beside the exe) and
`shadercache\` there.

**Starting.** `hpps.exe` only runs when started by `hpps_launcher.exe` (which sets `HPPS_LAUNCHER=1` and the
`RECOMP_*` defaults the game expects, see `StartGame` in launcher.cpp). For scripted test runs set
`HPPS_LAUNCHER=1` yourself. Only one copy runs at a time (named mutex, `src/single_instance.h`); a second start
brings the running one forward. Alt+F4 ends the process at once (`fb_present.c`).

**debug.env** beside the exe: `KEY=VALUE` lines set as environment variables however the game is started (an
existing variable wins, `#` comments). This is how trace switches are turned on.

**Logs.** Only with F10 > Debug > "Write debug logs" (`debug_logs = true` in hpps.toml, from the next start):
`hpps_run.log` (the game's console output, written by the launcher) and `xbox_kernel.log` (kernel layer).
Crash reports (`[CRASH]` with symbolised guest functions; hpps.exe keeps its symbol table) land in hpps_run.log.

**Shader cache.** Shaders are generated HLSL keyed by a hash of their source, compiled with D3DCompile and kept in
`shadercache\shaders.bin`; pipelines in `shadercache\hpps_pipelines.bin` (per driver). A release ships
`res/base_shaders.bin` as `shadercache\base_shaders.bin` (read only, after the player's own) so a new PC compiles
almost nothing; bytecode a driver refuses is recompiled locally (`DropBaseCode`). Draws whose pipeline is still
building are skipped (`async_shaders`), and such a frame is not shown: the last whole frame stays up for up to 3 s
(`kHoldMax`) - without that, the first visit to an area showed the level's clear colour (light blue). **Refresh
`res/base_shaders.bin`** (copy a well-played `shadercache\shaders.bin` over it) whenever shader generation changes;
stale entries are harmless but useless.

**Release.** `bash port/tools/package_release.sh` (MSYS2) configures `build-release` with `HPPS_RELEASE=ON`,
builds, and fills `port/dist/hpps-<date>/` with hpps_launcher.exe, hpps.exe, README.txt and
`shadercache/base_shaders.bin`. It checks ASLR/NX/high-entropy, the stack protector, and that every imported DLL
ships with Windows 10/11 (C/C++ runtimes are static; XInput is `xinput1_4`). **It never deletes the folder**
(testers drop game files into it): only its own files are overwritten. Keep it that way.


## 6. How the port works

### 6.1 Title facts

Warthog engine, XDK 5659 (XbSymbolDatabase alias 5788), entry 0x154331, `.text` 0x11000-0x190714, title id
45410037. Guest memory is mapped at host = guest + 0x10000; the 64 MB contiguous window at 0x80000000.
D3D: CDevice 0x1A13E0 (g_pDevice 0x1A13D8) - PsTextureModes +0x790, m_VertexShader +0x794, ZScale +0x948,
SuperX/Y +0x95C/+0x960, transforms +0xC60, viewport +0xEE0, textures +0xF88, palettes +0xF98, RT/depth
+0x1A04/+0x1A08, vblank count +0x1DE8, VBlankCallback +0x1DB8, swap count +0x2478; DirtyFlags 0x1A0F38,
TextureState 0x1A0F40, RenderState 0x1A1140, g_Stream 0x1A3898. No vertex shaders: fixed function with lights,
P8 palettes, fences, CopyRects, SetGammaRamp, a vblank callback. Movies are XMV decoded by the game into a texture.

### 6.2 Overrides (hand-written code over recompiled functions)

| File | What it does |
|---|---|
| `scda/xdk/hle_d3d8.cpp` | the XDK's D3D8 entry points (≈90) onto the D3D12 backend; Swap, frame pacing, captures |
| `hle_dsound.c` | DirectSound on XAudio2 (3D buffers: distance/pan), audio latency ~32 ms |
| `hle_video.c` | XMV playback, the 16:9 camera (`sub_0016A4A0` widens 4:3 cameras, Hor+), menu movie / loading screen crop |
| `hle_input.c` | XInput onto the PC pad (`app/input.cpp`: remap, deadzones, keyboard and mouse); rumble forwarded on change only |
| `hle_xapi.c` | XAPI: the disc authentication check succeeds (it booted to the dashboard otherwise) |
| `hle_frontend.c` | "Quit To Desktop" row in the main and pause menus |
| `hle_camera.c` | Free camera: the camera stick's up / down pitch the chase view (`sub_00014ED0` stick, `sub_00016450` view) |
| `hle_anim.c` | animation start wrapper: door speed (`RECOMP_DOOR_SPEED`, default 1.8), anim/footstep traces |
| `hle_trainer.c` | the debug trainer's game side, autoplay, checkpoints, debug report |
| `hle_debug.c` | debug hooks and traces (script, crabs, DS3D, file objects) |

### 6.3 Renderer notes (scda/gpu/d3d12.cpp)

* Depth: W-buffered draws (ZEnable 2) write `log2(1+w)/32` (`/20` saturated ~75 units ahead); others plain z.
* Texture borders are emulated in the shader (`border2d`) - vkd3d/D3D12 border colours came out black.
* No-alpha console formats (R5G6B5, X8R8G8B8...) read alpha 1 in the shader (`kTexOpaque`).
* `D3D_BlockOnNonSurfaceResource` (0x196140, HLE in hle_d3d8.cpp) waits for the GPU (`WaitResource`, render
  thread): without it the CPU-skinned characters' ring vertex buffer was overwritten while still being drawn
  (missing/flickering NPCs).
* 16:9 (`Squeeze2D`, `WideFrame`, `WideCrop`): the 3D camera is widened; 2D keeps 4:3 shape - HUD in the left/right
  thirds anchors to the screen edges, centred 2D stays centred, rotated quads ("markers", the aiming circle) keep
  their own middle; cutscenes (letterbox bars) are shown zoomed to the original 4:3 framing with subtitles mapped
  into the band; HUD is hidden in cutscenes. Menu cameras (2.4/3.2) stay 4:3 and are tagged.
* F12 / Insert writes a capture (see 7.1).

### 6.4 Threads and kernel

Each guest thread gets its own KTHREAD copy and id (the game keys per-thread heap context on
`GetCurrentThreadId`); `RECOMP_HEAP_RECLAIM=1` is needed for New Game (contiguous arena). Saves: `E:\UDATA` /
`E:\TDATA` → `<exe dir>\saves` (`src/kernel/kernel_path.c`). Save files carry an HMAC-SHA1 signature (key: 16 zero
bytes) over the block after the 24-byte header.


## 7. Debugging tools

### 7.1 Keys and captures

* **F10** options panel (also Alt+Enter windowed/borderless).
* **F12 or Insert** captures the next frame to `captures\<HHMMSS>\`: `final.bmp`, `frame.txt` (every draw: state,
  textures, shaders, 16:9 squeeze), textures. `RECOMP_CAP_DRAWS=1` also saves the target before every draw;
  `RECOMP_CAP_VERTS=<tex addr hex>` dumps the vertices of draws using that texture;
  `RECOMP_CAPTURE_AFTER=<s>[:<every>:<count>]` captures unattended.
* `RECOMP_DEBUG_KEYS=1`: F8 log depth for all draws, F9 alpha test off, F7 depth test always passes.
  F6 (non-release builds) moves Harry to the camera.

### 7.2 Trainer and reports

`hpps_trainer.exe` (beside hpps.exe, either started first) talks to the game through the shared mapping
`Local\hpps_trainer` (`trainer_shm.h`): noclip (left stick flies, triggers up/down), health, spells, cards, beans,
bag size, level warp (via the hub when needed), checkpoints (`checkpoints\*.bin`, the 0x4AE4-byte progress block),
insta-kill (Ctrl+Shift+K), Undo. **Ctrl+Shift+R** writes `reports\<date_time>\`: report.txt, guest memory images,
the progress block, logs, settings, all thread stacks and game state (from a separate thread, so a hung game still
answers), plus a capture. `tools/memtool` (hpps_mem.exe) reads/writes guest memory of a running game;
`tools/warp` (hpps_warp.exe <level>) warps.

`RECOMP_BOOT_CHECKPOINT=<file>` restores a checkpoint on Harry's first frame; `RECOMP_AUTOPLAY=<file>` runs
`<seconds> <command> [args]` lines (captures, quitafter, dump, diffsweep, ...; see the comment in hle_trainer.c).

### 7.3 Traces (debug.env)

`[SPEECH]` is on with the debug logs (or `RECOMP_SPEECH_TRACE=1`): every PlaySpeech / Subtitle / FadeOutSpeech /
WaitForSpeech with the cutscene flag and speech state, and each change of them (see `hle_debug.c`).
`RECOMP_ANIM_TRACE`, `RECOMP_FOOTSTEP_TRACE`, `RECOMP_SCRIPT_TRACE`, `RECOMP_CRAB_TRACE`, `RECOMP_DS3D_TRACE`,
`RECOMP_DSOUND_TRACE` (`RECOMP_DSOUND_WAV` records the mix), `RECOMP_XMV_TRACE`, `RECOMP_D3D_TRACE`,
`RECOMP_PAD_TRACE`, `RECOMP_RUMBLE_TRACE`, `RECOMP_VSC_TRACE`, `RECOMP_WATCH`, `RECOMP_PROFILE` (sampling
profiler). F11 writes every thread's native stack plus held guest locks and I/O queues to `stacks.txt`
(symbolise with `nm -n hpps.exe`).

### 7.4 Difftest

`-DHPPS_DIFFTEST=ON` (own build folder) builds a hpps.exe that runs each called function's **original x86** in
Unicorn from the live state, then the lifted C, and compares registers, x87 and memory; needs `unicorn.dll`
(Unicorn Engine 2.x, x64) beside it. F5 starts/stops a sweep, `RECOMP_DIFF_AUTO=start_s,len_s` runs one,
`RECOMP_DIFF_ONLY=<addr,...>` restricts it, `RECOMP_DIFF_SAMPLES` / `RECOMP_DIFF_STRIDE` thin it; results in
`difftest.txt`. Known noise: the rdtsc timer family (QPC 0x18DD83), the 16:9 camera HLE (0x12110), the FPU control
word's IC bit, recursive functions (0x177E50, 0x1B6B0, 0x176BB0, 0x17A1C0), XMV MMX/x87 aliasing. Don't combine a
sweep with captures (it once overloaded the GPU driver).

### 7.5 Game facts for debugging

Player entity `[0x3565B0]` (+0x0C name, +0x28 scene object, +0xDC controller); health/max floats
0x329B90/0x329B94; progress/save block 0x3253B0 (0x4AE4 bytes; level byte at +0x49C2); spell buttons
0x329DD5..7 (Flipendo 1, Lumos 2, Diffindo 5, Spongify 8, Incendio 10, Alohomora 12); cards 0x329BD4; beans
0x329B8A; bag 0x329B8C; frame dt float 0x21B7BC. Levels 6..30 = 6 + index into the names at 0x21BB70 (level in play
`[0x356CC4]`, load `sub_00146110(id)`); hubs (dayN/nightN) load the shared hogday/hognight.hog, sub-levels expect
the hub's archive resident. Cutscenes are levels 32..75 (hpm01..hpm38, `game/cut`). Not on the retail disc
(warping there crashes): night3library (18, the Restricted Section is inside night3) and day5quidditch (24).
Animation start `sub_00142DB0(set, anim, first, last, fps, blend)`; AI types table 0x212078 (enter, update, exit).


## 8. Working rules

* **Translate faithfully.** Bridge what the game asks for with the console's semantics (e.g. rumble speeds persist
  until changed, so forward changes only). No keep-alives, synthetic events or hacks that mask a host quirk; if the
  host misbehaves, say so and fix it there.
* **Debug information first, then other recomps.** Gather captures, logs, traces, stacks, difftest results; then
  look for the same XDK function / lifter bug / renderer behaviour in other original-Xbox recomps
  (SCDA-XBERecomp, limeylicious/Nightfire-Windows-Port, DOAXBV, DefJam FFNY, Black, ...) and port a proven fix;
  move on quickly if one has nothing. Only then derive a fix from first principles. Check original-hardware
  behaviour before "fixing" something: several reports (slow doors, eyelids in the Quidditch cutscenes, crab range,
  hog flames) turned out to be how the Xbox game behaves.
* **One test run should answer everything.** Make traces independent of how the game is started (debug.env),
  self-test new instrumentation, trigger captures automatically, and collect every open question in one run.
* Never name a local variable after an x86 register in hand-written override files (`recomp_types.h` defines
  `esp`, `eax`, ... in generated code).
* Saves stay in `<exe dir>\saves`, per game - never in the game folder or a shared folder.
* Don't delete whole output folders (dist/, run/); overwrite your own files.
* Nothing from the game goes into the repository (game files, gen/, tools/*/output/, captures, reports, art).


## 9. Open issues

* *(Resolved, original behaviour)* **Fat Lady sometimes doesn't confirm the password.** After Harry answers, her
  script rolls `randomi` and branches: either her line plus the hand gesture, or the gesture alone (both seen in
  one traced session, 2026-10-20). Background for other "missing speech" reports: the PlaySpeech handler
  (0x48220) skips every line while the cutscene flag `[0x331161]` is set (StartCutScene 0x4BC00 clears it when its
  cutscene ends with no speech playing, speech state `[0x331770]`); the `[SPEECH]` trace (debug logs) shows the
  flag, the speech state and every command of the scripts that speak, values included.

* **Troll (Diffindo challenge, level 16):** walk animation looks broken and footsteps are missing. Footsteps come
  from animation events (Walk frame 21 / Run 13 → script `Troll_Footstep_SFX`); in one report the second troll's
  stand/walk/idle anim sets had no data (+0x14 == 0) - suspect the DeferredLoad attribute path in `sub_00027B30`.
  `RECOMP_ANIM_TRACE=1 RECOMP_FOOTSTEP_TRACE=1` print set names and data pointers.
* **Ground fog tiles (Diffindo challenge):** the fog looks squared. Fog = additive ONE/ONE layers, FVF 0x142,
  texture 81085100 (128x128 P8 noise), second pass with a projected texture transform. Use
  `RECOMP_CAP_VERTS=81085100` to see whether shared tile corners differ (game data) or not (renderer).
* **Final duel (level 29):** Harry aims at the arena's middle rather than Quirrell; phase-2 QUIRRELL2 (type 0x63)
  is never a target by the original's own candidate rules (`sub_00152E00`), so this may be original behaviour -
  compare with the console.
* **Forest pop-in (day3diffindo):** a gap shows sky at one spot; not streaming. Next step: difftest sweep there.
* **Controllers:** only XInput pads (Xbox and XInput-mode pads). PS4/PS5/generic pads need Steam Input or
  DS4Windows; the toolkit's SDL2 backend (`src/input/xinput_device.c`, non-Windows today) could be enabled on
  Windows, statically linked.
* Minimum specs in README.txt are estimates; the port has been played on a recent high-end PC only.
