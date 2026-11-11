# RaceVision

An automatic TV director and broadcast overlays for **NASCAR Racing 2003 Season**. RaceVision reads the race straight
from the game's memory and cuts to the best thing happening: crashes, passes and close battles for position, pit
stops, the leader, restarts. It picks the camera for each shot, holds shots long enough to watch them, and draws
broadcast-style graphics over the game.

Download the latest release from the [Releases](../../releases) page. The `README.txt` inside the zip, also kept here
as [USER GUIDE.txt](USER%20GUIDE.txt), explains the window and every setting.

## What it does

- **Directs the race.** Crashes, passes and battles for position, pit stops, the leader and restarts, chosen from the
  official scoring order (laps, then line-crossing time). Battles are only between cars that are neighbours in the
  official order, and the director holds the view for a fight for the lead.
- **Cameras.** Every game camera, plus RaceVision's own: in-car right, drone, side tracking, low rear, front quarter,
  overhead, pack view and a rear view that rides the car ahead. Camera changes go through a hook on the game's
  per-frame view function, so there is no helicopter sound and no race-start crash. Lens, angle and position edits apply live.
- **Camera sets.** Swap whole sets of the game's track and car cameras (`camsets\`). The first time a file is changed
  its original is saved, and the "stock" set puts it back. Presets for several tracks and a series are included.
- **Dynamic cameras.** Static cameras placed on the pit lane camera blocks, learned per track.
- **Replays.** Automatic and manual crash replays with up to three angles, a highlight reel, and a TV-style wipe.
- **Overlays, used sparingly.** A driver tag, a battle box, a qualifying delta, and an in-car dash (place, driver,
  speed arc, RPM) on on-board shots, using each driver's number art, headshot and colour. No running-order tower,
  and no extra airtime for your own car.
- **Phone remote and director view.** Control RaceVision from a phone, or open `:8090/director` in a browser for a
  live track map, car positions, the cut log, and a timeline recorder. Both work on your own network and need no
  internet connection.
- **Presets.** Save and switch every setting; presets are plain `.ini` files.

## Requirements

- Windows 10/11 and NASCAR Racing 2003 Season (NR2003.exe 1.2.0.1).
- RaceVision runs as administrator, because NR2003 normally does and RaceVision reads and writes its memory.
- The graphics need NR2003 running windowed or borderless.
- RaceVision works alongside AIMOD and RaceControl.

## Building from source

Win32 C++, x86, built with Visual Studio's MSVC (a recent Community edition is fine). From a Developer Command Prompt
for x86:

    src\build.cmd

This writes `app\RaceVision.exe`, or `app\RaceVision_new.exe` if RaceVision is running. The phone page and logo are
built into the exe through `racevision.rc`. `RaceVision.exe --selftest out.txt` checks the camera sets and dynamic
cameras without the game running.

## Layout

- `src\tv_director.cpp` the main file. It includes `racevision_ui.cpp` (the window pages), `racevision_remote.cpp`
  (the web server on port 8090) and `racevision_qr.cpp`. Cameras are in `racevision_cams.cpp`,
  `racevision_camsets.cpp` and `racevision_dyncams.cpp`; `racevision_phone.html` and `racevision_director.html` are
  the browser pages.
- `camsets\` camera sets and the menu script.
- `tools\` camera test scripts.

## How it was built

RaceVision was written with AI assistance (Claude), working from live NR2003 sessions. Memory addresses and camera
structures were found by probing the running game and are documented where they are used. Features were tested in
live races and by testers.

## Credits

- **Jonathon Thomas**: RaceVision.

## License

MIT, see [LICENSE](LICENSE).
