"""Test harness for Monster Hunter: World (Iceborne 15.23, build 421810, Steam): launch the game, drive it into the
world, read the player's position, send input, measure key press -> simulation latency. Memory model, menu flow
and evidence: HARNESS.md next to this file.

usage: uv run --quiet python -I mhwharness.py <command> [args]   (--help on each command)
  launch                     start the game via Steam unless running; wait for its window
  to-game [--until STATE]    from any state (launches if needed) into the world, player controllable
  state [--watch SEC]        detected state: logos, offline-dialog, title, main-menu, save-select, loading,
                             in-game-notice, in-game, in-game-ui, ...
  pos [--watch SEC]          the player's position x y z
  key NAME [ms]              tap a key (scan code); hold NAME ms; mouse DX DY (relative move)
  react NAME [--count N]     key press -> first change of the player's position, ms
  shot PATH [--scale F]      screenshot (PrintWindow, no focus change)
  kill                       Stop-Process the game, after waiting out a save or system message
Input goes to the foreground window: key/hold/mouse/react/to-game bring the game window to the front first.
Exit codes: 0 ok, 1 failure or timeout (to-game saves a screenshot), 2 usage error, 3 game not running.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys
import time
import zlib

GAME_DIR = r"F:\SteamLibrary\steamapps\common\Monster Hunter World"
EXE_NAME = "MonsterHunterWorld.exe"
PLUGINS = GAME_DIR + r"\nativePC\plugins"
DLSS_LOG = PLUGINS + r"\MHW-DLSS4.log"
STEAM_URL = "steam://rungameid/582010"
SHOT_DIR = os.environ.get("MHWHARNESS_SHOTS") or os.path.join(os.environ.get("TEMP", "."), "mhwharness")

EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_NOGAME = 0, 1, 2, 3

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
u32 = ctypes.WinDLL("user32", use_last_error=True)
gdi = ctypes.WinDLL("gdi32", use_last_error=True)

try:  # physical pixels for window rects and captures
    u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except (AttributeError, OSError):
    pass


def _sig(fn, res, *args):
    fn.restype = res
    fn.argtypes = list(args)


HANDLE, HWND, HDC = wt.HANDLE, wt.HWND, wt.HDC
_sig(k32.OpenProcess, HANDLE, wt.DWORD, wt.BOOL, wt.DWORD)
_sig(k32.CloseHandle, wt.BOOL, HANDLE)
_sig(k32.ReadProcessMemory, wt.BOOL, HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
     ctypes.POINTER(ctypes.c_size_t))
_sig(k32.CreateToolhelp32Snapshot, HANDLE, wt.DWORD, wt.DWORD)
_sig(k32.GetExitCodeProcess, wt.BOOL, HANDLE, ctypes.POINTER(wt.DWORD))
_sig(k32.GetProcessTimes, wt.BOOL, HANDLE, *([ctypes.POINTER(wt.FILETIME)] * 4))
_sig(k32.GetCurrentThreadId, wt.DWORD)
_sig(k32.QueryPerformanceCounter, wt.BOOL, ctypes.POINTER(ctypes.c_int64))
_sig(k32.QueryPerformanceFrequency, wt.BOOL, ctypes.POINTER(ctypes.c_int64))
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, HWND, wt.LPARAM)
_sig(u32.EnumWindows, wt.BOOL, WNDENUMPROC, wt.LPARAM)
_sig(u32.GetWindowThreadProcessId, wt.DWORD, HWND, ctypes.POINTER(wt.DWORD))
_sig(u32.IsWindowVisible, wt.BOOL, HWND)
_sig(u32.IsIconic, wt.BOOL, HWND)
_sig(u32.GetWindow, HWND, HWND, wt.UINT)
_sig(u32.GetClientRect, wt.BOOL, HWND, ctypes.POINTER(wt.RECT))
_sig(u32.GetWindowRect, wt.BOOL, HWND, ctypes.POINTER(wt.RECT))
_sig(u32.GetClassNameW, ctypes.c_int, HWND, wt.LPWSTR, ctypes.c_int)
_sig(u32.GetWindowTextW, ctypes.c_int, HWND, wt.LPWSTR, ctypes.c_int)
_sig(u32.GetForegroundWindow, HWND)
_sig(u32.SetForegroundWindow, wt.BOOL, HWND)
_sig(u32.BringWindowToTop, wt.BOOL, HWND)
_sig(u32.ShowWindow, wt.BOOL, HWND, ctypes.c_int)
_sig(u32.AttachThreadInput, wt.BOOL, wt.DWORD, wt.DWORD, wt.BOOL)
_sig(u32.GetDC, HDC, HWND)
_sig(u32.ReleaseDC, ctypes.c_int, HWND, HDC)
_sig(u32.PrintWindow, wt.BOOL, HWND, HDC, wt.UINT)
_sig(u32.MapVirtualKeyW, wt.UINT, wt.UINT, wt.UINT)
_sig(gdi.CreateCompatibleDC, HDC, HDC)
_sig(gdi.DeleteDC, wt.BOOL, HDC)
_sig(gdi.SelectObject, wt.HGDIOBJ, HDC, wt.HGDIOBJ)
_sig(gdi.DeleteObject, wt.BOOL, wt.HGDIOBJ)
_sig(gdi.SetStretchBltMode, ctypes.c_int, HDC, ctypes.c_int)
_sig(gdi.SetBrushOrgEx, wt.BOOL, HDC, ctypes.c_int, ctypes.c_int, ctypes.c_void_p)
_sig(gdi.StretchBlt, wt.BOOL, HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, HDC, ctypes.c_int,
     ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.DWORD)

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
SYNCHRONIZE = 0x00100000


# ---------------------------------------------------------------------------------------------
# Timing


def qpc():
    """QueryPerformanceCounter in seconds (time.perf_counter is QPC on Windows)."""
    return time.perf_counter()


def log(msg):
    print(msg, flush=True)


# ---------------------------------------------------------------------------------------------
# Process and window


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


_sig(k32.Process32FirstW, wt.BOOL, HANDLE, ctypes.POINTER(PROCESSENTRY32W))
_sig(k32.Process32NextW, wt.BOOL, HANDLE, ctypes.POINTER(PROCESSENTRY32W))


def find_pid(name=EXE_NAME):
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    if not snap or snap == HANDLE(-1).value:
        return None
    try:
        e = PROCESSENTRY32W()
        e.dwSize = ctypes.sizeof(e)
        ok = k32.Process32FirstW(snap, ctypes.byref(e))
        while ok:
            if e.szExeFile.lower() == name.lower():
                return e.th32ProcessID
            ok = k32.Process32NextW(snap, ctypes.byref(e))
    finally:
        k32.CloseHandle(snap)
    return None


def window_info(hwnd):
    cls = ctypes.create_unicode_buffer(256)
    title = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(hwnd, cls, 256)
    u32.GetWindowTextW(hwnd, title, 256)
    rc = wt.RECT()
    u32.GetClientRect(hwnd, ctypes.byref(rc))
    return cls.value, title.value, rc.right - rc.left, rc.bottom - rc.top


def find_window(pid):
    """The game's main window: the visible top-level "MT FRAMEWORK" window of the process."""
    found = []

    def cb(h, _):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u32.IsWindowVisible(h) and not u32.GetWindow(h, 4):  # GW_OWNER
            cls, _, w, hh = window_info(h)
            if cls == "MT FRAMEWORK":  # a loader's log console reports the game's pid too
                found.append((w * hh, h))
        return True

    u32.EnumWindows(WNDENUMPROC(cb), 0)
    if not found:
        return None
    found.sort(key=lambda t: t[0], reverse=True)
    return found[0][1]


class Game:
    """The running game: pid, process handle (read access) and main window."""

    def __init__(self, pid):
        self.pid = pid
        self.h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, False, pid)
        if not self.h:
            raise OSError(f"OpenProcess({pid}) failed: {ctypes.get_last_error()}")
        self._buf = ctypes.create_string_buffer(0x10000)
        self._got = ctypes.c_size_t()
        self.hwnd = find_window(pid)

    @classmethod
    def attach(cls):
        pid = find_pid()
        return cls(pid) if pid else None

    def alive(self):
        code = wt.DWORD()
        return bool(k32.GetExitCodeProcess(self.h, ctypes.byref(code))) and code.value == 259  # STILL_ACTIVE

    def start_time(self):
        """Process creation time as a Unix timestamp."""
        t = [wt.FILETIME() for _ in range(4)]
        if not k32.GetProcessTimes(self.h, *[ctypes.byref(x) for x in t]):
            return None
        ft = (t[0].dwHighDateTime << 32) | t[0].dwLowDateTime
        return ft / 1e7 - 11644473600

    def window(self, refresh=False):
        if refresh or not self.hwnd:
            self.hwnd = find_window(self.pid)
        return self.hwnd

    # -- memory --------------------------------------------------------------------------------
    def read(self, addr, n):
        if not addr or addr < 0x10000 or addr >= 0x7FFFFFFF0000:
            return None
        buf = self._buf if n <= len(self._buf) else ctypes.create_string_buffer(n)
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(self._got)):
            return None
        return buf.raw[:n] if self._got.value == n else None

    def q(self, addr):
        b = self.read(addr, 8)
        return struct.unpack("<Q", b)[0] if b else None

    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack("<I", b)[0] if b else None

    def i32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack("<i", b)[0] if b else None

    def u8(self, addr):
        b = self.read(addr, 1)
        return b[0] if b else None

    def f32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack("<f", b)[0] if b else None

    def vec3(self, addr):
        b = self.read(addr, 12)
        return struct.unpack("<3f", b) if b else None

    def chain(self, base, *offsets):
        """[[base]+o1]+o2 ... : dereferences base, then each offset except the last."""
        p = self.q(base)
        for o in offsets[:-1]:
            if not p:
                return None
            p = self.q(p + o)
        if not p:
            return None
        return p + (offsets[-1] if offsets else 0)

    def cstr(self, addr, n=96):
        b = self.read(addr, n) if addr else None
        return b.split(b"\0")[0].decode("ascii", "replace") if b else None

    def dti(self, obj):
        """The MT Framework type (MtDTI) of an object: its vtable +0x20 is getDTI, `lea rax,[dti]; ret`."""
        vt = self.q(obj) if obj else None
        if not vt or not (0x140000000 <= vt < 0x146000000):
            return None
        fn = self.q(vt + 0x20)
        code = self.read(fn, 8) if fn else None
        if not code or code[:3] != b"\x48\x8d\x05" or code[7] != 0xC3:
            return None
        return fn + 7 + struct.unpack_from("<i", code, 3)[0]

    def class_name(self, obj):
        """MT Framework class name of an object (MtDTI name at +8), None when it has none."""
        d = self.dti(obj)
        name = self.cstr(self.q(d + 8)) if d else None
        return name if name and name.isprintable() else None


def running_game(require=True):
    g = Game.attach()
    if g is None and require:
        log("game is not running")
        sys.exit(EXIT_NOGAME)
    return g


# ---------------------------------------------------------------------------------------------
# Input (SendInput with scan codes; DirectInput reads scan codes)


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", wt.DWORD), ("wParamL", wt.WORD), ("wParamH", wt.WORD)]


class _INPUTUNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [("type", wt.DWORD), ("u", _INPUTUNION)]


_sig(u32.SendInput, wt.UINT, wt.UINT, ctypes.POINTER(INPUT), ctypes.c_int)

KEYEVENTF_EXTENDEDKEY, KEYEVENTF_KEYUP, KEYEVENTF_SCANCODE = 0x1, 0x2, 0x8
MOUSEEVENTF_MOVE, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP = 0x1, 0x2, 0x4
MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP = 0x8, 0x10

# Set 1 scan codes; 0xE0xx = extended.
SCAN = {
    "esc": 0x01, "escape": 0x01, "1": 0x02, "2": 0x03, "3": 0x04, "4": 0x05, "5": 0x06, "6": 0x07, "7": 0x08,
    "8": 0x09, "9": 0x0A, "0": 0x0B, "minus": 0x0C, "equals": 0x0D, "backspace": 0x0E, "tab": 0x0F,
    "q": 0x10, "w": 0x11, "e": 0x12, "r": 0x13, "t": 0x14, "y": 0x15, "u": 0x16, "i": 0x17, "o": 0x18,
    "p": 0x19, "lbracket": 0x1A, "rbracket": 0x1B, "enter": 0x1C, "return": 0x1C, "lctrl": 0x1D, "ctrl": 0x1D,
    "a": 0x1E, "s": 0x1F, "d": 0x20, "f": 0x21, "g": 0x22, "h": 0x23, "j": 0x24, "k": 0x25, "l": 0x26,
    "semicolon": 0x27, "apostrophe": 0x28, "grave": 0x29, "lshift": 0x2A, "shift": 0x2A, "backslash": 0x2B,
    "z": 0x2C, "x": 0x2D, "c": 0x2E, "v": 0x2F, "b": 0x30, "n": 0x31, "m": 0x32, "comma": 0x33,
    "period": 0x34, "slash": 0x35, "rshift": 0x36, "nummul": 0x37, "lalt": 0x38, "alt": 0x38, "space": 0x39,
    "capslock": 0x3A, "f1": 0x3B, "f2": 0x3C, "f3": 0x3D, "f4": 0x3E, "f5": 0x3F, "f6": 0x40, "f7": 0x41,
    "f8": 0x42, "f9": 0x43, "f10": 0x44, "num7": 0x47, "num8": 0x48, "num9": 0x49, "numminus": 0x4A,
    "num4": 0x4B, "num5": 0x4C, "num6": 0x4D, "numplus": 0x4E, "num1": 0x4F, "num2": 0x50, "num3": 0x51,
    "num0": 0x52, "numdel": 0x53, "f11": 0x57, "f12": 0x58,
    "up": 0xE048, "down": 0xE050, "left": 0xE04B, "right": 0xE04D, "home": 0xE047, "end": 0xE04F,
    "pgup": 0xE049, "pgdn": 0xE051, "insert": 0xE052, "delete": 0xE053, "rctrl": 0xE01D, "ralt": 0xE038,
    "numenter": 0xE01C,
}
FORBIDDEN_KEYS = {"f9"}  # SharpPluginLoader's ImGui overlay


def scan_of(name):
    key = name.lower()
    if key not in SCAN:
        raise KeyError(f"unknown key {name!r}; known: {', '.join(sorted(SCAN))}")
    return SCAN[key]


def _key_input(scan, up):
    inp = INPUT()
    inp.type = 1  # INPUT_KEYBOARD
    flags = KEYEVENTF_SCANCODE | (KEYEVENTF_KEYUP if up else 0)
    if scan & 0xE000:
        flags |= KEYEVENTF_EXTENDEDKEY
    inp.ki = KEYBDINPUT(0, scan & 0xFF, flags, 0, 0)
    return inp


def send_inputs(inputs):
    arr = (INPUT * len(inputs))(*inputs)
    n = u32.SendInput(len(inputs), arr, ctypes.sizeof(INPUT))
    if n != len(inputs):
        raise OSError(f"SendInput sent {n}/{len(inputs)} (error {ctypes.get_last_error()})")


def key_down(name):
    send_inputs([_key_input(scan_of(name), False)])


def key_up(name):
    send_inputs([_key_input(scan_of(name), True)])


def mouse_move(dx, dy):
    inp = INPUT()
    inp.type = 0  # INPUT_MOUSE
    inp.mi = MOUSEINPUT(int(dx), int(dy), 0, MOUSEEVENTF_MOVE, 0, 0)
    send_inputs([inp])


def foreground(game, settle=0.3, timeout=3.0):
    """Brings the game window to the front (DirectInput only reads input in the foreground)."""
    hwnd = game.window(refresh=True)
    if not hwnd:
        return False
    if u32.GetForegroundWindow() == hwnd:
        return True
    if u32.IsIconic(hwnd):
        u32.ShowWindow(hwnd, 9)  # SW_RESTORE
    me = k32.GetCurrentThreadId()
    attached = []
    fg = u32.GetForegroundWindow()
    for tid in {u32.GetWindowThreadProcessId(fg, None) if fg else 0, u32.GetWindowThreadProcessId(hwnd, None)}:
        if tid and tid != me and u32.AttachThreadInput(me, tid, True):
            attached.append(tid)
    try:
        u32.BringWindowToTop(hwnd)
        u32.SetForegroundWindow(hwnd)
    finally:
        for tid in attached:
            u32.AttachThreadInput(me, tid, False)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if u32.GetForegroundWindow() == hwnd:
            time.sleep(settle)  # let the game re-acquire its DirectInput devices
            return True
        time.sleep(0.02)
    return False


def tap(game, name, ms=80):
    if name.lower() in FORBIDDEN_KEYS:
        raise ValueError(f"{name} is reserved (mod overlay)")
    if not foreground(game):
        raise RuntimeError("could not bring the game window to the foreground")
    key_down(name)
    time.sleep(ms / 1000)
    key_up(name)


# ---------------------------------------------------------------------------------------------
# Screenshots (PrintWindow with PW_RENDERFULLCONTENT; no focus change)


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


_sig(gdi.CreateDIBSection, wt.HBITMAP, HDC, ctypes.POINTER(BITMAPINFOHEADER), wt.UINT,
     ctypes.POINTER(ctypes.c_void_p), HANDLE, wt.DWORD)


class Image:
    def __init__(self, w, h, bgra):
        self.w, self.h, self.bgra = w, h, bgra

    def rgb(self):
        n = self.w * self.h
        out = bytearray(n * 3)
        out[0::3] = self.bgra[2::4]
        out[1::3] = self.bgra[1::4]
        out[2::3] = self.bgra[0::4]
        return bytes(out)

    def save_png(self, path):
        rgb = self.rgb()
        stride = self.w * 3
        raw = b"".join(b"\0" + rgb[y * stride:(y + 1) * stride] for y in range(self.h))

        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0))
                    + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

    def mean(self, x0=0.0, y0=0.0, x1=1.0, y1=1.0):
        """Mean of (R+G+B)/3 over a region given in fractions of the image (0..255)."""
        xa, xb = int(x0 * self.w), max(int(x1 * self.w), int(x0 * self.w) + 1)
        ya, yb = int(y0 * self.h), max(int(y1 * self.h), int(y0 * self.h) + 1)
        total = 0
        for y in range(ya, yb):
            row = self.bgra[(y * self.w + xa) * 4:(y * self.w + xb) * 4]
            total += sum(row) - sum(row[3::4])
        return total / (3 * (xb - xa) * (yb - ya))

    def is_black(self):
        return max(self.bgra[0::4][::97] + self.bgra[1::4][::97] + self.bgra[2::4][::97] or b"\0") < 3


def capture(hwnd, scale=0.5):
    rc = wt.RECT()
    u32.GetClientRect(hwnd, ctypes.byref(rc))
    w, h = rc.right - rc.left, rc.bottom - rc.top
    if w <= 0 or h <= 0:
        return None
    sw, sh = max(1, int(w * scale)), max(1, int(h * scale))
    wdc = u32.GetDC(hwnd)
    dc1, dc2 = gdi.CreateCompatibleDC(wdc), gdi.CreateCompatibleDC(wdc)
    bits1, bits2 = ctypes.c_void_p(), ctypes.c_void_p()

    def dib(width, height, bits):
        bih = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), width, -height, 1, 32, 0, 0, 0, 0, 0, 0)
        return gdi.CreateDIBSection(wdc, ctypes.byref(bih), 0, ctypes.byref(bits), None, 0)

    bm1 = dib(w, h, bits1)
    bm2 = dib(sw, sh, bits2) if (sw, sh) != (w, h) else None
    try:
        old1 = gdi.SelectObject(dc1, bm1)
        ok = u32.PrintWindow(hwnd, dc1, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
        if bm2:
            old2 = gdi.SelectObject(dc2, bm2)
            gdi.SetStretchBltMode(dc2, 4)  # HALFTONE
            gdi.SetBrushOrgEx(dc2, 0, 0, None)
            gdi.StretchBlt(dc2, 0, 0, sw, sh, dc1, 0, 0, w, h, 0x00CC0020)  # SRCCOPY
            data = ctypes.string_at(bits2, sw * sh * 4)
            gdi.SelectObject(dc2, old2)
        else:
            data = ctypes.string_at(bits1, w * h * 4)
        gdi.SelectObject(dc1, old1)
    finally:
        gdi.DeleteObject(bm1)
        if bm2:
            gdi.DeleteObject(bm2)
        gdi.DeleteDC(dc1)
        gdi.DeleteDC(dc2)
        u32.ReleaseDC(hwnd, wdc)
    return Image(sw, sh, data) if ok else None


def screenshot(game, path, scale=0.5, retries=4):
    """Saves a capture; retries when PrintWindow returns an all-black frame."""
    hwnd = game.window(refresh=True)
    if not hwnd:
        return None
    img = None
    for _ in range(retries):
        img = capture(hwnd, scale)
        if img and not img.is_black():
            break
        time.sleep(0.15)
    if img:
        img.save_png(path)
    return img


# ---------------------------------------------------------------------------------------------
# Plugin log


def log_lines_since(start_ts):
    """MHW-DLSS4.log lines when the log belongs to the run that started at start_ts."""
    try:
        if os.path.getmtime(DLSS_LOG) < start_ts:
            return []
        with open(DLSS_LOG, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return []
    return lines


# ---------------------------------------------------------------------------------------------
# Game memory model (MHW:I 15.23, build 421810; absolute VAs, the exe has no ASLR). See HARNESS.md.

S_PLAYER = 0x14500ECA0      # sPlayer*
S_MHUNIT = 0x1451238C8      # sMhUnit*
S_MHRENDER = 0x1451C4480    # sMhRender*
S_MHKEYBOARD = 0x1451C53B0  # sMhKeyboard*
KEY_BITS = 0x138            # sMhKeyboard: key state bitset indexed by virtual-key code, set when the game reads
                            # the keyboard at the start of a frame (+0x158: the same one frame later)
PLAYER_PTR = 0x58           # sPlayer +0x58: slot 0's player (uPlayer*), the local player offline
POS = 0x160                 # uPlayer (uCoord): position x, y, z floats; +0x170 rotation quaternion; +0x180 scale
LIST_PLAYER = 0xA30         # sMhUnit unit list heads: the player list
LIST_GUI = 0x13E0           # ... the GUI list
UNIT_NEXT = 0x30            # unit: next unit in its list
UNIT_FLAGS = 0x14           # unit: bit 0x10 set while the unit is active (a GUI is shown)
TITLE_MENU_INDEX = 0x28F8   # uGUITitle: main menu cursor (0 = Start Game ... 6 = Quit)
SAVE_SLOT_INDEX = 0x2938    # uGUITitleDataSelect: save slot cursor (0 = slot 1)
RENDER_FRAME = 0x1F210      # sMhRender: frame counter


def units(g, list_off, limit=4000):
    """(address, class name, flags) of each unit in one sMhUnit list."""
    root = g.q(S_MHUNIT)
    node = g.q(root + list_off) if root else None
    out = []
    while node and len(out) < limit:
        name = g.class_name(node)
        if not name:
            break
        out.append((node, name, g.u32(node + UNIT_FLAGS) or 0))
        node = g.q(node + UNIT_NEXT)
    return out


def active_gui(g):
    """Class name -> address of the GUI units that are active (shown)."""
    return {name: addr for addr, name, flags in units(g, LIST_GUI) if flags & 0x10}


PLAYER_SLOTS = 0x48         # sPlayer: 24 player slots of 0x740 bytes: +0 key (u64), +8/+0xc ids (i32, < 0 = empty),
PLAYER_SLOT_SIZE = 0x740    #   +0x10 uPlayer*  (so slot 0's player is sPlayer+0x58)
PLAYER_SLOT_COUNT = 24
LOCAL_KEY = 0xAE40          # sPlayer: key of the local player's slot


def player(g):
    """The local player (uPlayer*), as the game's getter 0x141b42010 finds it: the first used slot whose key equals
    sPlayer+0xae40, if its unit flags (+0xc) have a bit of 0xe set. Offline that is slot 0 = [sPlayer+0x58]."""
    sp = g.q(S_PLAYER)
    if not sp:
        return None
    key = g.q(sp + LOCAL_KEY)
    for i in range(PLAYER_SLOT_COUNT):
        b = g.read(sp + PLAYER_SLOTS + i * PLAYER_SLOT_SIZE, 0x18)
        if not b:
            return None
        k, a, c, pl = struct.unpack("<QiiQ", b)
        if a < 0 and c < 0:
            continue
        if k != key:
            continue
        if pl and (g.u8(pl + 0xC) or 0) & 0xE and g.class_name(pl) == "uPlayer":
            return pl
        return None
    return None


def player_pos(g):
    pl = player(g)
    v = g.vec3(pl + POS) if pl else None
    if v is None or any(x != x or abs(x) > 1e7 for x in v):  # NaN or garbage
        return None
    return v


def render_frame(g):
    r = g.q(S_MHRENDER)
    return g.q(r + RENDER_FRAME) if r else None


# ---------------------------------------------------------------------------------------------
# State detection (from the active GUI units, the player pointer and the plugin log)

TITLE_PHASE = 0x28F0        # uGUITitle: 2 "Press Any Button", 4 main menu, 8 save select open (observed values)
LIST_SYSTEM = 0x15D0        # sMhUnit list holding the system message window (and the post filters)
HUD_LIKE = ("uGUIChat", "uGUICommonCursor", "uGUICommonButtonAssign", "uGUICommonSaving")

# state -> what to_game does there
STATES = {
    "not-running": "launch",
    "starting": "wait (no window or nothing on screen yet)",
    "logos": "wait (company logos)",
    "message": "wait: system message such as 'Loading data' / 'Checking for add-on content' / 'Saving data'",
    "offline-dialog": "confirm: 'Starting the game in offline mode' dialog",
    "autosave-notice": "confirm or wait: autosave explanation before the title",
    "title": "press a key: 'Press Any Button'",
    "title-busy": "wait: title GUI in another phase (intro animation, transition)",
    "main-menu": "select Start Game (index 0), confirm",
    "save-select": "select the save slot, confirm",
    "loading": "wait: loading screen",
    "in-game-notice": "confirm: 'Continuing the game in offline mode' notice in the world",
    "in-game-ui": "in the world with another GUI open (menu, dialog); not driven",
    "in-game": "in the world, no blocking GUI; to-game verifies control with a short W press",
    "transition": "wait",
}


class State:
    def __init__(self, name, **info):
        self.name = name
        self.info = info

    def __str__(self):
        extra = " ".join(f"{k}={v}" for k, v in self.info.items() if v not in (None, [], ""))
        return f"{self.name} {extra}".strip()


def system_gui(g):
    return {name for addr, name, flags in units(g, LIST_SYSTEM) if flags & 0x10 and name.startswith("uGUI")}


def detect(g):
    """The game's current state (see STATES), from memory only."""
    if g is None or not g.alive():
        return State("not-running")
    if not g.window(refresh=True) or not g.q(S_MHUNIT):
        return State("starting")
    gui = active_gui(g)
    sysgui = system_gui(g) - {"uGUICommonButtonAssign"}
    pl = player(g)
    pos = player_pos(g) if pl else None
    hud = any(n.startswith("uGUIHud") for n in gui)
    saving = "uGUICommonSaving" in gui
    guis = sorted(n for n in gui if n.startswith("uGUI")) + sorted(sysgui)
    info = {"gui": ",".join(guis), "saving": saving or None}
    if "uGUICommonSaveLoad" in gui:
        return State("loading", **info)
    if hud and pl and pos:
        info["pos"] = "%.1f,%.1f,%.1f" % pos
        blockers = [n for n in guis if n.startswith("uGUI") and not n.startswith("uGUIHud") and n not in HUD_LIKE]
        if not blockers:
            return State("in-game", **info)
        if blockers == ["uGUINetworkErrorWindow"]:
            return State("in-game-notice", **info)
        return State("in-game-ui", blockers=",".join(blockers), **info)
    if "uGUICommonMessageWindow" in sysgui:
        return State("message", **info)
    if "uGUITitleDataSelect" in gui:
        return State("save-select", slot=g.u32(gui["uGUITitleDataSelect"] + SAVE_SLOT_INDEX), **info)
    if "uGUITitle" in gui:
        t = gui["uGUITitle"]
        phase, idx = g.u32(t + TITLE_PHASE), g.u32(t + TITLE_MENU_INDEX)
        others = [n for n in guis if n not in ("uGUITitle",) + HUD_LIKE]
        if others:
            return State("title-busy", phase=phase, others=",".join(others), **info)
        if phase == 2:
            return State("title", **info)
        if phase == 4:
            return State("main-menu", index=idx, **info)
        return State("title-busy", phase=phase, **info)
    if "uGUITitleSaveInfo" in gui:
        return State("autosave-notice", **info)
    if "uGUINetworkErrorWindow" in gui:
        return State("offline-dialog", **info)
    if "uGUILogo" in gui:
        return State("logos", **info)
    return State("transition", **info)


# ---------------------------------------------------------------------------------------------
# Commands


def cmd_launch(args):
    g, t = launch(args.timeout)
    return EXIT_OK if g else EXIT_FAIL


def launch(timeout=120):
    t0 = time.time()
    g = Game.attach()
    if g is None:
        log("launching via Steam")
        os.startfile(STEAM_URL)
        while time.time() - t0 < timeout:
            time.sleep(0.5)
            g = Game.attach()
            if g:
                break
        if g is None:
            log(f"FAIL: no {EXE_NAME} after {timeout} s")
            return None, time.time() - t0
        log(f"process {g.pid} after {time.time() - t0:.1f} s")
    else:
        log(f"game already running (pid {g.pid})")
    while time.time() - t0 < timeout:
        if not g.alive():
            log("FAIL: the game exited")
            return None, time.time() - t0
        hwnd = g.window(refresh=True)
        if hwnd:
            cls, title, w, h = window_info(hwnd)
            if w >= 640:
                log(f"window {hwnd:#x} '{title}' ({cls}) {w}x{h} after {time.time() - t0:.1f} s")
                return g, time.time() - t0
        time.sleep(0.25)
    log(f"FAIL: no game window after {timeout} s")
    return None, time.time() - t0


def cmd_shot(args):
    g = running_game()
    img = screenshot(g, args.path, args.scale)
    if not img:
        log("capture failed")
        return EXIT_FAIL
    log(f"{args.path} {img.w}x{img.h}{' (black)' if img.is_black() else ''}")
    return EXIT_OK


def cmd_key(args):
    g = running_game()
    tap(g, args.name, args.ms)
    log(f"tapped {args.name} for {args.ms} ms")
    return EXIT_OK


def cmd_hold(args):
    return cmd_key(args)


def cmd_mouse(args):
    g = running_game()
    if not foreground(g):
        log("could not bring the game window to the foreground")
        return EXIT_FAIL
    steps = max(1, args.steps)
    for i in range(steps):
        mouse_move(args.dx * (i + 1) // steps - args.dx * i // steps, args.dy * (i + 1) // steps - args.dy * i // steps)
        time.sleep(args.interval / 1000)
    log(f"mouse moved {args.dx},{args.dy} in {steps} steps")
    return EXIT_OK


def describe(g):
    """detect() plus process details for the state command."""
    st = detect(g)
    if st.name == "not-running":
        return st
    started = g.start_time()
    st.info["pid"] = g.pid
    st.info["age"] = "%.1fs" % (time.time() - started) if started else None
    f0, t0 = render_frame(g), qpc()
    time.sleep(0.25)
    f1, t1 = render_frame(g), qpc()
    if f0 is not None and f1 is not None and f1 > f0:
        st.info["fps"] = "%.0f" % ((f1 - f0) / (t1 - t0))
    lines = log_lines_since(started or 0)
    st.info["log"] = ",".join(k for k, pat in (("swapchain", "swap chain"), ("eval512", "evaluate #512 ok"))
                              if any(pat in l for l in lines))
    return st


def cmd_state(args):
    g = Game.attach()
    if args.watch <= 0:
        st = describe(g) if g else State("not-running")
        log(str(st))
        return EXIT_OK if st.name != "not-running" else EXIT_NOGAME
    t_end = time.time() + args.watch
    last = None
    while time.time() < t_end:
        if g is None or not g.alive():
            g = Game.attach()
        st = detect(g)
        key = (st.name, st.info.get("gui"), st.info.get("index"), st.info.get("slot"), st.info.get("phase"))
        if key != last:
            log(f"{time.strftime('%H:%M:%S')} {st}")
            last = key
        time.sleep(0.1)
    return EXIT_OK


def fail_shot(g, what):
    os.makedirs(SHOT_DIR, exist_ok=True)
    path = os.path.join(SHOT_DIR, f"fail-{time.strftime('%Y%m%d-%H%M%S')}.png")
    try:
        if g and g.alive() and screenshot(g, path, 0.5):
            log(f"screenshot: {path}")
    except OSError:
        pass
    log(f"FAIL: {what}")
    return EXIT_FAIL


def probe_control(g, key="w", hold=0.3, min_move=5.0):
    """Holds a movement key; True when the player moved at least min_move units (cm)."""
    pl = player(g)
    if not pl:
        return False, 0.0
    wait_still(g, pl, 0.3, 3.0)
    p0 = player_pos(g)
    if not p0 or not foreground(g):
        return False, 0.0
    key_down(key)
    time.sleep(hold)
    key_up(key)
    time.sleep(0.2)
    p1 = player_pos(g)
    if not p1:
        return False, 0.0
    d = sum((a - b) ** 2 for a, b in zip(p0, p1)) ** 0.5
    return d >= min_move, d


def park_mouse(g):
    """Moves the game's GUI cursor into the bottom-right corner, where no menu item sits (title menu, save
    select, dialogs), so a mouse hover cannot hold the menu selection."""
    if foreground(g):
        for _ in range(4):
            mouse_move(1500, 1000)
            time.sleep(0.02)


def to_game(args):
    """Drives the game from any state into the world with the player controllable (or until args.until is
    reached). Returns an exit code."""
    t0 = time.time()
    g = Game.attach()
    if g is None:
        g, _ = launch(args.timeout)
        if g is None:
            return fail_shot(None, "launch failed")
    started = g.start_time() or t0
    deadline = t0 + args.timeout
    last, entered, stages = None, t0, []
    pressed = {}  # (state, key) -> time of the last press
    nav = {"at": None, "presses": 0, "parked": False, "since": 0.0}
    probes = 0

    def report(final, t_end):
        log("stages (seconds since to-game start / since process start, duration):")
        for (n1, a), nxt in zip(stages, stages[1:] + [(final, t_end)]):
            log(f"  {n1:16} {a - t0:7.1f} {a - started:7.1f}   ({nxt[1] - a:5.1f} s)")
        log(f"  {final:16} {t_end - t0:7.1f} {t_end - started:7.1f}")

    def press(state, key, every):
        if time.time() - pressed.get((state, key), 0) < every:
            return False
        tap(g, key, 90)
        pressed[(state, key)] = time.time()
        log(f"{time.time() - t0:7.1f} s    key {key}")
        return True

    def navigate(state, idx, want):
        """Presses up/down until the cursor index is `want`. True once it has stayed there for 0.4 s."""
        if not nav["parked"]:
            # A visible GUI cursor hovering a menu item selects that item (seen: "Quit" in the main menu, a
            # "New Game" slot in the save select), also after a key press. Park it where nothing is hovered.
            log(f"{time.time() - t0:7.1f} s    parking the mouse cursor in the bottom-right corner")
            park_mouse(g)
            nav["parked"] = True
            return False
        if nav["at"] != (state, idx):
            nav.update(at=(state, idx), presses=0, since=time.time())
        if idx == want:
            return time.time() - nav["since"] >= 0.4
        if nav["presses"] >= 4:  # the index does not follow the keys
            raise RuntimeError(f"{state}: the cursor stays at index {idx} after {nav['presses']} key presses")
        if press(state, "up" if idx > want else "down", 0.35):
            nav["presses"] += 1
        return False

    while time.time() < deadline:
        if not g.alive():
            return fail_shot(None, "the game exited")
        st = detect(g)
        now = time.time()
        if st.name != last:
            log(f"{now - t0:7.1f} s  {st}")
            stages.append((st.name, now))
            last, entered = st.name, now
        if args.until and st.name == args.until:
            report(st.name, now)
            return EXIT_OK
        since = now - entered
        name = st.name
        try:
            if name == "offline-dialog" and since > 0.5:
                press(name, args.dialog_key, 6.0)
            elif name == "autosave-notice" and since > 1.0:
                press(name, args.confirm, 6.0)
            elif name == "title" and since > 0.5:
                press(name, args.dialog_key, 6.0)
            elif name == "main-menu" and since > 0.5:
                idx = st.info.get("index")
                if idx is None or idx > 6:
                    return fail_shot(g, f"main menu index unreadable ({idx})")
                if navigate(name, idx, 0):
                    press(name, args.confirm, 4.0)  # index 0 = Start Game
            elif name == "save-select" and since > 1.5:
                slot = st.info.get("slot")
                if slot is None or slot > 2:
                    return fail_shot(g, f"save slot index unreadable ({slot})")
                if navigate(name, slot, args.slot - 1):
                    press(name, args.confirm, 6.0)
            elif name == "in-game-notice" and since > 0.5:
                press(name, args.dialog_key, 6.0)
            elif name == "in-game" and since > args.settle:
                ok, d = probe_control(g, args.probe_key)
                if ok:
                    t_end = time.time()
                    log(f"{t_end - t0:7.1f} s  controllable: {args.probe_key} moved the player {d:.0f} units")
                    report("controllable", t_end)
                    return EXIT_OK
                probes += 1
                log(f"{time.time() - t0:7.1f} s    probe {probes}: moved {d:.1f} units")
                if probes >= 3:
                    return fail_shot(g, f"in the world but {args.probe_key} does not move the player")
                entered = time.time()
            elif name == "in-game-ui" and since > args.ui_timeout:
                return fail_shot(g, f"in the world, blocked by {st.info.get('blockers')}")
            elif name == "title-busy" and since > 30:
                return fail_shot(g, f"title stuck: {st}")
        except RuntimeError as e:  # foreground failures, stuck menu cursor
            return fail_shot(g, str(e))
        time.sleep(0.1)
    return fail_shot(g, f"timeout after {args.timeout:.0f} s in state {last}")


def cmd_to_game(args):
    return to_game(args)


def cmd_kill(args):
    import subprocess
    g = Game.attach()
    if g is None:
        log("game is not running")
        return EXIT_OK
    t_end = time.time() + args.wait
    while True:
        st = detect(g)
        busy = st.info.get("saving") or "uGUICommonMessageWindow" in (st.info.get("gui") or "")
        if not busy:
            break
        if time.time() > t_end:
            if not args.force:
                log(f"still busy ({st}); not killing it (use --force)")
                return EXIT_FAIL
            break
        log(f"waiting: {st}")
        time.sleep(0.5)
    shell = "pwsh" if any(os.path.exists(os.path.join(p, "pwsh.exe")) for p in os.environ.get("PATH", "").split(";")) \
        else "powershell"
    subprocess.run([shell, "-NoProfile", "-NonInteractive", "-Command",
                    "Stop-Process -Name MonsterHunterWorld -Force"], timeout=60)
    for _ in range(100):
        if not g.alive():
            log("game killed")
            return EXIT_OK
        time.sleep(0.1)
    log("FAIL: the game is still running")
    return EXIT_FAIL


def cmd_pos(args):
    g = running_game()
    t_end = time.time() + args.watch
    while True:
        p = player_pos(g)
        if p is None:
            log("no player")
        else:
            pl = player(g)
            log(f"{p[0]:.2f} {p[1]:.2f} {p[2]:.2f}" + (f"   uPlayer {pl:#x}" if args.verbose else ""))
        if time.time() >= t_end:
            return EXIT_OK if p else EXIT_FAIL
        time.sleep(args.interval)


OPPOSITE = {"w": "s", "s": "w", "a": "d", "d": "a", "up": "down", "down": "up", "left": "right", "right": "left"}
COORD_BYTES = 0x20  # position (12) + pad (4) + rotation quaternion (16)


POS_EPS = 0.001   # game units (cm): smaller position changes are noise
ROT_EPS = 1e-4    # quaternion components flicker by 1 ulp every frame (renormalisation)


def coord(raw):
    """(x, y, z), (qx, qy, qz, qw) from the COORD_BYTES block at uPlayer+POS."""
    v = struct.unpack("<8f", raw)
    return v[0:3], v[4:8]


def moved(a, b, eps):
    return any(abs(x - y) > eps for x, y in zip(a, b))


def wait_still(g, pl, still_s, timeout_s):
    """Waits until the player's position and rotation stay put for still_s. Returns (pos, rot) or None."""
    t_end = qpc() + timeout_s
    raw = g.read(pl + POS, COORD_BYTES)
    if raw is None:
        return None
    last = coord(raw)
    since = qpc()
    while qpc() < t_end:
        time.sleep(0.004)
        raw = g.read(pl + POS, COORD_BYTES)
        if raw is None:
            return None
        cur = coord(raw)
        if moved(cur[0], last[0], POS_EPS) or moved(cur[1], last[1], ROT_EPS):
            last, since = cur, qpc()
        elif qpc() - since >= still_s:
            return cur
    return None


def stats(xs):
    xs = sorted(xs)
    n = len(xs)
    med = xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2
    return f"min {xs[0]:.2f}  median {med:.2f}  mean {sum(xs) / n:.2f}  max {xs[-1]:.2f} ms (n={n})"


def vk_of(name):
    """Virtual-key code of a key name (the game's key bitset is indexed by VK)."""
    sc = scan_of(name)
    return u32.MapVirtualKeyW((0xE000 | (sc & 0xFF)) if sc & 0xE000 else sc, 3)  # MAPVK_VSC_TO_VK_EX


def last_latency_line(g):
    st = g.start_time() or 0
    for line in reversed(log_lines_since(st)):
        if "latency over" in line:
            return line
    return None


def cmd_react(args):
    import random
    g = running_game()
    pl = player(g)
    if not pl or player_pos(g) is None:
        log("FAIL: no player (not in the game world)")
        return EXIT_FAIL
    st = detect(g)
    if st.name != "in-game" and not args.force:
        log(f"FAIL: state is {st} (in-game needed; --force to measure anyway)")
        return EXIT_FAIL
    if args.name.lower() in FORBIDDEN_KEYS:
        log(f"{args.name} is reserved")
        return EXIT_USAGE
    alt = None if args.no_alt else (args.alt or OPPOSITE.get(args.name.lower()))
    keys = [args.name] + ([alt] if alt else [])
    if not foreground(g, settle=0.5):
        log("FAIL: could not bring the game window to the foreground")
        return EXIT_FAIL
    hwnd = g.window()
    rend, kb = g.q(S_MHRENDER), g.q(S_MHKEYBOARD)
    rpm, h, got = k32.ReadProcessMemory, g.h, ctypes.c_size_t()
    cbuf, fbuf, kbuf = (ctypes.create_string_buffer(COORD_BYTES), ctypes.create_string_buffer(8),
                        ctypes.create_string_buffer(1))
    caddr, faddr = ctypes.c_void_p(pl + POS), ctypes.c_void_p(rend + RENDER_FRAME)
    size = ctypes.sizeof(INPUT)
    unpack8f, unpackq = struct.Struct("<8f").unpack, struct.Struct("<Q").unpack
    rows = []
    log(f"react: keys {'/'.join(keys)}, {args.count} tries, hold {args.hold:.0f} ms, still {args.still:.0f} ms")
    run_t0, run_f0 = qpc(), g.q(rend + RENDER_FRAME)
    for i in range(args.count):
        key = keys[i % len(keys)]
        vk = vk_of(key)
        kaddr, kbit = ctypes.c_void_p(kb + KEY_BITS + vk // 8), 1 << (vk % 8)
        if wait_still(g, pl, args.still / 1000, 6.0) is None:
            log(f"try {i}: the player does not stand still; stopping")
            break
        if u32.GetForegroundWindow() != hwnd:
            log(f"try {i}: the game lost the foreground; re-focusing")
            if not foreground(g, settle=0.5):
                log("FAIL: could not bring the game window back")
                return EXIT_FAIL
            continue
        time.sleep(random.uniform(0, args.jitter / 1000))
        bpos, brot = coord(g.read(pl + POS, COORD_BYTES))
        f0 = g.q(rend + RENDER_FRAME)
        down = (INPUT * 1)(_key_input(scan_of(key), False))
        up = (INPUT * 1)(_key_input(scan_of(key), True))
        t_read = t_pos = t_rot = t_frame = None
        f_read = f_pos = None
        t0 = qpc()
        u32.SendInput(1, down, size)
        t_sent = qpc()
        deadline = t0 + args.timeout / 1000
        while True:
            rpm(h, caddr, cbuf, COORD_BYTES, ctypes.byref(got))
            rpm(h, faddr, fbuf, 8, ctypes.byref(got))
            rpm(h, kaddr, kbuf, 1, ctypes.byref(got))
            now = qpc()
            v = unpack8f(cbuf.raw)
            fr = unpackq(fbuf.raw)[0]
            if t_frame is None and fr != f0:
                t_frame = now
            if t_read is None and kbuf.raw[0] & kbit:
                t_read, f_read = now, fr
            if t_rot is None and moved(v[4:8], brot, ROT_EPS):
                t_rot = now
            if moved(v[0:3], bpos, POS_EPS):
                t_pos, f_pos = now, fr
                break
            if now > deadline:
                break
        while qpc() < t0 + args.hold / 1000:
            pass
        u32.SendInput(1, up, size)
        ms = lambda t: None if t is None else (t - t0) * 1000  # noqa: E731
        row = {"try": i, "key": key, "sendinput_ms": ms(t_sent), "next_frame_ms": ms(t_frame),
               "key_read_ms": ms(t_read), "rotation_ms": ms(t_rot), "position_ms": ms(t_pos),
               "frames_to_read": None if f_read is None else f_read - f0,
               "frames_to_position": None if f_pos is None else f_pos - f0}
        rows.append(row)
        if t_pos is None:
            log(f"try {i:2} {key:>5}: no position change within {args.timeout:.0f} ms"
                + (f" (key read +{row['key_read_ms']:.2f} ms)" if t_read else " (the game did not see the key)"))
            continue
        fmt = lambda x: "  -   " if x is None else f"{x:6.2f}"  # noqa: E731
        log(f"try {i:2} {key:>5}: position +{fmt(row['position_ms'])} ms | SendInput {fmt(row['sendinput_ms'])}, "
            f"key read {fmt(row['key_read_ms'])} (frame +{row['frames_to_read']}), "
            f"position at frame +{row['frames_to_position']}")
    run_frames = (g.q(rend + RENDER_FRAME) or 0) - (run_f0 or 0)
    run_ms = (qpc() - run_t0) * 1000
    good = [r for r in rows if r["position_ms"] is not None]
    if not good:
        log("FAIL: no reaction measured")
        return EXIT_FAIL
    log(f"key press -> position change:  {stats([r['position_ms'] for r in good])}")
    reads = [r for r in good if r["key_read_ms"] is not None]
    if reads:
        log(f"key press -> game reads key:   {stats([r['key_read_ms'] for r in reads])}")
        log(f"game reads key -> position:    {stats([r['position_ms'] - r['key_read_ms'] for r in reads])}")
    log(f"SendInput call:                {stats([r['sendinput_ms'] for r in good])}")
    if run_frames > 0:
        log(f"frame interval during the run: {run_ms / run_frames:.2f} ms ({1000 * run_frames / run_ms:.0f} fps)")
    line = last_latency_line(g)
    if line:
        log("plugin: " + line.split("] ", 2)[-1][:200])
    if args.csv:
        cols = list(rows[0].keys())
        with open(args.csv, "w") as f:
            f.write(",".join(cols) + "\n")
            for r in rows:
                f.write(",".join("" if r[c] is None else (f"{r[c]:.3f}" if isinstance(r[c], float) else str(r[c]))
                                 for c in cols) + "\n")
        log(f"wrote {args.csv}")
    return EXIT_OK if len(good) == len(rows) else EXIT_FAIL


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("launch", help="start the game unless running; wait for its window")
    p.add_argument("--timeout", type=float, default=120)
    p.set_defaults(fn=cmd_launch)
    p = sub.add_parser("shot", help="save a screenshot (PrintWindow, no focus change)")
    p.add_argument("path")
    p.add_argument("--scale", type=float, default=0.5)
    p.set_defaults(fn=cmd_shot)
    p = sub.add_parser("key", help="tap a key (scan code) with the game in the foreground")
    p.add_argument("name")
    p.add_argument("ms", type=int, nargs="?", default=80)
    p.set_defaults(fn=cmd_key)
    p = sub.add_parser("hold", help="hold a key for ms milliseconds")
    p.add_argument("name")
    p.add_argument("ms", type=int)
    p.set_defaults(fn=cmd_hold)
    p = sub.add_parser("mouse", help="relative mouse move")
    p.add_argument("dx", type=int)
    p.add_argument("dy", type=int)
    p.add_argument("--steps", type=int, default=1)
    p.add_argument("--interval", type=float, default=10, help="ms between steps")
    p.set_defaults(fn=cmd_mouse)
    p = sub.add_parser("state", help="print the detected state")
    p.add_argument("--watch", type=float, default=0, help="print every state change for this many seconds")
    p.set_defaults(fn=cmd_state)
    p = sub.add_parser("to-game", help="get into the world with the player controllable (launches if needed)")
    p.add_argument("--timeout", type=float, default=240)
    p.add_argument("--slot", type=int, default=1, choices=(1, 2, 3), help="save slot (1-3)")
    p.add_argument("--confirm", default="f", help="menu confirm key (the button guide shows F)")
    p.add_argument("--dialog-key", default="space", help="key for the offline dialogs and 'Press Any Button'")
    p.add_argument("--probe-key", default="w", help="movement key for the control check")
    p.add_argument("--settle", type=float, default=1.0, help="s in the world before the control check")
    p.add_argument("--ui-timeout", type=float, default=15, help="s to wait for an unknown in-world GUI to close")
    p.add_argument("--until", choices=sorted(STATES), help="stop with success when this state is reached")
    p.set_defaults(fn=cmd_to_game)
    p = sub.add_parser("kill", help="Stop-Process the game, after waiting out a save or system message")
    p.add_argument("--wait", type=float, default=30)
    p.add_argument("--force", action="store_true", help="kill even when still saving after --wait")
    p.set_defaults(fn=cmd_kill)
    p = sub.add_parser("pos", help="print the player's position (x y z)")
    p.add_argument("--watch", type=float, default=0, help="keep printing for this many seconds")
    p.add_argument("--interval", type=float, default=0.25)
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(fn=cmd_pos)
    p = sub.add_parser("react", help="key press to first player position change (ms)")
    p.add_argument("name")
    p.add_argument("--count", type=int, default=10)
    p.add_argument("--hold", type=float, default=150, help="ms the key is held")
    p.add_argument("--still", type=float, default=300, help="ms the player must stand still before a try")
    p.add_argument("--jitter", type=float, default=17, help="random extra wait before a try, ms (0..value)")
    p.add_argument("--timeout", type=float, default=1000, help="ms to wait for a reaction")
    p.add_argument("--alt", help="alternate with this key (default: the opposite direction)")
    p.add_argument("--no-alt", action="store_true", help="always press the same key")
    p.add_argument("--csv", help="write the per-try results to this file")
    p.add_argument("--force", action="store_true", help="measure even when the state is not in-game")
    p.set_defaults(fn=cmd_react)
    args = ap.parse_args(argv)
    try:
        return args.fn(args)
    except (KeyError, ValueError) as e:  # unknown or reserved key name
        log(str(e).strip("'\""))
        return EXIT_USAGE
    except RuntimeError as e:  # foreground failures
        log(f"FAIL: {e}")
        return EXIT_FAIL


if __name__ == "__main__":
    sys.exit(main())
