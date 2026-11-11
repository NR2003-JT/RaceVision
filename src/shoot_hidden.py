# Runs the RVTest.exe copy on a hidden desktop (never on the user's screen), clicks through its pages,
# scrolls them, resizes the window, and saves PrintWindow captures as PNGs in shots\.
import ctypes, ctypes.wintypes as W, os, sys, time
from PIL import Image

u32, k32, g32 = ctypes.windll.user32, ctypes.windll.kernel32, ctypes.windll.gdi32
HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.join(HERE, 'app'); EXE = os.path.join(APP, 'RVTest.exe'); OUT = os.path.join(HERE, 'shots')
os.makedirs(OUT, exist_ok=True)

u32.CreateDesktopW.restype = W.HANDLE
desk = u32.CreateDesktopW('rvtest_hidden', None, None, 0, 0x10000000, None)
assert desk, 'CreateDesktop failed'

class SI(ctypes.Structure):
    _fields_ = [('cb', W.DWORD), ('lpReserved', W.LPWSTR), ('lpDesktop', W.LPWSTR), ('lpTitle', W.LPWSTR),
                ('dwX', W.DWORD), ('dwY', W.DWORD), ('dwXSize', W.DWORD), ('dwYSize', W.DWORD), ('dwXCountChars', W.DWORD),
                ('dwYCountChars', W.DWORD), ('dwFillAttribute', W.DWORD), ('dwFlags', W.DWORD), ('wShowWindow', W.WORD),
                ('cbReserved2', W.WORD), ('lpReserved2', ctypes.c_void_p), ('hStdInput', W.HANDLE), ('hStdOutput', W.HANDLE), ('hStdError', W.HANDLE)]
class PI(ctypes.Structure):
    _fields_ = [('hProcess', W.HANDLE), ('hThread', W.HANDLE), ('dwProcessId', W.DWORD), ('dwThreadId', W.DWORD)]
si = SI(); si.cb = ctypes.sizeof(SI); si.lpDesktop = 'rvtest_hidden'
pi = PI()
assert k32.CreateProcessW(EXE, None, None, None, False, 0, None, APP, ctypes.byref(si), ctypes.byref(pi)), 'CreateProcess failed'
try:
    hwnd = None
    EnumProc = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    for _ in range(60):
        time.sleep(0.25)
        found = []
        def cb(h, l):
            pid = W.DWORD(); u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            if pid.value == pi.dwProcessId and u32.IsWindowVisible(h):
                n = ctypes.create_unicode_buffer(256); u32.GetWindowTextW(h, n, 256)
                r = W.RECT(); u32.GetClientRect(h, ctypes.byref(r))
                if r.right > 900: found.append(h)
            return True
        u32.EnumDesktopWindows(desk, EnumProc(cb), 0)
        if found: hwnd = found[0]; break
    assert hwnd, 'RaceVision window not found'
    time.sleep(1.5)

    def client():
        r = W.RECT(); u32.GetClientRect(hwnd, ctypes.byref(r)); return r.right, r.bottom
    k = client()[0] / 1100.0                                   # window pixels per 96-dpi unit

    def shot(name):
        time.sleep(0.6)
        w, h = client()
        hdc = u32.GetDC(hwnd); mdc = g32.CreateCompatibleDC(hdc); bmp = g32.CreateCompatibleBitmap(hdc, w, h)
        g32.SelectObject(mdc, bmp)
        u32.PrintWindow(hwnd, mdc, 1 | 2)                       # client only, full content
        class BIH(ctypes.Structure):
            _fields_ = [('biSize', W.DWORD), ('biWidth', W.LONG), ('biHeight', W.LONG), ('biPlanes', W.WORD), ('biBitCount', W.WORD),
                        ('biCompression', W.DWORD), ('biSizeImage', W.DWORD), ('biXPelsPerMeter', W.LONG), ('biYPelsPerMeter', W.LONG),
                        ('biClrUsed', W.DWORD), ('biClrImportant', W.DWORD)]
        bi = BIH(); bi.biSize = ctypes.sizeof(BIH); bi.biWidth = w; bi.biHeight = -h; bi.biPlanes = 1; bi.biBitCount = 32
        buf = ctypes.create_string_buffer(w * h * 4)
        g32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
        Image.frombuffer('RGBA', (w, h), buf, 'raw', 'BGRA', 0, 1).convert('RGB').save(os.path.join(OUT, name + '.png'))
        g32.DeleteObject(bmp); g32.DeleteDC(mdc); u32.ReleaseDC(hwnd, hdc)
        print('saved', name, w, h)

    def click(x, y):
        lp = (int(y * k) << 16) | int(x * k)
        u32.SendMessageW(hwnd, 0x0201, 1, lp); u32.SendMessageW(hwnd, 0x0202, 0, lp)
    def wheel(notches):
        for _ in range(abs(notches)):
            u32.SendMessageW(hwnd, 0x020A, ((-120 if notches > 0 else 120) & 0xFFFF) << 16, 0)
    def height(h96):
        r = W.RECT(); u32.GetWindowRect(hwnd, ctypes.byref(r)); w, h = client()
        u32.SetWindowPos(hwnd, None, 0, 0, r.right - r.left, (r.bottom - r.top) - h + int(h96 * k), 0x0002 | 0x0004)

    pages = ['live', 'director', 'battles', 'cameras', 'overlays', 'art', 'presets', 'phone']
    for hgt in (int(sys.argv[1]) if len(sys.argv) > 1 else 780, 600):
        height(hgt)
        for i, p in enumerate(pages):
            click(100, 64 + 16 + i * 44 + 22)
            shot(f'{hgt}_{i}_{p}_top')
            wheel(30)
            shot(f'{hgt}_{i}_{p}_bottom')
            wheel(-3)
            shot(f'{hgt}_{i}_{p}_mid')
            wheel(-30)
finally:
    k32.TerminateProcess(pi.hProcess, 0)
    time.sleep(0.5)
    u32.CloseDesktop(desk)
