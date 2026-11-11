RaceVision camera sets
======================
Double-click "Camera sets.cmd": pick a track (or your series' car cameras) by number, then a set by number.
NR2003 reads camera files when a session loads, so a change shows from the next session you load.
Everything the game had is kept: the first time a file is changed, its original is saved in stock\ (made on the spot).
"r" in the menu (or the "stock" set) puts the original back.

CAR CAMERAS (series\<series>\make_a.cam - the 6 car-mounted views; only the right side / left side views change)
  tv onboard          right side = door cam turned in (door, hood, right front tyre in the picture);
                      left side = roof cam on the front of the roof looking back down the roof at the cars behind
  tv onboard + nose   the same, and the front bumper camera turned round to look back up the hood at the driver
  external            outside the car, fixed to it: low front quarter (right side) and rear quarter (left side)
  external 2          outside the car, fixed to it: broadside from 5.5 m (right side), head-on from 6 m ahead (left)
  mix                 tv door cam (right side) + low front quarter outside (left side)
  broadcast / broadcast tight   the first try (door cam straight ahead, roof cam looking back) - doesn't show the car

TRACK CAMERAS (tracks\<track>\<track>.cam - TV1, TV2, spectator, pit lane)
  tv more cuts        TV1 also gets TV2's following cameras: about twice the cameras, cuts like a TV broadcast
  tv more cuts tight  the same with a tighter lens
  tv tight / tv wide  TV1 and TV2 a third tighter / wider
  tv from <track>     TV cameras borrowed from another version of the same track (same lap length). Only
                      bristol_night_RR <- bristol_revamped is the same track build; the others are experiments
                      (a camera may sit behind scenery there).
Charlotte_2012_SDT_Night_CE and Riverside Cup WKC keep their cameras inside the track .dat; their originals were
taken out with DatX (one of the NR2003 editing tools), putting a set on writes a loose .cam next to the .dat (the
game prefers it), and "back to the game's own" deletes that loose file again.

Commands (if you ever want them, in this folder):
  python camsets.py list
  python camsets.py apply series "FCRD NCS22 v1" "tv onboard"
  python camsets.py restore track Talladega_2023
  python camsets.py saveall track <track folder>        the usual track sets for another track
  python camsets.py borrow <track> <other track>        TV cameras from another version of that track
  python camsets.py dump <any .cam file>                every camera in it as text
Presets are plain text (presets\<kind>\<name>\*.txt), one camera per line, and can be edited by hand.
Cam Edit 1.00b (one of the NR2003 editing tools) edits the game's track camera files by hand too.

In RaceVision's director view (the "Camera sets" button) the same sets are a list: Preview runs a simulated race through a set on the
track's own map, "Use now" switches the cameras NR2003 is showing this moment (no session reload) and keeps the choice for the
next sessions, "Save for next session" only writes the file. This menu and the director view share the same files.
