"""Read-only probe for the in-car crash (rend_dxg.dll+0x53782).

rend_dxg keeps 256 dynamic vertex buffers (two halves of 128) at rend_dxg+0xC3678,
16 bytes each: stride, FVF, buffer pointer, vertices used. A draw bigger than 8192
vertices never fits, so every frame it makes one more buffer; when a half is full
the game crashes. This prints how many slots are taken, plus the camera and the
cockpit FOV, so we can see what makes the count climb.

Run as admin while NR2003 is running:  python render_probe.py
Log: %TEMP%\\render_probe_<time>.txt   Nothing is written to the game.
"""
import ctypes, ctypes.wintypes as wt, os, struct, sys, time

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
TH32CS_SNAPPROCESS, TH32CS_SNAPMODULE, TH32CS_SNAPMODULE32 = 0x2, 0x8, 0x10
PROCESS_VM_READ, PROCESS_QUERY_INFORMATION = 0x10, 0x400

class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wt.DWORD),
                ('cntThreads', wt.DWORD), ('th32ParentProcessID', wt.DWORD),
                ('pcPriClassBase', ctypes.c_long), ('dwFlags', wt.DWORD), ('szExeFile', wt.WCHAR * 260)]

class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('th32ModuleID', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('GlblcntUsage', wt.DWORD), ('ProccntUsage', wt.DWORD), ('modBaseAddr', ctypes.c_void_p),
                ('modBaseSize', wt.DWORD), ('hModule', wt.HMODULE), ('szModule', wt.WCHAR * 256),
                ('szExePath', wt.WCHAR * 260)]

k32.CreateToolhelp32Snapshot.restype = wt.HANDLE

TABLE = 0xC3678      # rend_dxg rva of the buffer table (static base 0x10000000)
CUR_HALF = 0xC467C   # 0 or 0x80
OVERSIZED = 0xC3264  # count of buffers the renderer has created
INCAR_FOV = 0x894EE8 # cockpit FOV float (NR2003.exe)
TV_CAM2 = 0xC06B40   # TV2 camera index byte (255 on TV1)


def find_pid():
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32W(); e.dwSize = ctypes.sizeof(e)
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    pid = None
    while ok:
        if e.szExeFile.lower() == 'nr2003.exe':
            pid = e.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return pid


def find_module(pid, name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    e = MODULEENTRY32W(); e.dwSize = ctypes.sizeof(e)
    ok = k32.Module32FirstW(snap, ctypes.byref(e))
    base = None
    while ok:
        if e.szModule.lower() == name:
            base = e.modBaseAddr
        ok = k32.Module32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return base


def main():
    pid = find_pid()
    if not pid:
        sys.exit('NR2003.exe is not running')
    base = find_module(pid, 'rend_dxg.dll')
    if not base:
        sys.exit('rend_dxg.dll not found (run as admin?)')
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        sys.exit('OpenProcess failed %d (run as admin)' % ctypes.get_last_error())

    def read(addr, n):
        buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
            return None
        return buf.raw

    path = os.path.join(os.environ.get('TEMP', '.'), time.strftime('render_probe_%Y%m%d_%H%M%S.txt'))
    log = open(path, 'w', buffering=1)
    def out(s):
        print(s); log.write(s + '\n')
    out('NR2003 pid %d, rend_dxg at %08X, log %s' % (pid, base, path))
    out('time      half0 half1 created  fov    tv2  note')
    last = None
    while True:
        t = read(base + TABLE, 256 * 16)
        if t is None:
            out('NR2003 closed'); break
        used = [struct.unpack_from('<I', t, i * 16 + 8)[0] != 0 for i in range(256)]
        h0, h1 = sum(used[:128]), sum(used[128:])
        created = struct.unpack('<I', read(base + OVERSIZED, 4) or b'\0' * 4)[0]
        fov = struct.unpack('<f', read(INCAR_FOV, 4) or b'\0' * 4)[0]
        tv2 = (read(TV_CAM2, 1) or b'\xff')[0]
        row = (h0, h1, created, round(fov, 2), tv2)
        if row != last:
            note = 'NEAR FULL' if max(h0, h1) >= 120 else ''
            out('%s  %5d %5d %7d  %6.2f %4d  %s' % (time.strftime('%H:%M:%S'), h0, h1, created, fov, tv2, note))
            last = row
        time.sleep(0.1)


if __name__ == '__main__':
    main()
