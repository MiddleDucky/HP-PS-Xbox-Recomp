# Harry Potter and the Philosopher's Stone (PAL/NTSC) — Xbox to PC port

A native Windows port of the original Xbox game *Harry Potter and the Philosopher's Stone* (2003), made by
**static recompilation**: the game's x86 code is translated to C and compiled for Windows, with the Xbox kernel,
Direct3D 8, DirectSound and input replaced by native code. No emulator.

Features beyond the original: resolutions up to 4K with MSAA/SMAA and anisotropic filtering, true 16:9
(widened 3D, 2D kept in shape), windowed/borderless/fullscreen, keyboard and mouse with rebinding, controller
remapping, deadzones and vibration strength, colour-blind correction filters, a 30 fps cap or unlocked frame
rate, and "Quit To Desktop" in the game's menus.

**This repository contains no game content.** To play or build it you need the files from your own copy of the
Xbox disc. The build regenerates the recompiled code from your `default.xbe`.

## Playing

Download the repository, build the executables and put your disc's files or the .xiso in a `game` folder beside the executables and start
`hpps_launcher.exe`. Requirements, controls and every option are in [port/README.txt](port/README.txt).

## Building

See [port/DEVELOPING.md](port/DEVELOPING.md): MSYS2 (UCRT64) setup, regenerating the code from the XBE, building,
debugging tools and how the port works.

## Credits and licences

The port's own code is MIT licensed, © 2026 MiddleDucky ([LICENSE](LICENSE)). The parts below keep their own
licences and copyright.

* **xboxrecomp** — the static recompilation toolkit this port is built on (runtime, kernel layer, pipeline),
  MIT, by sp00nz. Its own readme: [README.xboxrecomp.md](README.xboxrecomp.md); licence [LICENSES/xboxrecomp-MIT.txt](LICENSES/xboxrecomp-MIT.txt);
  third-party parts (including LGPL code from xemu) in [NOTICE](NOTICE) and [LICENSES/](LICENSES/).
* **SCDA-XBERecomp** by TGP482 — the D3D8-to-Direct3D 12 renderer the port's graphics are based on, MIT
  ([port/src/scda/LICENSE.SCDA](port/src/scda/LICENSE.SCDA)).
* **Dear ImGui** (MIT, port/third_party/imgui) and **SMAA** (MIT, Jorge Jimenez et al.).

### Thanks

These projects were references for hardware behaviour, XDK details or fixes
(no code taken unless listed above):

* [xemu](https://github.com/xemu-project/xemu) — Xbox hardware behaviour; the APU code listed in NOTICE
* [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) — XDK function addresses
* [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) and [xboxdevwiki](https://xboxdevwiki.net) — kernel and hardware reference
* [Nightfire Windows Port](https://github.com/limeylicious/Nightfire-Windows-Port) — the same lifter bugs, found first
* [DefJamFFNY-recomp](https://github.com/vyanhursky/DefJamFFNY-recomp) — single-core guest thread pinning
* [Unicorn Engine](https://github.com/unicorn-engine/unicorn) — the debug-only difftest build

*Harry Potter* and all related names and artwork are trademarks of and © Warner Bros. Entertainment Inc. The game
is © Electronic Arts. This is an unofficial fan project, not affiliated with or endorsed by Warner Bros.,
Electronic Arts or Microsoft.

This project has been aided by Claude Code.
