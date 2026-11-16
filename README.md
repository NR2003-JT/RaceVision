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
- **Overlay layouts.** Every overlay is a layout file you can edit: drag its elements around, resize them, and choose
  the data that feeds each one (driver, place, speed, RPM, gear, throttle, brake, last / best lap, gap to the car ahead
  or the leader, battle gap, the race story, any car by running position and more). Use PNG backgrounds drawn in GIMP
  or any image editor and your own fonts, and build tickers and extra boxes on four custom panels. See "Overlay layouts" below.
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
  `racevision_camsets.cpp` and `racevision_dyncams.cpp`; `racevision_layout.cpp` is the overlay layout engine and
  its editor calls; `racevision_phone.html`, `racevision_director.html` and `racevision_overlays.html` are the browser pages.
- `camsets\` camera sets and the menu script.
- `tools\` camera test scripts.

## Overlay layouts

The overlays are drawn from layout files in `layouts\` next to `RaceVision.exe` (written on first start; the built-in
looks `tag_classic`, `tag_dash`, `battle_classic`, `battle_dash`, `onboard_classic` and `dash_dash` are layouts too).
Open the editor from the Overlays page (Open layout editor) or the Director page (Overlays button). The picture on the
editor's stage is RaceVision's own drawing of the layout, so it matches the screen. A layout is JSON: a size and a list
of elements.

- Elements: `text`, `run` (text in several colours), `rect` (box, rounded or slanted, gradient, border), `image`
  (a PNG from `layouts\images`, or a driver's number card / headshot), `portrait`, `bar`, `segbar`, `arc`, `ordinal`
  ("12TH"). Each has x, y, w, h, an opacity and an optional `show` rule (for example `story.any !replay`).
- Data: text holds `{tokens}` such as `{car.last}`, `{car.speed_mph:0}` or `{p3.best_lap}`; bars and arcs take a
  number source such as `car.throttle`. Cars are `car` (on screen), `other`, `top` / `bottom` (the battle box rows),
  `leader`, `ahead`, `behind` and `p1`...`p99` (running position). Fields are listed in the editor's pickers.
- Colours: `#rrggbb`, `#rrggbbaa` or tokens such as `@gold`, `@white`, `@car` (the driver's paint colour), `@dash`.
- Pictures: PNG / JPG from GIMP go in `layouts\images` (the editor's picture picker uploads them) or come from the selected art
  folder (the picker lists its images; number cards and headshots have their own elements). A `bar` can be made of two pictures
  (empty and full) that fill left, right, up or down; a `dial` fills a ring picture around its centre; a `needle` turns a picture
  about a pivot. The driver `portrait` has an optional number banner (off by default in the built-ins; use a Picture set to
  Number card to place the number yourself).
- Fonts: `fontpack\` (18 free OFL fonts, licences included) is copied into `layouts\fonts` on first start; any font you add there
  shows up in the editor's font list, which draws every font in its own face.
- Editor helpers: grid lines and snap, pink alignment guides to the other elements and the overlay's edges and centre, teal
  distances to the edges, align buttons, drag-to-reorder layers, and dividers you can drag to resize the panels.
- Screen view: the editor opens on it. Each slot (`tag`, `battle`, `onboard`, `dash`, `rc_qual`, `rc_tele`, `rc_cone`, `rc_order`,
  `custom1`...`4`) gets a layout, Automatic or Off, and is dragged to where it should sit on a picture of the screen (wheel to
  resize). Changes apply straight away through the ini keys `layout_<slot>` (`off` = switched off) and `overlay_<slot>`.
- Presets: a preset (`layouts\presets\*.json`) holds `layouts` (the layout for each slot, or `off`) and `screen` (`[x, y, scale]`
  for each slot). Picking one in the Screen view, or in the window's Style box, applies it. Dash and Classic stay as the two
  built-in looks and only change the look, not positions. A preset without `screen` (the three shipped ones) only sets layouts.
- Overlay pack: `layouts_stock\` (the RaceVision_* layouts, alternates, their art and three style presets) and `fontpack\` ship beside
  the exe; RaceVision copies what your layouts folder lacks into it once.
- Groups and locks: in the editor, Shift-click selects several elements; Group (Ctrl+G) makes them move together, Lock (Ctrl+L) pins
  them in place.
- Where they show: the Screen view (ini keys `layout_tag`, `layout_battle`, `layout_onboard`, `layout_dash`,
  `layout_custom1`...`4`, `layout_rc_*`). A custom or RaceControl panel shows by its layout's `when` (`always`, `race`, `green`,
  `onboard`, `offboard`, `battle`, `replay`, `rc.qual`, `rc.tele`, `rc.cone`; all words must hold). Move the panels on screen with Ctrl+F10 as before.
- RaceControl overlays: RaceControl 3.3.13+ writes `%PROGRAMDATA%\NR2003 Companions\racecontrol\overlay_feed.json` (about 4 reads a
  second, ignored when `written_ms` is over 3 s old). Every field is a data source: `{rc.qual.pole_time}`, `{rc.tele.mph}`,
  `{rc.tele.order.1.last}`, `{rc.cone.inside.1.num}` (lists count from 1). The four `rc_*` slots are off until a layout is picked; set
  RaceControl's `OverlaysDrawnBy` to `RaceVision` so it doesn't draw its own. Starter layouts: `RaceVision_RC_*` in the pack.
- Blowovers: a car must be both `blowover_m` above its line at that spot last lap and moving up or down at `blowover_vz` m/s within
  the last 3 s. Without the second test, a car in another lane of a banked track looks like a flip.
- Fonts (`.ttf`, `.otf`) go in `layouts\fonts` and are read when RaceVision starts. A hand-edited layout file is picked up
  within a second.

## How it was built

RaceVision was written with AI assistance (Claude), working from live NR2003 sessions. Memory addresses and camera
structures were found by probing the running game and are documented where they are used. Features were tested in
live races and by testers.

## Credits

- **Jonathon Thomas**: RaceVision.

## License

MIT, see [LICENSE](LICENSE). Free to use, copy, modify and share, with the copyright notice kept.
