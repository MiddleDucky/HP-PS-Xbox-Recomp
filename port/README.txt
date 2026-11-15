HARRY POTTER AND THE PHILOSOPHER'S STONE - PC PORT
==================================================

The original Xbox game, running natively on Windows.
No game files are included. You need your own copy of the Xbox disc.
Both discs work: the European "Philosopher's Stone" and the American
"Sorcerer's Stone".


WHAT YOU NEED
-------------

  Windows:   10 or 11, 64-bit
  Processor: 4 threads with SSE4.1 (Intel Core 2nd generation, AMD FX or newer)
  Memory:    4 GB
  Graphics:  a DirectX 12 card with an up-to-date driver
             (NVIDIA GTX 600, AMD Radeon HD 7000, Intel HD 500 or newer)
  Space:     about 3.1 GB
  Controls:  keyboard and mouse, or an Xbox controller (360, One or Series)

Windows "N" editions also need Microsoft's Media Feature Pack.


INSTALLING
----------

1. Unzip the port into any folder.
2. Make a folder called "game" next to hpps_launcher.exe.
3. Copy all the files from your game disc into it. You can also put your
   .iso or .xiso file there instead: the launcher unpacks it for you.
4. Start hpps_launcher.exe.

The first time, the launcher checks that your game files are complete. This
takes a few seconds. If a file is missing or damaged, it tells you which one.

Your folder will then look like this:

    hpps_launcher.exe   start the game with this
    game\               your game files
    saves\              your saved games (made on the first run)
    bin\                the port itself, your settings and logs
    licenses\
    README.txt


UPDATING FROM AN OLDER VERSION
------------------------------

Unzip the new version over the old one. Your saves and settings are kept.
If you are coming from a version before 0.5.0, you can delete the old
hpps.exe next to the launcher. It is not used any more.


PLAYING
-------

Always start the game with hpps_launcher.exe.

The launcher lets you pick the screen shape (4:3 or 16:9), the language,
graphics and controls before you press Play. If you hide the launcher page,
hold Shift while starting it to see it again.

To change settings while you play, open "PC Settings". It is the first row
in the game's Options menu, and a row in the pause menu. Most changes work
straight away.

To quit, choose "Quit To Desktop" in the main menu or pause menu, or press
Alt+F4. Alt+Enter switches between a window and borderless full screen.


CONTROLS
--------

Controller
  Plays like the original Xbox controller. The original White and Black
  buttons are LB and RB.

Keyboard and mouse (the default keys)
  Move                  W A S D
  Look around           mouse, or the arrow keys
  A                     Space
  B                     E
  X                     left mouse button
  Y                     right mouse button
  Black / White         R / Q
  Left / right trigger  Shift / Ctrl
  D-pad                 1 2 3 4 (up, down, left, right)
  Start / Back          Escape / Tab
  Stick clicks          C (left), middle mouse button (right)
  Sneak on / off        Caps Lock
  Walking speed         mouse wheel (Sneak, Walk or Run)

On a controller, push the left stick gently to sneak.

Keyboard and controller can both be used. To play with keyboard and mouse
while a controller is plugged in, turn off "Use controller".

Changing keys and buttons
  Open bin\hpps.toml in Notepad while the game is closed.
  Keys:     key_a, key_b, key_move_forward, key_camera_up, key_sneak, ...
            Write a key name (Space, E, Shift, F1, Num0) or Mouse1 to Mouse5.
  Buttons:  pad_a, pad_b, ... say which controller button does what.
  Sneak:    pad_sneak picks a controller button that turns sneaking on/off.


SETTINGS
--------

These are in the launcher and in PC Settings. Settings marked * take effect
the next time you start the game.

Display
  Aspect ratio *       4:3 (bars at the sides), 16:9 (wider view) or Stretch
  Display mode         Window, Borderless (fills the screen) or Fullscreen
  Resolution           the window size or the full screen resolution
  Vsync                stops the picture tearing
  Frame rate           60 fps (normal), 30 fps, or Unlocked (experimental)

Graphics
  Internal resolution  how sharp the 3D is. 480 is the Xbox's own. Higher
                       is sharper but needs a faster graphics card.
  Render scale,
  Max render height    used when Internal resolution follows the window
  MSAA, SMAA           smooth jagged edges
  Anisotropic filter   sharper textures on floors and walls
  Brightness, Contrast 50 means unchanged
  Field of view        80 is the default. Wider can make the camera get
                       stuck in some places; lower it again if that happens.
  FPS counter          shows the frame rate

Post-processing
  Bloom                a soft glow around bright light (off by default)
  Sharpening           a crisper picture (off by default)

Audio
  Speakers *           Stereo or 5.1 surround. For 5.1, set Windows' sound
                       to 5.1 too. If your amplifier already makes surround
                       from stereo, keep Stereo.

Controls
  Keyboard and mouse   on or off
  Button prompts       show keyboard keys or controller buttons on screen.
                       Automatic shows whichever you used last.
  Mouse sensitivity, Invert mouse Y
  Movement keys        how fast W A S D move Harry: Sneak, Walk or Run
  Use controller       off: the controller is ignored
  Deadzone             how far a stick moves before it counts
  Sensitivity          how strongly the stick responds
  Invert camera X / Y
  Vibration            on or off, and how strong

Accessibility
  Colour filter        helps with red-weak, green-weak or blue-weak vision

Enhancements
  Bean flavours        Harry says the flavour of each bean he picks up
                       (on by default)
  Free camera          move the stick or mouse up and down to look up and
                       down. Off: up and down move the camera closer or
                       further away, as on the Xbox. (on by default)
  Cutscene bars        16:9 only: keep or remove the black bars in cutscenes
  Skip logo movies     skip the Warner Bros. and EA logos at the start


IF SOMETHING GOES WRONG
-----------------------

The launcher says game files are damaged or missing
  Copy those files from your disc again. Or put your .iso / .xiso file in
  the game folder and press "Repair" in the launcher.

"Failed to load default.xbe"
  The game files are not where the port expects them. Check that
  game\default.xbe is there, next to hpps_launcher.exe.

The game does not start, or closes straight away
  Update your graphics driver. If that does not help, report the problem
  and attach the two newest log files from bin\logs\runs.

A short pause the first time you enter a new area
  This is normal. The port prepares the graphics for your card the first
  time. Next time it loads at once.

Reporting a problem
  Every start writes a log to bin\logs\runs. Attach the newest
  hpps_run_... and xbox_kernel_... files to your report.
  "Extra debug traces" in the launcher adds more detail to the log.


LICENCES AND SOURCE CODE
------------------------

The port's own code is under the MIT licence. It also contains code from the
xemu project (the Xbox audio processor emulation), which stays under the GNU
Lesser General Public License, version 2.1 or later. The licence texts and the
list of third-party code are in the licenses folder.

The complete source code, including that code and any changes made to it, is
available at:
    https://github.com/MiddleDucky/HP-PS-Xbox-Recomp

The Harry Potter game itself is not included and is not covered by these
licences.
