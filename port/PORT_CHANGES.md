# HP: Philosopher's Stone port: changes that alter the game

Every change that makes the port behave or look different from the Xbox, or that replaces game code, and what else it
could affect. When something looks wrong, check this list first and switch the suspects off before hunting
elsewhere. Add an entry for every new change of this kind. (Paths are under `port/`.)

Columns: what changed, where, how to turn it off, what it could plausibly break.

## Game logic replaced or changed

| Change | Where | Off switch | Could affect |
|---|---|---|---|
| Door animations play 1.8x as fast (doors take ~7 s on the console) | `src/hle_anim.c` over `sub_00142DB0` (door state machine `sub_000D86D0`, push at `0xD7332`) | `RECOMP_DOOR_SPEED=1` | anything sharing those animation calls; Harry's push timing |
| XAPI DVD-X2 disc authentication skipped | `src/hle_xapi.c` over `sub_00155251` | `RECOMP_HLE_XAPI=0` | boot only |
| "Quit To Desktop" row on main/pause menu | `src/hle_frontend.c` (menu input `sub_0001F200`, draw `sub_0001C380`) | `RECOMP_QUIT_ITEM=0` | menu row count, wrapping, Extras row |
| "PC Settings" in Options and the pause menu (2026-11-14), after HP:CoS's: the first row of the title's Options (front-end screen 0, lit as row -1 of `[0x325334]`; the game's eight rows a row lower, 32 apart from 146 through its `.rdata` 36.0 / 114.0 swapped while it draws) and the pause menu right after Save Game (index 10, drawn third: rows 2..9 a row lower, colours eased by distance on screen; Quit To Desktop now index 11; rows 28 apart; a page sits 24 left of the rows so values stay in the box). A opens pages drawn in the game's font in place of its rows: Display, Graphics, Post-processing, Audio, Controls, Accessibility, Enhancements, each the F10 settings as rows with values (left / right / A change them, saved to hpps.toml as F10 does); B goes back a level; the game's step is not run while one is up. Key bindings, button remapping and Debug stay in F10 | `src/hle_frontend.c` (`sub_0001F5D0` / `sub_0001FEB0` over their `_gen` bodies, `sub_000307F0` / `sub_00030F00`, `sub_00169A40`), `scda/app/menu.cpp` (`app_GameOpt*`) | `RECOMP_SETTINGS_ITEM=0` | Options and pause navigation and drawing, the volume sliders' arrows |
| Stick radial range compensation | input path | `RECOMP_STICK_RANGE` | walk/run thresholds, all stick input |
| Trainer hooks (warp, items, autoplay) | `src/hle_trainer.c` over `sub_00095370` | only with the trainer / `RECOMP_AUTOPLAY` | Harry's update each frame |
| Settings: fps, keyboard/mouse, colour-blind, controller | `src/scda/app`, launcher | per setting | timing, input |
| Game files checked (2026-10-29, from the CoS port): the launcher compares `game\` with a list of the PAL disc's 105 files (size + CRC-32, `game_files.inc` from `tools/make_game_files.py`): every byte on a fresh start (no `game_check.txt` under the list's CRC), after unpacking and from the page's Verify/Repair buttons, sizes on every other start; damaged files kept by name in `game_check.txt`; named on the page (Repair re-unpacks them from an .iso in `game\`) and, with debug logs on, in `hpps_run.log`'s first line `[FILES]` | `launcher/gamefiles.c`, `launcher.cpp`, `xiso.c` (`xiso_FindAny`) | Skip on the check page; Play stays allowed | another print run of the disc would show as damaged; first start reads 3.1 GB |
| Crash log with the debug logs off (2026-10-29): an exception nothing handles writes `hpps_crash.log` beside the exe (the [CRASH] report, `crash_filter` via SetUnhandledExceptionFilter); with them on it goes to `hpps_run.log` as before, plus a 'stops here' line | `main.c` (`print_crash`, `crash_filter`) | none | nothing in play: runs only when the process is going down |
| Launcher: no game art in any build (2026-10-29): the built-in/beside-the-exe `launcher_bg.png` is gone; black backdrop with the title in Windows' Georgia. d3d11 (and its dxgi) loaded from System32 only, so a DXVK `dxgi.dll` beside the exes no longer crashes it under Wine. `show_launcher` first in `hpps.toml` | `launcher.cpp`, `CMakeLists.txt`, `scda/app/app.cpp` | none | look of the launcher only |

## Camera / 16:9

| Change | Where | Off switch | Could affect |
|---|---|---|---|
| True 16:9: cameras widened (x scale 3/4) | `src/hle_video.c` Camera_SetScale, `scda/gpu/d3d12.cpp` | aspect 4:3 | anything computed from the projection |
| Wide camera's near plane cut to 0.75x so walls don't show outside the level | `d3d12.cpp` | `RECOMP_WIDE_NEAR=1` | near clipping, depth precision close up |
| 2D squeeze, centred panels, spinning marker, cutscene bars, WideFrame crop | `Squeeze2D`, `WideFrame`, `d3d12.cpp` | 4:3 | any draw classed as 2D, cutscene framing |
| Menu background movie / loading screen crops | `src/hle_video.c` | `RECOMP_MENU_MOVIE_CROP`, `RECOMP_LOADING_CROP` | those screens |
| Free camera (2026-11-15): the camera stick's / mouse's up and down tilt the play camera round Harry (30° under to 80° over the focus, through the game's camera collision probe, above his feet) instead of stepping the console's 4 zoom levels; the game's camera state is left alone (the view is built from a pitched copy of its position), and the tilt eases back in cutscenes and scripted cameras | `src/hle_camera.c` (`sub_00014ED0` the camera stick, `sub_00016450` the chase view) | F10 > Enhancements > Free camera, `free_camera = false`, `RECOMP_FREE_CAMERA=0` | the zoom levels (stay where they are), what the camera sees below / above ledges |

## Renderer (D3D8 HLE + D3D12, `src/scda/`)

| Change | Where | Off switch | Could affect |
|---|---|---|---|
| Pipeline recording: every pipeline made goes into `shadercache/pipelines_list.bin`, and all of them are made at startup on the build threads (2026-10-24, from HP:CoS) | `pipeline_cache.cpp`, `d3d12.cpp` `StartPrebuild` | `RECOMP_PREBUILD=0` | startup work on the build threads |
| Per-game Mesa shader cache under Wine (`run/shadercache/mesa`, 4 GB): Wine's vkd3d has no pipeline library, and the shared 1 GB cache evicted the game's pipelines | `run_win.sh`, `run_launcher.sh` | set `MESA_SHADER_CACHE_DIR` yourself | first run after the change compiles everything once |
| Lines drawn as the NV2A draws them: a geometry shader makes each segment a quad one console pixel across, its ends on console pixel edges, half a pixel down/right for the NV2A's integer pixel centres (2026-10-24, from HP:PoA, whose shadow fade mask relies on it) | `shaders.cpp` `LineGsHlsl`, `d3d12.cpp` `EnsureVertexShader` / `EnsurePipeline` | `RECOMP_LINE_GS=0` | every line draw (thicker when upscaled, as on the console) |
| Whole renderer: D3D8 calls replaced by HLE | `scda/xdk/hle_d3d8.cpp` | `RECOMP_HLE_D3D=0` | everything drawn |
| 3D draws with depth on the log2(1+w) scale | `UseLogDepth` | `RECOMP_LOGDEPTH_ALL=0` | depth ties, effects vs geometry |
| Target read as a no-alpha format reads alpha 1 | `BindTargetTexture` | none | alpha of render-to-texture reads |
| Loading-bar track fix on the loading picture | `d3d12.cpp` (`load_hognight.tga`) | none | that texture |
| Colour-blind correction in the gamma pass | `d3d12.cpp` | setting off | final colours |
| GPU failsafes: lost/hung GPU ends the game; draw counts past a limit dropped as garbage | `d3d12.cpp` | none | a genuinely huge draw |
| Pipeline hold limit raised (first frames on a new PC) | `d3d12.cpp` | none | frames held while shaders build |
| Shipped shader cache `base_shaders.bin` | `d3d12.cpp`, release | delete file | first-use stutter only |
| Draw-all / culling, P8 palettes as BGRA, border addressing in the shader | `d3d12.cpp`, `shaders.cpp` | none | textures at edges, palettes |
| 2D seam fix: quad texture coordinates held inside the quad's rectangle (`qclamp`); texel units only for linear textures (2026-10-22 fix from CoS: guessing from coordinates > 1.5 flattened scrolling 2D layers after a long run) | `shaders.cpp` `QuadHlsl` | none | edges of 2D quads, scrolling 2D layers |
| Button prompts as PoA's and CoS's (2026-11-10): with key prompts (F10 Controls > Button prompts: Automatic follows the device used last, or keys always) the game's Xbox button pictures are drawn as the bound key, round like the Xbox buttons (a face of the button's colour inside a pale ring, the key's name in white, dark on yellow, or a small mouse with that button lit) at the size of the game's own button in its picture: the face-button wheel (`xbox_buttons.tga`, 128x128 P8, its four quarters drawn by the HUD and the inventory, Y B / A X) as a 256x256 picture of four keys, X Y B ("to equip"), Back ("Exit Remembrall"), the inventory's L and R as LT / RT; matched by the FNV-1a of level 0 decoded to BGRA (F12's `tex_*.bmp`); A, Black and White alone not captured yet (`RECOMP_PROMPT_LOG=1` prints the hash of other button-sized P8 pictures) | `PromptLookup` / `PromptPicture` / `BindPromptTexture` in `src/scda/gpu/d3d12.cpp`; `PromptsKbmNow`, `NoteDevice` in `src/scda/app/input.cpp` | F10 Controls > Button prompts: Controller buttons | the BACK picture's own "BACK" word goes with it; spell icons over the HUD's buttons cover part of a key cap; any other P8 picture with the same pixels |
| Key names in the game's text (2026-11-10): with key prompts on, the text's button tags and button words name the bound key in quotes, "Press 'Space' to continue", "the target button 'Ctrl'", "PRESS 'SPACE' TO CONTINUE" (tags by the Xbox sprite table 0x211FB8: CROSS A, R1 RT, L1 Black, R2 White, START; words A B X Y START BACK after "press" / "pressing", or "or" after one); B X Y BACK tags and legends ("[[CROSS ICON]] CLOSE BOOK") keep the sprite, drawn as the key's icon; each such text a heap copy rewritten in place each Swap when the device or a key changes | `sub_0002A9F0` override, `prompt_rewrite`, `hpps_PromptRefresh` in `src/hle_frontend.c` (gen's body renamed `_gen` until a regen); `app_ButtonKeyLabel` in `src/scda/app/input.cpp`; the Swap in `src/scda/xdk/hle_d3d8.cpp` | `RECOMP_KEY_TEXT=0`, or F10 Controls > Button prompts: Controller buttons | English words only ("press"); tags in every language; legends with A / White keep the Xbox picture until their hashes are captured |

## Audio / system

| Change | Where | Off switch | Could affect |
|---|---|---|---|
| Heap reclaim on by default in the toolkit (freed memory reused, as the console does; RtlHeap decommits once instead of ~38k failed retries) | toolkit copy `src/kernel/xbox_memory_layout.c` | `RECOMP_HEAP_RECLAIM=0` | addresses of every later allocation |
| Cache partition (Z:) kept between boots: toolkit partition table moved to sector 0, XAPI's cache database in sector 4 left to the title; dismount (FSCTL 0x90020) succeeds | toolkit copy `src/kernel/kernel_path.c`, `kernel_bridge.c`, `kernel_file.c` | none | Z: contents, first boot formats once |
| Factory game region (0x104) answered from the XBE certificate: this PAL release gets 4 | toolkit copy `src/kernel/kernel_xbox.c` | `RECOMP_GAME_REGION=<n>` | language choice, region checks |
| DirectSound replaced by host mixer | `src/hle_dsound.c` | `RECOMP_HLE_DSOUND=0` | all sound; `RECOMP_AUDIO_LATENCY_MS` |
| 5.1 surround output (2026-10-24): F10 Audio > Speakers (`speakers = 6`); each mixbin goes to its own speaker (front L R, centre, LFE, back L R; crosstalk bins to their positions), a multichannel buffer channel by channel, 6-channel XAudio2 output with mask 0x3F. Stereo (the default) mixes as before | `src/hle_dsound.c` `levels6`, `scda/app` | `speakers = 2` | all sound when 5.1 is on; needs a 5.1 Windows speaker setup |
| Display vblank counter kept by the host | `src/hle_video.c` | none | game timing tied to vblank |
| Factory AV region answered (NTSC) | toolkit `kernel_xbox.c` | none | video standard, game clock step |
| Console time zone = the PC's (bias, daylight dates and biases, names; also the whole user block, index 0xFF, which XAPI's GetTimeZoneInformation reads) - was a fixed UTC+5 | toolkit `kernel_xbox.c` `ExQueryNonVolatileSetting` | none | local time: save timestamps, anything the game shows from the clock |
| ISO/XISO unpacked by the launcher | `src/launcher/xiso.c` | none | game files on first run |
| Release hardening (`HPPS_RELEASE`), saves in `<exe dir>\saves` | build, `kernel_path.c` | debug build | debug keys/tools absent in release; save location |
