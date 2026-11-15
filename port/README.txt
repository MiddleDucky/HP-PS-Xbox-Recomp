HARRY POTTER AND THE PHILOSOPHER'S STONE - PC PORT
==================================================

A native Windows port of the original Xbox game, made by static recompilation.
It contains no game data: you need the files from your own copy of the Xbox disc.


MINIMUM SYSTEM REQUIREMENTS
---------------------------

  OS:       Windows 10 or 11, 64-bit
  CPU:      4 threads with SSE4.1 (Intel Core 2nd generation / AMD FX or newer)
  Memory:   4 GB RAM
  Graphics: DirectX 12 card (feature level 11_0) with a current driver:
            NVIDIA GeForce GTX 600 series, AMD Radeon HD 7000 series,
            Intel HD Graphics 500 series or newer
  Storage:  about 3.1 GB (3 GB game files + the port)
  Input:    keyboard and mouse, or an Xbox controller (360, One or Series)

Windows "N" editions also need Microsoft's Media Feature Pack.


INSTALLING
----------

1. Put the port's files in a folder of your choice:
       hpps_launcher.exe
       bin\            the game itself (hpps.exe, hpps_pal.dll, hpps_ntsc.dll,
                       shadercache\base_shaders.bin)
       README.txt
       licenses\
2. Copy the game files from your disc or the .xiso file into a folder named "game" beside hpps_launcher.exe.
   Either disc works: the PAL "Philosopher's Stone" or the NTSC-US
   "Sorcerer's Stone"; the launcher sees which one is there.
   The launcher then checks every game file against the disc's once (a few
   seconds, longer on a hard drive) and names any that are damaged or
   missing; "Verify game files" on its page checks them again.

Updating from an earlier version: copy the files above over the old ones and
keep your saves\. A hpps.toml and shadercache\ from before bin\ are moved
into bin\ by the launcher the first time it starts (your settings and the
shaders already built on your PC stay as they are); the old hpps.exe beside
the launcher is no longer used and can be deleted.

The port creates these itself on the first run:
    saves\             your saved games (beside the launcher)
    bin\hpps.toml      your settings (the launcher and F10 change them)
    bin\shadercache\   shaders built on your PC (the first visit to an area
                       prepares them; later visits load at once)
    bin\logs\          the run logs


PLAYING
-------

Start the game with hpps_launcher.exe. Opening bin\hpps.exe directly only
offers to open the launcher.

The launcher sets the aspect ratio (4:3 or 16:9), display, graphics,
controls and the language, then starts the game with Play. Language: the
game starts in English, French, German, Spanish, Italian, Dutch or Swedish,
the seven the PAL disc has speech for, or English or Spanish on the US disc
(the game's own menu can still change it). If you turn its page off, the
launcher starts the game straight away; hold Shift while opening it to see
the page again.

Only one copy of the game runs at a time: starting it again brings the
running game to the front.

Quitting: "Quit To Desktop" in the main menu or the pause menu, or Alt+F4.

"PC Settings", the first row of the game's Options and a row in the pause
menu, opens the F10 pages below in the game's own menu, for a controller.

Keys that always work
  F10          Options (see below)
  Alt+Enter    switch between windowed and borderless
  Alt+F4       quit the game

Controller
  An Xbox 360 / One / Series controller plays like the original Xbox pad.
  The original pad's White and Black buttons are LB and RB.
  To play with keyboard and mouse while a controller is connected, turn
  off "Use controller" (launcher or F10 > Controls).

Keyboard and mouse (defaults; change them under F10 > Controls)
  Move                  W A S D
  Camera                mouse, or the arrow keys
  A                     Space
  B                     E
  X                     left mouse button
  Y                     right mouse button
  Black / White         R / Q
  Left / right trigger  Shift / Ctrl
  D-pad                 1 2 3 4 (up, down, left, right)
  Start / Back          Escape / Tab
  Stick clicks          C (left), middle mouse button (right)
  Sneak (toggle)        Caps Lock: Harry keeps his sneaking pace until you
                        press it again ("Sneaking" shows in the corner)
  Pace of the keys      mouse wheel: down Sneak < Walk < Run up (Run is the
                        default; also F10 > Controls > Movement keys)

Sneaking: on a controller, push the left stick gently. Keys always move at
full speed, so use the sneak toggle; a controller button can toggle it too
(F10 > Controls > Sneak toggle button).


OPTIONS (F10)
-------------

F10 opens the options over the game; most changes apply at once and are saved
to bin\hpps.toml. "Defaults (this page)" resets the page you are on (Graphics,
Controls, ...) and leaves the others; the aspect ratio stays. "Show the
launcher at startup" turns the launcher's page on or off.

Graphics
  Display
    Aspect ratio         4:3 (black bars at the sides), 16:9 (the game's
                         camera widened; menus and movies keep 4:3) or
                         Stretch. Applies at the next start.
    Cutscene bars        (16:9) cutscenes fill the screen and keep the
                         original black bars over the picture, with the
                         subtitles in the bottom bar
    Display mode         Windowed, Borderless (covers the monitor) or
                         Fullscreen (switches the monitor's resolution)
    Window size /
    Resolution           the window's size, or the fullscreen mode; Desktop
                         uses the monitor's own
    Vsync                waits for the monitor's refresh (no tearing)
    Frame rate           the game's rate (60 fps), 30 fps, or Unlocked
                         (experimental: the game was made for 60, some timing
                         may misbehave)
  Resolution
    Internal resolution  the lines the 3D is drawn at: 480 is the Xbox's own;
                         720 to 2160 sharpen it; "Follow window" matches the
                         window
    Render scale %       with "Follow window": the render size as a share of
                         the window's
    Max render height    with "Follow window": the most lines it follows the
                         window to (1080 by default, so a 4K screen keeps
                         60 fps on modest cards)
  Anti-aliasing and filtering
    MSAA                 smooths polygon edges: off, 2x, 4x, 8x
    SMAA                 smooths edges over the finished picture; combines
                         with MSAA
    Anisotropic filtering  sharper textures at an angle: the game's own, 2x,
                         4x, 8x or 16x
  Picture
    Brightness, Contrast 50 leaves the picture as the game draws it
    FPS counter          shows the frame rate
  Post-processing
    Bloom, Bloom strength  a glow around bright parts of the 3D scene (not
                         the HUD), off by default
    Sharpening           contrast-adaptive sharpening, off by default
    Field of view (experimental)
                         80 to 130 % of the game's; 80 by default. A wider
                         view can make the camera stick at some places
                         (it stays put while Harry moves on); lower it if
                         that happens

Controls
  Keyboard and mouse
    Keyboard and mouse play as controller 1 - alongside a controller
    Mouse sensitivity, Invert mouse Y
    Movement keys        the pace W A S D move Harry at: Sneak, Walk or Run;
                         the mouse wheel steps it in game
    Key bindings         click a binding, then press a key or mouse button
                         (Escape clears it); "Default keys" restores them
  Controller
    Use controller       off: the game ignores the controller (and it does
                         not vibrate); keyboard and mouse still play
    Left / right stick deadzone %      how far a stick moves before it counts
    Left / right stick sensitivity %   the response past the deadzone
    Invert camera X / Y
    Sneak toggle button  a controller button that toggles sneaking (it then
                         does nothing else), or None
    Vibration, Vibration strength %
    Button remapping     which pad button each game button reads;
                         "Default buttons" restores them

Audio
  Speakers               Stereo, or 5.1 surround. The game is stereo on the
                         console too; 5.1 spreads it over six speakers as a
                         Pro Logic receiver does (centre, back pair, LFE), for
                         analog 5.1 speakers. Needs Windows' speaker setup set
                         to 5.1. With a receiver that upmixes by itself
                         (S/PDIF, HDMI), Stereo is enough. Applies at the next
                         start.

Accessibility
  Colour blindness
    Correction filter    Off, Protanopia (red-weak), Deuteranopia
                         (green-weak) or Tritanopia (blue-weak): shifts the
                         colours you confuse toward ones you can tell apart
    Strength %

Enhancements
  Bertie Bott's beans
    Announce bean flavours on pickup
                         off: the flavour is no longer said aloud when Harry
                         picks up a bean (on by default)
  Camera
    Free camera          up and down on the right stick or the mouse tilt the
                         camera up and down around Harry (on by default); off:
                         they move it nearer and further, as on the console
  Cutscenes
    Cutscenes without black bars
                         16:9: cutscenes fill the screen; off: the console's
                         bars over the picture
  Startup
    Skip the Warner Bros. and EA logos   off by default

Debug
  Logs                   every start from the launcher writes a log to
                         bin\logs\runs (hpps_run_<date_time>.log, with the
                         kernel's xbox_kernel_<date_time>.log beside it); the
                         newest 10 runs are kept. Send both with a report.
    Open logs folder     opens bin\logs\runs
    Mark this moment in the log
                         writes a numbered [MARK] line with where Harry is
                         and what he is doing: press it right after something
                         goes wrong and give the number with your report
    Extra debug traces   more detail in the log; from the next start


TROUBLESHOOTING
---------------

The launcher says game files are damaged or missing
    Copy those files again from your disc or game dump, or put the disc
    image (.iso / .xiso) in the game folder and press "Repair" on the
    launcher's page. A damaged file can crash the game or lose sound or
    pictures where it is used.

"Failed to load default.xbe"
    The game files are missing: check that game\default.xbe exists beside
    hpps_launcher.exe.

The game will not start / closes at once
    Update your graphics driver (DirectX 12 is required). If it still fails,
    send the newest hpps_run_<date_time>.log and xbox_kernel_<date_time>.log
    from bin\logs\runs.

A short pause the first time you enter an area
    Shaders are being prepared for your graphics card; the loading picture
    stays up meanwhile. It happens once per area. From the next start the
    port prepares everything it has seen ahead, while you are in the menus
    ("Preparing shaders" shows in the corner), so those areas load at once.


LICENCES AND SOURCE CODE
------------------------

The port's own code is under the MIT licence. It also contains code from the
xemu project (the Xbox audio processor emulation), which stays under the GNU
Lesser General Public License, version 2.1 or later. The licence texts and the
list of third-party code are in the licenses folder beside this file.

The complete source code, including that code and any changes made to it, is
available at:
    https://github.com/MiddleDucky/HP-PS-Xbox-Recomp

The Harry Potter game itself is not included and is not covered by these
licences.
