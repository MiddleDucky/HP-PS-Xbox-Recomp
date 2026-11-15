# Changes

## v0.5.0 (2026-11-15), compared with v0.4.2

### New
- **The NTSC-US disc, *Harry Potter and the Sorcerer's Stone*, plays too.** Put either disc's files in `game\`: the launcher sees which one it is, checks the files against that disc's list, shows its title and languages (English and Spanish on the US disc) and starts the matching game. Saves, settings and options are shared.
- **Free camera** (F10 > Enhancements, on by default). Up and down on the right stick or the mouse now tilt the camera up and down around Harry, from below his head to high above him, as left and right already turn it. On the console they only moved the camera nearer and further in four steps; turn the option off to get that back. The tilt eases back by itself in cutscenes and scripted camera shots, and the game's camera collision still keeps it out of walls.
- **PC Settings in the game's own menus**: a new first row in Options and a row under Save Game in the pause menu open the F10 pages (Display, Graphics, Post-processing, Audio, Controls, Accessibility, Enhancements) drawn in the game's font, so a controller can change them without F10. Key bindings, button remapping and Debug stay in F10.
- **Keyboard prompts** (F10 > Controls > Button prompts). While you play with keyboard and mouse, the game's Xbox button pictures are drawn as the keys you bound, and its text names them: "Press 'Space' to continue". Automatic (the default) follows whichever device you used last.
- **Skip the Warner Bros. and EA logos** (F10 > Enhancements, off by default).
- **Resolution lists reach your screen's biggest mode** (launcher and F10), including 16:10 and ultrawide sizes, marked "(screen)"; internal resolution goes up to the screen's own height.

### Changes
- **New folder layout.** The launcher, `game\`, `saves\`, `licenses\` and the README stay at the top; the game itself (`hpps.exe` and one DLL per disc) goes in `bin\`, with your settings (`bin\hpps.toml`), the shader cache and the logs. Copy the new files over the old ones: the launcher moves an old `hpps.toml` and `shadercache\` into `bin\` the first time it starts, and the old `hpps.exe` at the top can be deleted.
- **5.1 surround** now spreads the game's stereo over six speakers as a Pro Logic receiver does (the game is stereo on the console too), for analog 5.1 speakers. With a receiver that upmixes by itself, Stereo is enough.
- "Cutscenes without black bars" is now under F10 > Enhancements.
- The pupil standing in mid-air beside the cloisters corridor wall is no longer hidden.

### Fixes
- **No more long black screens before cutscenes** (issue #11; up to about three minutes before the flying lesson). The game's loading thread now runs at the priority it has on the console, so the cutscene's characters load at once.

## v0.4.2 (2026-11-05), hotfix

### Fixes
- **No more false "disc dirty or damaged" error in long sessions.** The game opens a new handle every time the controller rumbles and never closes it, which the Xbox allows. The port had room for only 16,384 of them, so after a few hours of play the next file the game opened failed, and the game blamed the disc. Your game files were fine. The port now keeps handles the way the Xbox kernel does, with no fixed limit.

## v0.4.1 (2026-11-02), hotfix

### Fixes
- **Rooms no longer go black from the Mirror of Erised cutscene on** (issue #6). From that cutscene in the night after Diffindo, only Harry, Peeves and the eyes were drawn, and the room stayed black afterwards. The cause was the port's own fix that hides the pupil floating beside the cloisters corridor wall: it hid every mesh standing where she stood, and in that part of the game she is put away at the very spot all the room's meshes share. It now leaves that spot alone, and never hides more than one character's worth of meshes.

### Changes
- **Field of view is 80 % by default** (F10 > Graphics). In 16:9 that keeps the camera's width close to the console's, which the game's camera relies on: with a wider view the camera could stick at one place while Harry moved on. F10 now says so under the slider. Settings saved before keep their value; F10's Defaults sets 80.

## v0.4.0 (2026-11-02), compared with v0.3.0

### New options
- **Language** (launcher): the game starts in English, French, German, Spanish, Italian, Dutch or Swedish, the seven the disc has speech for. Dutch and Swedish, which no Xbox dashboard offers, can be chosen too; the game's own menu can still change it.

### Smoother play
- **Movies and the main menu run at full speed.** The movie player no longer spins on the game's CPU core while it waits for the next frame, which held back the game's own clock: intro movies and the main menu could drop to about 20 fps on Windows.
- **Timing and threading fixes from the Chamber of Secrets port.** A game timer could miss a wake-up and stop now and then (in Chamber of Secrets it silenced the level music); the game's threads now share their CPU core as they do there.

### Fixes
- **16:9 free look:** looking up no longer moves the life bar and the rest of the HUD to their 4:3 places.
- **No more crash during a long pause** (for example the save check after answering "no" to loading a save). The crash came from the port's own diagnostics.
- **The retail DVD's untouched `default.xbe`** passes the launcher's file check too.

### Logs
- **A log every run.** The launcher writes `logs\runs\hpps_run_<date_time>.log` and `xbox_kernel_<date_time>.log` each time, keeping the newest 10, so a problem always leaves a log. "Write debug logs" is now **Extra debug traces** and only adds detail.
- **F10 > Debug:** "Open logs folder" and **"Mark this moment in the log"**, which writes a numbered mark with where Harry is and what he is doing. Press it right after something goes wrong and give the number with your report.
- **Crash reports name the faulting module and thread**, and a long pause logs what the game was doing.

## v0.3.0 (2026-10-30), compared with v0.2.1

### New options
- **Bloom and sharpening** (F10 > Graphics > Post-processing), both off by default.
- **Field of view slider (experimental)** (F10 > Graphics), 80 to 130 % of the game's.
- **Announce bean flavours on pickup** (F10 > Enhancements, on by default). Turn it off and the flavour is no longer said aloud when Harry picks up a bean.

### The launcher
- **Checks your game files.** On the first start, after unpacking a disc image, and from "Verify game files", every file is compared with the disc's; damaged or missing ones are named, and "Repair" writes them again from a disc image in the game folder. Later starts only check the sizes.
- **A new look:** the title in the Magic School One font over a Gryffindor scarlet backdrop, with no game artwork (the old main-menu picture is gone from every build); the exes have a wand icon in the same colours.
- **No more crash under Wine** when a DXVK `dxgi.dll` sits beside the exes.
- **`show_launcher` is the first setting in `hpps.toml`**, to bring the page back (or hold Shift, or F10).

### Fixes
- **A crash leaves `hpps_crash.log`** beside the game even with the debug logs off.
- **Smoother camera:** the game's timer runs at 3000 ticks a second from the start, which removed a camera judder.
- **Starts under Wine 11** and other setups where the usual memory address is taken.
- **Another edition's game files are turned away with a clear message** instead of the game stopping at once (only the PAL release works), and a start-up fault that overwrote some of the game's data is fixed.

## v0.2.1 (2026-10-25), compared with v0.2.0

- **F10's Defaults resets only the page you're on.** On Graphics it resets the graphics settings, on Controls the controls, and so on; the other pages keep your settings. Before, it reset every page at once. The aspect ratio is still kept, as before.
- **File properties:** both exes now show the product name, version, and copyright under Properties > Details.

## v0.2.0 (2026-10-25), compared with v0.1.0 (2026-10-20)

### New options
- **5.1 surround sound** (F10 > Audio > Speakers). The console's own speaker mix is played with each channel on its own speaker, instead of folded down to stereo. It needs Windows' speaker setup set to 5.1, or a 5.1 HDMI receiver, and applies at the next start. Stereo is still the default and mixes exactly as before.
- **Movement pace for the keyboard** (mouse wheel, or F10 > Controls > Movement keys): Sneak, Walk or Run, as in the Chamber of Secrets port. The movement keys always pushed the stick all the way, so Harry could only run, or sneak with the Caps Lock toggle. The wheel now steps the keys' pace down to Walk and Sneak and back up. The Sneak and Walk speeds can be set under F10 > Controls; the Caps Lock sneak toggle uses the Sneak speed too, and sneaks a little quicker than before. Run is the default, so nothing changes until you turn the wheel.
- **Use controller** (launcher and F10 > Controls). Turn it off to play with keyboard and mouse while a controller is plugged in. The game then ignores the controller and doesn't make it vibrate.

### Smoother play
- **Shaders prepared ahead.** Every effect the game has drawn on your PC is recorded (`shadercache\pipelines_list.bin`). From the next start, it is all prepared in the background while you're in the menus ("Preparing shaders" shows in the corner). Areas you've visited before then load without the first-visit pause.
- **Game clock ticks every millisecond,** as the console's does. Windows' own timer moves in steps of about 15.6 ms, which made the game's timing slightly uneven.
- **Freed memory is given back,** as the console does. The game no longer retries the same memory request thousands of times in a session.
- **Softer sound starts.** A sound that starts or stops now fades over about 5 ms, as the console's audio chip smooths it, instead of clicking.

### Fixes
- **Saved games' times:** saves are stamped with your PC's local time and time zone. Before, every save was stamped as if the player were in UTC+5.
- **Game region:** the port now reports the region the game disc is made for, as a console of that region would. Before, it reported no region at all.
- **Cache partition:** the console's hard-disk cache for the game is kept between starts instead of being formatted at every start. The first start after updating formats it one last time; saves aren't affected.
- **Lines** are drawn the way the original console draws them, one console pixel wide on console pixel edges. Thin line-drawn details no longer come out slightly offset or too thin at high resolutions.
- **Scrolling 2D layers** (fog and similar effects) no longer snap to a single texel after a long session.
- **Folder listings:** a search with an empty pattern lists every entry, as the console's file system does.
- **Startup window** is black until the first frame, instead of white.

### Removed
- **Ubershaders,** an option tried in test builds only. They made the first start slower on some graphics cards (GTX 1060); preparing shaders ahead, above, replaces them.

### Updating
Replace `hpps.exe`, `hpps_launcher.exe`, `README.txt`, `licenses\` and `shadercache\base_shaders.bin`. Keep your `hpps.toml`, `saves\` and the rest of `shadercache\`.
