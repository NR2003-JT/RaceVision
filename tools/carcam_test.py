# Car-camera test (2026-10-27): does the car show on its own bumper / side / roof cameras if NR2003 stops treating it
# as "the car the camera is on"? NR2003.exe 0x563c41 (live) and 0x563c18 (replay) set a flag "this car is the focus
# car" before the car's drawing is set up; this test makes that flag always "no" until you press Enter, then puts the
# game's own bytes back. Nothing is written to any file. Run it with NR2003 in a session, on a bumper / side / roof
# camera (keys 10-15 in RaceVision's numbering), and watch.
import ctypes, struct, subprocess, sys

PATCH = [(0x563C41, bytes.fromhex("0f9444241f")), (0x563C18, bytes.fromhex("0f9444241f"))]   # sete byte [esp+1Fh]
NEW = bytes.fromhex("c644241f00")                                                         # mov byte [esp+1Fh], 0

k = ctypes.WinDLL("kernel32", use_last_error=True)
pid = None
for line in subprocess.check_output(["tasklist", "/fo", "csv", "/nh"]).decode(errors="ignore").splitlines():
    p = line.split('","')
    if p and p[0].strip('"').lower() == "nr2003.exe":
        pid = int(p[1])
if not pid:
    sys.exit("NR2003 isn't running.")
h = k.OpenProcess(0x0008 | 0x0010 | 0x0020, False, pid)   # VM_OPERATION | VM_READ | VM_WRITE
if not h:
    sys.exit("Can't open NR2003 (run this as administrator, like RaceVision).")


def read(a, n):
    b = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k.ReadProcessMemory(h, ctypes.c_void_p(a), b, n, ctypes.byref(got)):
        return None
    return b.raw


def write(a, data):
    old = ctypes.c_ulong()
    k.VirtualProtectEx(h, ctypes.c_void_p(a), len(data), 0x40, ctypes.byref(old))
    ok = k.WriteProcessMemory(h, ctypes.c_void_p(a), data, len(data), None)
    k.VirtualProtectEx(h, ctypes.c_void_p(a), len(data), old.value, ctypes.byref(old))
    k.FlushInstructionCache(h, ctypes.c_void_p(a), len(data))
    return ok


for a, orig in PATCH:
    if read(a, 5) != orig:
        sys.exit("NR2003.exe at %#x isn't the code this test knows - nothing changed." % a)
for a, _ in PATCH:
    write(a, NEW)
print("Test ON: the car the camera is on is now drawn like any other car.")
print("Look at the bumper / side / roof cameras (and in-car, which may look odd). Then press Enter to put it back.")
try:
    input()
finally:
    for a, orig in PATCH:
        write(a, orig)
    print("Back to the game's own code." if all(read(a, 5) == o for a, o in PATCH) else "Couldn't restore - restart NR2003.")
