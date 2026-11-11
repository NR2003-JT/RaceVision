"""RaceVision camera sets - presets for NR2003's own camera files.

NR2003 reads two kinds of camera file when a session loads (so a preset applies from the next session):
  tracks\\<track>\\<track>.cam   MACT v2: the TV1, TV2 and spectator cameras along the track, plus the 2 pit lane cameras
  series\\<series>\\make_a.cam   MACC: the 6 car-mounted cameras (bumpers, roof, sides). A loose file in the series
                                 folder wins over the one packed in the series .dat.
This tool turns them into text (one camera per line, named fields), builds them back byte for byte, makes new sets
from recipes, and puts a set in place with the game's original kept in camsets\\stock (restore puts it back).

  python camsets.py [menu]                              pick sets by number ('Camera sets.cmd')
  python camsets.py dump  <file.cam>                    text to the screen
  python camsets.py save  track <track> | series <series> <preset name> [recipe]
                                                         stock file (+ a recipe) -> presets\\<kind>\\<who>\\<name>.txt
  python camsets.py saveall track <track> | series <series>   the usual presets (TRACK_SET / SERIES_SET)
  python camsets.py import track <track> <file.cam>     a .cam taken out of the track .dat = its stock
  python camsets.py borrow <track> <other track> [note]  TV cameras from another version of the same track
  python camsets.py apply track <track> | series <series> <preset name>
  python camsets.py restore track <track> | series <series>
  python camsets.py list                                 presets, and what is applied where

Field names follow Cam Edit 1.00b's readme (Christoph Moeller, 2002) where it names them; u.. = not known.
"""
import os, shutil, struct, sys, textwrap

GAME = r"C:\Papyrus\NASCAR Racing 2003 Season"
HERE = os.path.dirname(os.path.abspath(__file__))
STOCK, PRESETS = os.path.join(HERE, "stock"), os.path.join(HERE, "presets")

TV_FIELDS = ["sf", "cl", "height", "takeover", "u4", "zoom_in", "zoom", "zoom_out", "aim", "tilt", "lean", "view",
             "u12", "u13"]           # metres from the S/F line, from the centre line (+ = infield), above ground, ...
PIT_FIELDS = ["x", "y", "z", "yaw", "pitch", "roll", "fov", "u7", "u8"]   # world metres, radians
CAR_FIELDS = ["x", "y", "z", "yaw", "pitch", "roll", "fov", "u7", "u8"]   # metres from the car (x forward, y left)
GROUPS = ["TV1", "TV2", "SPECTATOR"]
CAR_SLOTS = ["front bumper", "rear bumper", "third", "roof", "right side", "left side"]


def f32(v):
    return struct.unpack("<f", struct.pack("<f", v))[0]


# ---- binary <-> dict
def read_cam(path):
    b = open(path, "rb").read()
    magic, ver, size = struct.unpack_from("<4sII", b, 0)
    if magic == b"MACT":
        if ver != 2: raise ValueError(f"{path}: MACT version {ver}, only 2 is known")
        o, groups = 12, []
        for _ in GROUPS:
            n = struct.unpack_from("<I", b, o)[0]; o += 4
            cams = []
            for _ in range(n):
                cams.append({"f": list(struct.unpack_from("<14f", b, o)), "type": b[o + 56]}); o += 57
            groups.append(cams)
        pit = [list(struct.unpack_from("<9f", b, o)), list(struct.unpack_from("<9f", b, o + 36))]; o += 72
        tail = b[o:o + 16]; o += 16
        return {"kind": "track", "groups": groups, "pit": pit, "tail": tail, "extra": b[o:]}
    if magic == b"MACC":
        n = size // 36
        cams = [list(struct.unpack_from("<9f", b, 12 + 36 * k)) for k in range(n)]
        return {"kind": "car", "ver": ver, "cams": cams, "extra": b[12 + 36 * n:]}
    raise ValueError(f"{path}: not a camera file ({magic!r})")


def write_cam(d, path):
    if d["kind"] == "track":
        body = b""
        for cams in d["groups"]:
            body += struct.pack("<I", len(cams))
            for c in cams: body += struct.pack("<14f", *c["f"]) + bytes([c["type"]])
        for p in d["pit"]: body += struct.pack("<9f", *p)
        body += d["tail"]
        out = b"MACT" + struct.pack("<II", 2, len(body)) + body + d["extra"]
    else:
        body = b"".join(struct.pack("<9f", *c) for c in d["cams"])
        out = b"MACC" + struct.pack("<II", d["ver"], len(body)) + body + d["extra"]
    tmp = path + ".tmp"
    with open(tmp, "wb") as f: f.write(out)
    os.replace(tmp, path)


# ---- dict <-> text
def num(v):
    return repr(f32(v))


def to_text(d, title=""):
    L = [f"# RaceVision camera set{(' - ' + title) if title else ''}", f"kind = {d['kind']}"]
    if d["kind"] == "track":
        L.append("# one camera per line; type 1 / 0 = the two camera kinds (fixed / turning in Cam Edit's words)")
        for g, cams in zip(GROUPS, d["groups"]):
            L.append(f"[{g}]")
            for c in cams:
                L.append("cam " + " ".join(f"{k}={num(v)}" for k, v in zip(TV_FIELDS, c["f"])) + f" type={c['type']}")
        L.append("[PIT]")
        for p in d["pit"]: L.append("cam " + " ".join(f"{k}={num(v)}" for k, v in zip(PIT_FIELDS, p)))
        L.append(f"tail = {d['tail'].hex()}")
    else:
        L.append("# the 6 car cameras in file order; x forward, y left, z up (m); yaw + = turned left; fov radians")
        L.append(f"version = {d['ver']}")
        L.append("[CAR]")
        for k, c in enumerate(d["cams"]):
            name = CAR_SLOTS[k] if k < len(CAR_SLOTS) else str(k)
            L.append("cam " + " ".join(f"{f}={num(v)}" for f, v in zip(CAR_FIELDS, c)) + f"   # {name}")
    L.append(f"extra = {d['extra'].hex()}")
    return "\n".join(L) + "\n"


def from_text(t):
    d, group, kind = {"extra": b""}, None, None
    for line in t.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line: continue
        if line.startswith("["): group = line.strip("[]"); continue
        if line.startswith("cam "):
            kv = dict(p.split("=", 1) for p in line[4:].split())
            if group == "CAR": d["cams"].append([float(kv[f]) for f in CAR_FIELDS])
            elif group == "PIT": d["pit"].append([float(kv[f]) for f in PIT_FIELDS])
            else: d["groups"][GROUPS.index(group)].append({"f": [float(kv[f]) for f in TV_FIELDS], "type": int(kv["type"])})
            continue
        k, v = (s.strip() for s in line.split("=", 1))
        if k == "kind":
            kind = d["kind"] = v
            if v == "track": d.update(groups=[[], [], []], pit=[])
            else: d.update(cams=[])
        elif k == "version": d["ver"] = int(v)
        elif k in ("tail", "extra"): d[k] = bytes.fromhex(v)
    if kind == "track" and len(d["pit"]) != 2: raise ValueError("a track set needs exactly 2 pit cameras")
    return d


# ---- recipes: new sets made from a stock one
# Track cameras (checked on every installed track): a TV camera covers the stretch from its own take-over point
# (sf + takeover, takeover < 0 = before the camera) to the next camera's. type 0 = turning (follows the car, zoom_in /
# zoom / zoom_out = lens width in radians as the car comes, passes, goes), type 1 = fixed (aim / tilt / view).
PI = 3.14159265358979


def _scale_lens(d, k):
    for cams in d["groups"][:2]:
        for c in cams:
            for i in (5, 6, 7): c["f"][i] *= k
            if c["type"] == 1: c["f"][11] *= k
    return d


def recipe_tv_tight(d):
    """TV1 / TV2: a lens about a third tighter (x 0.7) - the car fills more of the picture."""
    return _scale_lens(d, 0.7)


def recipe_tv_wide(d):
    """TV1 / TV2: a lens about a third wider (x 1.4) - more of the pack in shot."""
    return _scale_lens(d, 1.4)


def _track_length(d):
    """about the lap length: the furthest camera along, rounded up a little (only used to wrap distances)"""
    sfs = [c["f"][0] for g in d["groups"] for c in g]
    return max(sfs) * 1.02 if sfs else 1000.0


def recipe_tv_more_cuts(d):
    """TV1 gets TV2's following (turning) cameras too: about twice the cameras, so the picture cuts like a TV
    broadcast does. Each camera takes over a little after the car passes the one before (like the game's own sets)."""
    L = _track_length(d)
    pool = [dict(c, f=list(c["f"])) for c in d["groups"][0]]
    for c in d["groups"][1]:
        if c["type"] == 0 and all(abs((c["f"][0] - o["f"][0] + L / 2) % L - L / 2) > 60 for o in pool):
            pool.append(dict(c, f=list(c["f"])))
    pool.sort(key=lambda c: c["f"][0])
    for n, c in enumerate(pool):
        gap = (c["f"][0] - pool[n - 1]["f"][0]) % L or L
        c["f"][3] = max(c["f"][3], -0.65 * gap)        # never earlier than it used to take over
    d["groups"][0] = pool[:64]
    return d


def recipe_tv_more_cuts_tight(d):
    """More cuts, and the tighter lens."""
    return recipe_tv_tight(recipe_tv_more_cuts(d))


# Car cameras: x forward, y left, z up from the car's middle; yaw + turns left (pi = looking back); pitch + looks down.
# Slots 4 and 5 are the stock right / left side cameras (they look out at the scenery); the others stay as they are.
def _car(d):
    cams = d["cams"]
    roof, right, left = cams[3], cams[4], cams[5]
    half = max(abs(right[1]), abs(left[1])) + 0.26                  # the side of the body, about
    half = min(max(half, 0.9), 1.05)
    return cams, roof, right, half, right[7], right[8]


def recipe_car_broadcast(d):
    """First try (2026-10-26): right side = door cam looking straight forward, left side = roof cam looking back.
    Kept for comparison - neither shows much of the car itself."""
    cams, roof, right, half, u7, u8 = _car(d)
    cams[4] = [-0.6, -1.0, max(right[2], 0.2) + 0.3, 0.08, 0.02, 0.0, 1.15, u7, u8]
    cams[5] = [roof[0] + 0.05, 0.0, roof[2] + 0.06, PI, 0.05, 0.0, 1.0, u7, u8]
    return d


def recipe_car_tight(d):
    """First try, and the bumper and roof cameras a longer lens (fov 1.361 -> 1.1 rad, 78 -> 63 degrees)."""
    d = recipe_car_broadcast(d)
    for k in (0, 1, 3): d["cams"][k][6] = 1.1
    return d


def recipe_car_tv_onboard(d):
    """Onboards that show the car: right side = door cam high on the right of the car, turned in so the door,
    hood and right front tyre are in the picture; left side = roof cam on the front of the roof looking back down
    the roof and rear deck at the cars behind."""
    cams, roof, right, half, u7, u8 = _car(d)
    cams[4] = [-0.9, -(half + 0.12), roof[2] - 0.15, 0.20, 0.08, 0.0, 1.2, u7, u8]
    cams[5] = [roof[0] + 0.35, 0.0, roof[2] + 0.10, PI, 0.18, 0.0, 1.25, u7, u8]
    return d


def recipe_car_tv_onboard_nose(d):
    """TV onboards, and the front bumper camera turned round: on the nose looking back up the hood at the
    windshield and the driver."""
    d = recipe_car_tv_onboard(d)
    front, roof = d["cams"][0], d["cams"][3]
    d["cams"][0] = [front[0] - 0.1, 0.0, max(front[2] + 0.55, roof[2] * 0.65), PI, -0.06, 0.0, 1.25, front[7], front[8]]
    return d


def _look_at(cx, cy, cz, fov, u7, u8, tz=0.6):
    """a camera at (cx, cy, cz) on the car, aimed at the middle of the car"""
    import math
    yaw = math.atan2(-cy, -cx)
    pitch = math.atan2(cz - tz, math.hypot(cx, cy))
    return [cx, cy, cz, yaw, pitch, 0.0, fov, u7, u8]


def recipe_car_external(d):
    """Outside the car, fixed to it (it moves with the car, like a TV car-to-car shot): right side = low front
    quarter on the right, looking back at the car; left side = rear quarter on the left, higher, looking forward
    at it. Bumpers, the third camera and the roof stay."""
    cams, roof, right, half, u7, u8 = _car(d)
    cams[4] = _look_at(3.2, -2.4, 0.55, 1.0, u7, u8)
    cams[5] = _look_at(-4.0, 2.2, 1.3, 0.95, u7, u8)
    return d


def recipe_car_external_2(d):
    """Outside the car, fixed to it: right side = broadside from 5.5 m off the right, left side = head-on from
    6 m ahead, low (the car coming at the lens)."""
    cams, roof, right, half, u7, u8 = _car(d)
    cams[4] = _look_at(0.0, -5.5, 1.0, 0.9, u7, u8)
    cams[5] = _look_at(6.0, 0.0, 0.9, 0.75, u7, u8)
    return d


def recipe_car_mix(d):
    """One of each: right side = TV door cam, left side = low front quarter outside the car (looking back at it)."""
    d = recipe_car_tv_onboard(d)
    cams, roof, right, half, u7, u8 = _car(d)
    cams[5] = _look_at(3.2, 2.4, 0.55, 1.0, u7, u8)
    return d


RECIPES = {"tv_tight": recipe_tv_tight, "tv_wide": recipe_tv_wide, "tv_more_cuts": recipe_tv_more_cuts,
           "tv_more_cuts_tight": recipe_tv_more_cuts_tight,
           "car_broadcast": recipe_car_broadcast, "car_tight": recipe_car_tight, "car_tv_onboard": recipe_car_tv_onboard,
           "car_tv_onboard_nose": recipe_car_tv_onboard_nose, "car_external": recipe_car_external,
           "car_external_2": recipe_car_external_2, "car_mix": recipe_car_mix}
# the presets every track / series gets (name -> recipe; None = the game's own)
TRACK_SET = {"stock": None, "tv tight": "tv_tight", "tv wide": "tv_wide", "tv more cuts": "tv_more_cuts",
             "tv more cuts tight": "tv_more_cuts_tight"}
SERIES_SET = {"stock": None, "broadcast": "car_broadcast", "broadcast tight": "car_tight", "tv onboard": "car_tv_onboard",
              "tv onboard + nose": "car_tv_onboard_nose", "external": "car_external", "external 2": "car_external_2",
              "mix": "car_mix"}


# ---- where the game keeps them
def game_file(kind, who):
    """the loose camera file the game reads (it may not exist yet: some tracks keep theirs inside the .dat)"""
    if kind == "track":
        folder = os.path.join(GAME, "tracks", who)
        names = [n for n in os.listdir(folder) if n.lower() == who.lower() + ".cam"] or \
                [n for n in os.listdir(folder) if n.lower().endswith(".cam")]
        return os.path.join(folder, names[0] if names else who + ".cam")
    return os.path.join(GAME, "series", who, "make_a.cam")


def stock_dir(kind, who):
    return os.path.join(STOCK, kind, who)


def packed(kind, who):
    """the game's own camera file is only inside the .dat (restore = remove our loose copy)"""
    return os.path.exists(os.path.join(stock_dir(kind, who), "packed.txt"))


def stock_copy(kind, who):
    """the game's own file, saved once before anything is changed"""
    src = game_file(kind, who)
    dst = os.path.join(stock_dir(kind, who), os.path.basename(src))
    if not os.path.exists(dst):
        if not os.path.exists(src):
            raise FileNotFoundError(f"{src} - the camera file is packed in the .dat; take it out with `import` first")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
    return dst


def cmd_import(kind, who, path):
    """a camera file taken out of the .dat (DatX) becomes this track's stock; the game has no loose one"""
    if os.path.exists(game_file(kind, who)) and not packed(kind, who):
        raise FileExistsError(f"{game_file(kind, who)} exists - this one isn't packed")
    read_cam(path)
    os.makedirs(stock_dir(kind, who), exist_ok=True)
    shutil.copy2(path, os.path.join(stock_dir(kind, who), os.path.basename(game_file(kind, who))))
    open(os.path.join(stock_dir(kind, who), "packed.txt"), "w").write(
        "The game's own camera file is inside the track .dat; the copy here came out of it (DatX).\n"
        "Restore removes the loose file from the track folder, so the game reads the .dat's again.\n")
    print("imported", path, "as the stock set of", who)


def preset_path(kind, who, name):
    return os.path.join(PRESETS, kind, who, name + ".txt")


def cmd_save(kind, who, name, recipe=None):
    d = read_cam(stock_copy(kind, who))
    if recipe: d = RECIPES[recipe](d)
    p = preset_path(kind, who, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    note = f"{who}, {name}" + (f" ({recipe}: {RECIPES[recipe].__doc__})" if recipe else " (the game's own)")
    open(p, "w").write(to_text(d, " ".join(note.split())))
    print("saved", p)


def cmd_borrow(who, other, note=""):
    """another version of the same track (same lap length) lends its TV1 / TV2 / spectator cameras - they are placed
    along the lap, so they fit; the pit lane cameras (world positions) stay this track's own"""
    d = read_cam(stock_copy("track", who))
    src = game_file("track", other)
    st = os.path.join(stock_dir("track", other), os.path.basename(src))
    e = read_cam(st if os.path.exists(st) else src)
    d["groups"] = e["groups"]
    name = "tv from " + other
    p = preset_path("track", who, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    what = (f"{who}, {name} (TV1 / TV2 / spectator cameras from {other}: "
            + "/".join(str(len(g)) for g in e["groups"]) + " cameras; this track's own pit cameras." + (" " + note if note else "") + ")")
    open(p, "w").write(to_text(d, what))
    print("saved", p)


def cmd_saveall(kind, who):
    for name, recipe in (TRACK_SET if kind == "track" else SERIES_SET).items(): cmd_save(kind, who, name, recipe)


def cmd_apply(kind, who, name):
    stock_copy(kind, who)
    d = from_text(open(preset_path(kind, who, name)).read())
    if d["kind"] != {"track": "track", "series": "car"}[kind]: raise ValueError(f"{name} is a {d['kind']} set")
    write_cam(d, game_file(kind, who))
    open(os.path.join(stock_dir(kind, who), "applied.txt"), "w").write(name + "\n")
    print(f"applied {name} to {game_file(kind, who)} - used from the next session NR2003 loads")


def cmd_restore(kind, who):
    dst = game_file(kind, who)
    if packed(kind, who):
        if os.path.exists(dst): os.remove(dst)
        print("removed", dst, "- the game reads the camera file in the .dat again")
    else:
        shutil.copy2(stock_copy(kind, who), dst)
        print("restored the game's own", dst)
    a = os.path.join(stock_dir(kind, who), "applied.txt")
    if os.path.exists(a): os.remove(a)


def in_use(kind, who):
    a = os.path.join(stock_dir(kind, who), "applied.txt")
    return open(a).read().strip() if os.path.exists(a) else "game's own"


def entries():
    out = []
    for kind in ("series", "track"):
        base = os.path.join(PRESETS, kind)
        for who in sorted(os.listdir(base), key=str.lower) if os.path.isdir(base) else []:
            names = sorted((n[:-4] for n in os.listdir(os.path.join(base, who)) if n.endswith(".txt")),
                           key=lambda n: (n != "stock", n))
            out.append((kind, who, names))
    return out


def cmd_list():
    for kind, who, names in entries():
        print(f"{kind:6} {who:32} in use: {in_use(kind, who):20} presets: {', '.join(names)}")


def describe(kind, who, name):
    """the first line of a preset file says what it is"""
    try: first = open(preset_path(kind, who, name)).readline()
    except OSError: return ""
    i = first.find("(")
    return first[i + 1:].rstrip(")\n") if i >= 0 else ""


def cmd_menu():
    """numbers instead of commands (double-click 'Camera sets.cmd')"""
    while True:
        es = entries()
        print("\nCAMERA SETS  (a change shows from the next session NR2003 loads)\n")
        for n, (kind, who, names) in enumerate(es, 1):
            print(f" {n:2}. {'car cams ' if kind == 'series' else 'track    '} {who:34} now: {in_use(kind, who)}")
        pick = input("\nNumber (Enter = quit): ").strip()
        if not pick: return
        if not pick.isdigit() or not 1 <= int(pick) <= len(es): continue
        kind, who, names = es[int(pick) - 1]
        print(f"\n{who}  (now: {in_use(kind, who)})\n")
        for n, name in enumerate(names, 1):
            what = describe(kind, who, name).split(": ", 1)[-1]
            print(f" {n:2}. {name}" + ("\n" + textwrap.fill(what, 110, initial_indent="     ", subsequent_indent="     ")
                                       if what and name != "stock" else "   (the game's own)"))
        print("  r. back to the game's own")
        pick = input("\nNumber to use (Enter = back): ").strip().lower()
        try:
            if pick == "r": cmd_restore(kind, who)
            elif pick.isdigit() and 1 <= int(pick) <= len(names):
                name = names[int(pick) - 1]
                if name == "stock": cmd_restore(kind, who)
                else: cmd_apply(kind, who, name)
        except Exception as e:
            print("Didn't work:", e)


def main(a):
    if len(a) >= 2 and a[0] == "dump": print(to_text(read_cam(a[1]), os.path.basename(a[1]))); return
    if a and a[0] == "list": cmd_list(); return
    if not a or a[0] == "menu": cmd_menu(); return
    if len(a) >= 4 and a[0] == "save": cmd_save(a[1], a[2], a[3], a[4] if len(a) > 4 else None); return
    if len(a) >= 3 and a[0] == "saveall": cmd_saveall(a[1], a[2]); return
    if len(a) >= 4 and a[0] == "import": cmd_import(a[1], a[2], a[3]); return
    if len(a) >= 3 and a[0] == "borrow": cmd_borrow(a[1], a[2], " ".join(a[3:])); return
    if len(a) >= 4 and a[0] == "apply": cmd_apply(a[1], a[2], a[3]); return
    if len(a) >= 3 and a[0] == "restore": cmd_restore(a[1], a[2]); return
    print(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
