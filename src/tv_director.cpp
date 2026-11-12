// RaceVision - NR2003 TV director and broadcast overlays
//
// Watches the race straight from NR2003's memory and cuts the TV view to the best
// thing happening: crashes, passes and close battles for position, pit stops, the
// leader. Every possible shot gets a score (what it is, position, how close, how
// fresh) and the director cuts when something clearly beats the current shot.
// Draws broadcast overlays (driver tag, battle box, on-board gauges) in see-through
// windows on top of the game (needs borderless / windowed NR2003).
//
// No telemetry is used (the telemetry feed allows one app only, and the stage racing
// companion app needs it), so the director can start any time. Memory map: AIMOD's
// (verified live), plus the camera object found 2026-10-17 (NR2003.exe 1.2.0.1):
//   camera object 0xC069D7: +0x4 camera index, +0xC focused car record pointer
//   car record N = 0xC12030 + N * 5560: +0x5B2 = N, +0x1508 rpm, +0x1510 throttle, +0x1578 mph
//   car block N = 0xACCA30 + N * 8724 (slot N == car record N): see Car fields below
//
// Crowd: roars, cheers, oohs and ahhs on race events, played over the game's own sound
// (sounds\crowd\<kind>_*.wav, any number of each: roar, cheer, pop, ooh, ahh).
//
// The RaceVision window (racevision_ui.cpp) shows what the director is doing and holds every
// setting. Ctrl+F8 plays the next crowd sound (volume check), Ctrl+F9 overlays on/off,
// Ctrl+F10 overlay edit mode (drag to move, wheel to resize), Ctrl+F11 pause / resume
// directing, Ctrl+F12 quit. Settings: racevision.ini, log: racevision.log.
//
// Phone remote (racevision_remote.cpp): a web page on the home network that runs the same
// controls from a phone's browser; the window's Phone page shows the QR code to open it.
//
// Build (x86): build.cmd

#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <math.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <random>
#include <array>
#include <set>
#include <functional>
#include <xaudio2.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <uxtheme.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------------------------------------------------- log
// Lines go to racevision.log and to the window's log list. The director works on the main
// thread and the window on its own, so race data and settings are shared under g_lock.
static CRITICAL_SECTION g_lock, g_logLock;
static std::vector<std::string> g_logNew;   // lines the window has not shown yet
static std::string g_logPart;              // a line still being written (Log calls without a newline)
static FILE *g_logFile = nullptr;
static HWND g_ui = NULL;
static const UINT WM_APP_LOG = WM_APP + 1;

static std::vector<std::string> g_replayLog;     // the REPLAY lines (under g_logLock)
static void Log(const char *fmt, ...) {
	char buf[1024];
	va_list a;
	va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
	fputs(buf, stdout);
	bool any = false;
	EnterCriticalSection(&g_logLock);
	g_logPart += buf;
	size_t nl;
	while ((nl = g_logPart.find('\n')) != std::string::npos) {
		std::string line = g_logPart.substr(0, nl);
		g_logPart.erase(0, nl + 1);
		if (g_logFile) { fputs(line.c_str(), g_logFile); fputc('\n', g_logFile); fflush(g_logFile); }
		if (line.size() > 11 && line[0] == '[' && (line.compare(11, 6, "REPLAY") == 0 || line.compare(11, 9, "HIGHLIGHT") == 0)) {   // the Replays page's log
			g_replayLog.push_back(line);
			if (g_replayLog.size() > 60) g_replayLog.erase(g_replayLog.begin());
		}
		g_logNew.push_back(line);
		any = true;
	}
	LeaveCriticalSection(&g_logLock);
	if (any && g_ui) PostMessage(g_ui, WM_APP_LOG, 0, 0);
}

// ---------------------------------------------------------------- memory map
static const DWORD CAMOBJ = 0xC069D7;
static const DWORD CAM_ADDR = CAMOBJ + 0x4;
// the game's own "set camera + focus car" (what the C and V keys call): thiscall on the
// camera object, (camera, 0, 0, car record), ret 0x10. Going through it keeps the camera
// sounds right (a plain memory write left the helicopter sound playing on every camera).
static const DWORD CAM_SETTER = 0x568E20;
static const DWORD FOCUS_ADDR = CAMOBJ + 0xC;
// The replay has its own camera object (0x503470 returns it): Play moves the live camera and car onto it and clears
// the live one; Stop moves them back - but only into a live camera that is still clear. RaceVision used to point the
// LIVE object at the replay's car during a replay: the replay kept showing the car that was on TV before it, and
// after it the live TV cameras kept chasing the replay's frozen car (live 2026-10-27: TV1/TV2/spectator slow and
// darkening down to 11 fps for a while after a replay, the replay banner naming another driver than on screen).
static const DWORD REPLAY_CAMOBJ = 0xB8E5BD;
static const DWORD REC0 = 0xC12030, REC_SIZE = 5560;
static const DWORD REC_IDX = 0x5B2, REC_RPM = 0x1508, REC_THROTTLE = 0x1510, REC_MPH = 0x1578;

static const DWORD FLAG_STATE = 0x76F184;       // int, 7 = green
static const DWORD SESSION_STATE = 0x977A00;    // byte, 7 = race
static const DWORD SESSION_TIME = 0x977A04;     // double, game seconds (can freeze - use GameSec)
// The game's own clock: int ticks at 0xBEBDF5 / tick rate (36 if [0x755A18] != 0, else [0x7559B4]); the line crossings
// are stamped from it (checked live 2026-10-21, within one tick). Follows time acceleration, stops when paused.
static const DWORD GAME_TICKS = 0xBEBDF5, TICK_RATE_FLAG = 0x755A18, TICK_RATE_VALUE = 0x7559B4;
static const DWORD LAPS_TOTAL = 0x400000 + 0x9ACEE8;
static const DWORD ON_PIT_ROAD = 0x76B230 + 0x8DC;   // byte per slot
static const DWORD BLOCK0 = 0xACCA30, BLOCK_SIZE = 8724;
// NR2003's scoring table: header +4 laps per row, +8 row shift (row = 1 << shift dwords),
// +0xC table; entry [slot][n] = game ms (low 28 bits) when the car completed lap n
static const DWORD SCORING_HDR = 0x77F952, CROSS_MASK = 0x0FFFFFFF;
static const int NUM_SLOTS = 44, PACE_SLOT = 1;
static const int OFF_LAPS_DONE = 1360, OFF_LAP_DIST = 8668, OFF_VEL = 6634, OFF_GRIP = 7418, OFF_REASON_OUT = 4916;
// body damage: floats at +4903 + 16 rear, + 20 front, + 48 right, + 52 left (AIMOD's memory map; read live 2026-10-26:
// rubs ~0.15 a zone at Talladega, wrecked cars 0.4 - 2.0)
static const int OFF_DAMAGE = 4903;
// gear (byte): 0 reverse, 1 neutral, 2 = 1st, ... (found 2026-10-18: steps with the rpm drops, one speed / rpm
// ratio per value; forward gear count at +6362)
static const int OFF_GEAR = 6390;
// single-car qualifying (run in practice by the Single-Car Qualifying tool / Race Control):
// car block +5031 = pointer to the car's current AI state object, +6922 = pit-stall wait
// countdown (the tool holds a car in its box by setting it huge), +6918 = run-end lap, +6778 = lap
static const int OFF_AI_STATE = 5031, OFF_STALL_COUNT = 6922, OFF_RUN_END = 6918, OFF_LAP_NUM = 6778;
static const unsigned AI_WAIT_S = 0x760f60, AI_WAIT_F = 0x760f08, AI_LEAVE_S = 0x760efc, AI_LEAVE_F = 0x760f6c,
	AI_PRACTICE = 0x760d2c, AI_INLAP_S = 0x760f48, AI_INLAP_F = 0x760f78, AI_ENTER_S = 0x760f84, AI_ENTER_F = 0x760f90;
static const int HELD_COUNT = 1000000;           // stall countdown at or above this = held by the tool
static const DWORD DRIVER0 = 14711636, DRIVER_SIZE = 175;   // first[16] @0, last[20] @16, number int @156
static const int FLAG_GREEN = 7, SESSION_RACE = 7, SESSION_PRACTICE = 4;
static const DWORD TRACK_NAME = 0x977938;      // char[32], the loaded track's folder name
// Replay tape (found 2026-10-26 in NR2003.exe): the replay keys (0x503C00) work while the game state is 1, 4 or 5 and
// call the tape functions directly. Pause/Play from live = FN_REPLAY_PLAY(seconds back, paused, 1): it steps seconds x
// tick rate frames back from the end of the tape and rolls from there (live 2026-10-26: lands on time; the race runs
// on meanwhile). FN_REPLAY_STOP(1) = back to the live picture (it puts the camera back too). ("To end" is not: it
// stops, then rolls the tape again from a second before its end, so the tape mode stays 1.)
// While the tape plays the picture shows the replay's own car records, REPLAY_REC0 + slot * REC_SIZE (0x5AA360),
// so the camera has to be pointed at those, not at the live ones.
static const DWORD RP_MODE = 0xB8E59A;          // int, 0 live, 1 playing the tape back
static const DWORD RP_FRAMES = 0xB8E586;        // int, frames on the tape
static const DWORD RP_ROLLING = 0xB8E5AA;       // int, 1 rolling, 0 paused (while RP_MODE = 1)
static const DWORD GAME_STATE = 0xC067D0;       // int, replay keys work in 1, 4, 5
static const DWORD DRIVER_STATE = 0xC067D4;     // int, 3 = you are in your car (GetInCar sets it)
static const DWORD FN_REPLAY_PLAY = 0x5024A0, FN_REPLAY_STOP = 0x502B50;
static const DWORD REPLAY_REC0 = 0xE0C238;
static const DWORD FN_PAUSE = 0x5464E0, FN_RESUME = 0x5465E0, SIM_STATE = 0xBEBD30;   // byte: 2 running, 3 paused
static const int SIG_PAUSE[] = { 0xE8, -1, -1, -1, -1, 0x6A, 0x03, 0xA3, 0xA8, 0xBC, 0xBE, 0x00, 0xE8 };
static const unsigned char SIG_REPLAY_PLAY[] = { 0x53, 0xE8, 0x8A, 0x2A, 0x00, 0x00, 0xA1, 0x9A };
static const unsigned char SIG_REPLAY_STOP[] = { 0x53, 0xE8, 0xDA, 0x23, 0x00, 0x00, 0xA1, 0x9A };

static std::string g_gameDir;        // NR2003 folder (from the running exe)
// RaceVision's own cameras on top of the game's 21 (live 2026-10-27: the game turns In-car 2 / 3 back into In-car 1
// for any car, so a third in-car view can't live there): 22 = In-car right (In-car 1, the eye moved right of the
// driver), 23 = Drone (the helicopter, low and close). Set() puts the game's camera on and remembers which one.
// 24-28 are ready-made angles around the car on the chase cameras (they show the car); 29-40 are made in the live
// camera editor (racevision_cams.cpp).
struct VCam { int id, base; std::string name; bool preset; };
static std::vector<VCam> g_vcams = { { 22, 1, "In-car right", true }, { 23, 16, "Drone", true }, { 24, 4, "Side tracking", true },
	{ 25, 4, "Low rear", true }, { 26, 8, "Front quarter", true }, { 27, 5, "Overhead", true }, { 28, 5, "Pack view", true },
	{ 41, 17, "Static A", true }, { 42, 18, "Static B", true } };
static const int CAM_INCAR_RIGHT = 22, CAM_DRONE = 23, CAM_MAX = 40;
static int g_virtCam = 0;
static bool g_droneQuiet = true;           // drone_quiet: no helicopter sound on the Drone (23)
static const VCam *VCamById(int id) { for (auto &v : g_vcams) if (v.id == id) return &v; return nullptr; }
static int GameCam(int c) { const VCam *v = c > 21 ? VCamById(c) : nullptr; return v ? v->base : c; }
static int CamLast() { int m = 21; for (auto &v : g_vcams) m = std::max(m, v.id); return m; }

struct Game {
	HANDLE h = NULL;
	DWORD pid = 0;
	bool camOk = false;

	static DWORD FindPid() {
		if (getenv("RV_NO_GAME")) return 0;           // a test copy (screenshots) never touches a running game
		DWORD pid = 0;
		HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		PROCESSENTRY32 e; e.dwSize = sizeof(e);
		for (BOOL ok = Process32First(s, &e); ok; ok = Process32Next(s, &e))
			if (_stricmp(e.szExeFile, "NR2003.exe") == 0) pid = e.th32ProcessID;
		CloseHandle(s);
		return pid;
	}
	bool Attach() {
		DWORD p = FindPid();
		if (!p) { Detach(); return false; }
		if (h && p == pid) return true;
		Detach();
		h = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION |
		                PROCESS_CREATE_THREAD, FALSE, p);
		if (!h) {
			static bool told = false;
			if (!told) Log("Cannot open NR2003 (error %lu). NR2003 runs as administrator, so the director\n"
			                  "must too: start it with \"RaceVision.cmd\".\n", GetLastError());
			told = true;
			return false;
		}
		pid = p;
		char exe[MAX_PATH]; DWORD n = MAX_PATH;
		if (QueryFullProcessImageNameA(h, 0, exe, &n)) {
			char *sl = strrchr(exe, '\\');
			if (sl) *sl = 0;
			g_gameDir = exe;
		}
		return true;
	}
	void Detach() { if (h) CloseHandle(h); h = NULL; pid = 0; camOk = false; code = 0; mbox = 0; hooked = false; heliNear = -1; }
	DWORD code = 0;                  // our small call stub inside NR2003 (old remote-thread way)
	bool useCall = true;

	// ---- camera calls on the GAME's main thread ----
	// Calling the setter from a thread of our own ran it at the same time as the game's main
	// thread; a camera change touches the sound system, and its object pool got corrupted (crash
	// at race start, 2026-10-19: allocator 0x4F0710 returned a vtable). Instead a small hook at
	// the start of the per-frame view function (0x599A80, main thread, entered only through a
	// vtable) checks a mailbox and makes the call itself, between frames' work.
	// The same mailbox also runs one game function (cdecl, 3 dword arguments) on the main thread: the replay
	// controls (see Replay). [0] = 1 camera call, 2 function call.
	// v4: [40] = the camera object the camera call sets (live or the replay's).
	// v3: the message is taken ([0] cleared) before the call and a busy flag keeps a nested entry of the view function
	// (the replay start sets the game's camera from inside the call) from running it a second time inside the first
	// (live 2026-10-26: Replay crash sometimes dropped the game to ~1 fps with a dark picture). [32] counts calls done.
	static const DWORD HOOK_AT = 0x599A80;
	static const DWORD HOOK_MAGIC = 0x34485652;   // "RVH4" at [mbox+56]: this cave
	DWORD mbox = 0;                  // [0] pending, [4] camera, [8] car record, [12] frame count,
	                                 // [16] function, [20] [24] [28] its arguments, [32] calls done, [36] busy,
	                                 // [40] the camera object
	bool hooked = false;
	bool InstallHook() {
		if (hooked) return true;
		unsigned char orig[5], want[5] = { 0x83, 0xEC, 0x3C, 0x53, 0x55 };   // sub esp,3Ch; push ebx; push ebp
		if (!Read(HOOK_AT, orig, 5)) return false;
		if (orig[0] == 0xE9) {           // already hooked (RaceVision restarted): reuse that cave's mailbox
			DWORD rel; memcpy(&rel, orig + 1, 4);
			DWORD old = HOOK_AT + 5 + rel;
			if (Get<DWORD>(old - 64 + 56, 0) == HOOK_MAGIC) { mbox = old - 64; hooked = true; return true; }
			// a cave from an older RaceVision: a new one below, and the jump moved over to it
		} else if (memcmp(orig, want, 5) != 0) return false;   // not the code we know: don't touch it
		mbox = (DWORD)(UINT_PTR)VirtualAllocEx(h, NULL, 512, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!mbox) return false;
		DWORD cave = mbox + 64;
		unsigned char c[200]; int n = 0;
		auto b = [&](unsigned char x) { c[n++] = x; };
		auto d = [&](DWORD x) { memcpy(c + n, &x, 4); n += 4; };
		b(0xFF); b(0x05); d(mbox + 12);                     // inc dword [mbox+12]   (frame counter)
		b(0x83); b(0x3D); d(mbox); b(0);                    // cmp dword [mbox], 0
		b(0x74); int jz = n; b(0);                          // je skip
		b(0x83); b(0x3D); d(mbox + 36); b(0);               // cmp dword [mbox+36], 0   (inside a call already)
		b(0x75); int jbusy = n; b(0);                       // jne skip
		b(0x60); b(0x9C);                                   // pushad; pushfd
		b(0xC7); b(0x05); d(mbox + 36); d(1);               // mov dword [mbox+36], 1   (busy)
		b(0xA1); d(mbox);                                   // mov eax, [mbox]
		b(0xC7); b(0x05); d(mbox); d(0);                    // mov dword [mbox], 0      (taken)
		b(0x83); b(0xF8); b(2);                             // cmp eax, 2
		b(0x74); int jcall = n; b(0);                       // je call
		b(0x8B); b(0x0D); d(mbox + 40);                     // mov ecx, [mbox+40]   camera object
		b(0xFF); b(0x35); d(mbox + 8);                      // push [mbox+8]  car record
		b(0x6A); b(0); b(0x6A); b(0);                       // push 0; push 0
		b(0xFF); b(0x35); d(mbox + 4);                      // push [mbox+4]  camera
		b(0xB8); d(CAM_SETTER); b(0xFF); b(0xD0);           // call setter (thiscall, ret 10h)
		b(0xEB); int jdone = n; b(0);                       // jmp done
		c[jcall] = (unsigned char)(n - jcall - 1);          // call:
		b(0xFF); b(0x35); d(mbox + 28);                     // push [mbox+28]
		b(0xFF); b(0x35); d(mbox + 24);                     // push [mbox+24]
		b(0xFF); b(0x35); d(mbox + 20);                     // push [mbox+20]
		b(0xA1); d(mbox + 16);                              // mov eax, [mbox+16]
		b(0xFF); b(0xD0);                                   // call eax (cdecl)
		b(0x83); b(0xC4); b(12);                            // add esp, 12
		c[jdone] = (unsigned char)(n - jdone - 1);          // done:
		b(0xFF); b(0x05); d(mbox + 32);                     // inc dword [mbox+32]      (calls done)
		b(0xC7); b(0x05); d(mbox + 36); d(0);               // mov dword [mbox+36], 0
		b(0x9D); b(0x61);                                   // popfd; popad
		c[jz] = (unsigned char)(n - jz - 1);                // skip:
		c[jbusy] = (unsigned char)(n - jbusy - 1);
		if (n - jz - 1 > 127) return false;                 // (short jumps)
		for (int i = 0; i < 5; i++) b(want[i]);             // the 5 bytes we replaced
		b(0xE9); d(HOOK_AT + 5 - (cave + n + 4));           // jmp back to 0x599A85
		unsigned char zero[64] = {};
		DWORD magic = HOOK_MAGIC, obj = CAMOBJ; memcpy(zero + 56, &magic, 4); memcpy(zero + 40, &obj, 4);
		SIZE_T w = 0;
		if (!WriteProcessMemory(h, (LPVOID)(UINT_PTR)mbox, zero, 64, &w)) return false;
		if (!WriteProcessMemory(h, (LPVOID)(UINT_PTR)cave, c, n, &w)) return false;
		unsigned char jmp[5] = { 0xE9 }; DWORD rel = cave - (HOOK_AT + 5); memcpy(jmp + 1, &rel, 4);
		DWORD prot = 0;
		if (!VirtualProtectEx(h, (LPVOID)(UINT_PTR)HOOK_AT, 5, PAGE_EXECUTE_READWRITE, &prot)) return false;
		bool ok = WriteProcessMemory(h, (LPVOID)(UINT_PTR)HOOK_AT, jmp, 5, &w) && w == 5;
		VirtualProtectEx(h, (LPVOID)(UINT_PTR)HOOK_AT, 5, prot, &prot);
		FlushInstructionCache(h, (LPCVOID)(UINT_PTR)HOOK_AT, 5);
		hooked = ok;
		return ok;
	}
	void RemoveHook() {              // put the original 5 bytes back (RaceVision closing, game still running)
		if (!h || !hooked) return;
		unsigned char want[5] = { 0x83, 0xEC, 0x3C, 0x53, 0x55 };
		DWORD prot = 0; SIZE_T w = 0;
		if (VirtualProtectEx(h, (LPVOID)(UINT_PTR)HOOK_AT, 5, PAGE_EXECUTE_READWRITE, &prot)) {
			WriteProcessMemory(h, (LPVOID)(UINT_PTR)HOOK_AT, want, 5, &w);
			VirtualProtectEx(h, (LPVOID)(UINT_PTR)HOOK_AT, 5, prot, &prot);
			FlushInstructionCache(h, (LPCVOID)(UINT_PTR)HOOK_AT, 5);
		}
		hooked = false;
	}
	// one message to the cave: kind 1 = camera (args at +4), 2 = function (args at +16). True once the game ran it.
	// Frames running is checked over 250 ms (a slow frame rate is still running); a message the game hasn't taken
	// within takeMs is cancelled; one it has taken is waited for up to runMs (a long tape seek).
	bool Mail(DWORD kind, const DWORD *args, int nArgs, DWORD at, int takeMs, int runMs) {
		if (!InstallHook()) return false;
		DWORD f0 = Get<DWORD>(mbox + 12, 0);
		bool frames = false;
		for (int i = 0; i < 25 && !frames; i++) { Sleep(10); frames = Get<DWORD>(mbox + 12, 0) != f0; }
		if (!frames) return false;                              // menus, minimised
		for (int i = 0; i < 100 && (Get<DWORD>(mbox, 1) != 0 || Get<DWORD>(mbox + 36, 1) != 0); i++) Sleep(10);
		if (Get<DWORD>(mbox, 1) != 0 || Get<DWORD>(mbox + 36, 1) != 0) return false;   // another call still going
		DWORD done0 = Get<DWORD>(mbox + 32, 0);
		SIZE_T w = 0;
		if (!WriteProcessMemory(h, (LPVOID)(UINT_PTR)(mbox + at), args, nArgs * 4, &w)) return false;
		if (!WriteProcessMemory(h, (LPVOID)(UINT_PTR)mbox, &kind, 4, &w)) return false;
		for (int ms = 0; ms < takeMs + runMs; ms += 10) {
			Sleep(10);
			if (Get<DWORD>(mbox + 32, done0) != done0) return true;
			if (ms >= takeMs && Get<DWORD>(mbox, 0) == kind) {  // not taken: cancel
				DWORD zero = 0; WriteProcessMemory(h, (LPVOID)(UINT_PTR)mbox, &zero, 4, &w);
				Sleep(50);                                      // (unless it was taken just now)
				return Get<DWORD>(mbox + 32, done0) != done0;
			}
		}
		return false;
	}
	// returns false when frames aren't running (menus, minimised) - the caller writes memory instead
	bool MainThreadSet(int cam, DWORD rec, DWORD obj = CAMOBJ) {
		if (!InstallHook()) return false;
		for (int i = 0; i < 100 && (Get<DWORD>(mbox, 1) != 0 || Get<DWORD>(mbox + 36, 1) != 0); i++) Sleep(10);
		SIZE_T w = 0;
		if (!WriteProcessMemory(h, (LPVOID)(UINT_PTR)(mbox + 40), &obj, 4, &w)) return false;
		DWORD a[2] = { (DWORD)cam, rec };
		return Mail(1, a, 2, 4, 500, 500);
	}
	// fn(a0, a1, a2) on the game's main thread, between frames; false when frames aren't running or it didn't run
	bool MainThreadCall(DWORD fn, DWORD a0, DWORD a1, DWORD a2) {
		DWORD a[4] = { fn, a0, a1, a2 };
		return Mail(2, a, 4, 16, 1000, 5000);
	}

	// OLD: run camobj->Set(camera, 0, 0, carRecord) on a short thread of our own inside the game.
	// Not used any more (see above); kept for reference.
	bool CallSet(int cam, DWORD rec) {
		if (!code) code = (DWORD)(UINT_PTR)VirtualAllocEx(h, NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!code) return false;
		unsigned char st[40]; int n = 0;
		auto b = [&](unsigned char x) { st[n++] = x; };
		auto d = [&](DWORD x) { memcpy(st + n, &x, 4); n += 4; };
		b(0xB9); d(CAMOBJ);                 // mov ecx, camobj
		b(0x68); d(rec);                    // push car record
		b(0x6A); b(0); b(0x6A); b(0);       // push 0, push 0
		b(0x68); d((DWORD)cam);             // push camera
		b(0xB8); d(CAM_SETTER);             // mov eax, setter
		b(0xFF); b(0xD0);                   // call eax
		b(0x31); b(0xC0); b(0xC2); b(4); b(0);   // xor eax, eax; ret 4
		SIZE_T w = 0;
		if (!WriteProcessMemory(h, (LPVOID)(UINT_PTR)code, st, n, &w)) return false;
		HANDLE th = CreateRemoteThread(h, NULL, 0, (LPTHREAD_START_ROUTINE)(UINT_PTR)code, NULL, 0, NULL);
		if (!th) return false;
		DWORD r = WaitForSingleObject(th, 1000);
		CloseHandle(th);
		return r == WAIT_OBJECT_0;
	}
	bool Read(DWORD a, void *v, SIZE_T n) { SIZE_T got = 0; return h && ReadProcessMemory(h, (LPCVOID)(UINT_PTR)a, v, n, &got) && got == n; }
	template <class T> T Get(DWORD a, T def) { T v; return Read(a, &v, sizeof v) ? v : def; }
	bool Write(DWORD a, int v) { SIZE_T n = 0; return h && WriteProcessMemory(h, (LPVOID)(UINT_PTR)a, &v, 4, &n) && n == 4; }

	int Focus() {
		DWORD d = Get<DWORD>(FOCUS_ADDR, 0);
		if (d < REC0 || (d - REC0) % REC_SIZE != 0) return -1;
		int idx = (int)((d - REC0) / REC_SIZE);
		return idx < 64 ? idx : -1;
	}
	bool RecordIs(int idx) { return Get<int>(REC0 + idx * REC_SIZE + REC_IDX, -1) == idx; }
	bool CheckCamera() { int f = Focus(); camOk = f >= 0 && RecordIs(f); return camOk; }
	// camera and / or focus car; -1 keeps the current one. replay: the car as the replay tape shows it
	// The helicopter's sound: NR2003 starts it when the camera changes to the helicopter kind (0x593697 `cmp eax, 7`,
	// then the blimp_cam loop, sound 10). For the Drone (23) that compare looks for a kind that doesn't exist (0x7F)
	// - set before the camera changes, so the sound never starts; any other camera puts the 7 back.
	// The game only looks again when the camera KIND changes (it remembers the last one at 0xD8C30C): from the
	// Helicopter (16) to the Drone - the same kind - the loop played on (tester, new24), and a second compare at
	// 0x593577 sets the loop's volume every frame. Both 7s are switched, and the remembered kind is forgotten (-1)
	// so the game looks again next frame: the "not the helicopter" branch stops the loop (volume 0).
	int heliKind = -1;
	void HeliSound(bool on) {
		static const DWORD AT[2] = { 0x593699, 0x593579 };    // the 7 in 83 F8 07: kind change / every frame
		static const DWORD SEEN_KIND = 0xD8C30C;
		unsigned char want = on ? 7 : 0x7F;
		if (heliKind == want) return;
		heliKind = want;
		for (DWORD a : AT) {
			unsigned char cur[3];
			if (!Read(a - 2, cur, 3) || cur[0] != 0x83 || cur[1] != 0xF8 || (cur[2] != 7 && cur[2] != 0x7F)) return;
		}
		bool changed = false;
		for (DWORD a : AT) {
			unsigned char cur = 0;
			Read(a, &cur, 1);
			DWORD prot = 0; SIZE_T w = 0;
			if (cur != want && VirtualProtectEx(h, (LPVOID)(UINT_PTR)a, 1, PAGE_EXECUTE_READWRITE, &prot)) {
				changed |= WriteProcessMemory(h, (LPVOID)(UINT_PTR)a, &want, 1, &w) != 0;
				VirtualProtectEx(h, (LPVOID)(UINT_PTR)a, 1, prot, &prot);
				FlushInstructionCache(h, (LPCVOID)(UINT_PTR)a, 1);
			}
		}
		if (changed) Write(SEEN_KIND, -1);
	}
	// The helicopter camera's near clip plane: 0x56B064 mov [view+0xA4], 10.0 (the stock helicopter flies at 100 m). Our
	// helicopter cameras fly low (the Drone sits ~13 m over the car): anything nearer than 10 m wasn't drawn, so on the
	// banking the track under it vanished into the sky colour (user screenshot 2026-11-01, Talladega). 2 m on ours.
	int heliNear = -1;
	void HeliNear(bool ours) {
		static const DWORD AT = 0x56B064;
		unsigned char want[4] = { 0, 0, (unsigned char)(ours ? 0x00 : 0x20), (unsigned char)(ours ? 0x40 : 0x41) };   // 2.0 / 10.0
		if (heliNear == (int)ours) return;
		unsigned char cur[10];
		static const unsigned char op[6] = { 0xC7, 0x85, 0xA4, 0x00, 0x00, 0x00 };
		if (!Read(AT, cur, 10) || memcmp(cur, op, 6) || cur[6] || cur[7] || !((cur[8] == 0x20 && cur[9] == 0x41) || (cur[8] == 0 && cur[9] == 0x40))) { heliNear = (int)ours; return; }
		heliNear = (int)ours;
		if (!memcmp(cur + 6, want, 4)) return;
		DWORD prot = 0; SIZE_T w = 0;
		if (VirtualProtectEx(h, (LPVOID)(UINT_PTR)(AT + 6), 4, PAGE_EXECUTE_READWRITE, &prot)) {
			WriteProcessMemory(h, (LPVOID)(UINT_PTR)(AT + 8), want + 2, 2, &w);   // only the two bytes that differ
			VirtualProtectEx(h, (LPVOID)(UINT_PTR)(AT + 6), 4, prot, &prot);
			FlushInstructionCache(h, (LPCVOID)(UINT_PTR)(AT + 6), 4);
		}
	}
	bool Set(int cam, int idx, bool replay = false) {
		if (!camOk) return false;
		int virt = cam > 21 && VCamById(cam) ? cam : 0;
		if (cam >= 0) { g_virtCam = virt; cam = GameCam(cam); HeliSound(!(virt == CAM_DRONE && g_droneQuiet)); HeliNear(virt && cam == 16); }
		if (idx >= 0 && (idx >= NUM_SLOTS || !RecordIs(idx))) return false;
		if (cam >= 64) return false;
		int curCam = Camera();
		DWORD obj = replay ? REPLAY_CAMOBJ : CAMOBJ;            // during a replay: the replay's own camera
		if (replay) curCam = Get<int>(obj + 4, -1);
		DWORD rec = idx >= 0 ? (replay ? REPLAY_REC0 : REC0) + idx * REC_SIZE : Get<DWORD>(obj + 0xC, 0);
		if (cam < 0) cam = curCam;
		if (useCall && rec && MainThreadSet(cam, rec, obj)) return true;
		if (useCall && !hooked) { useCall = false; Log("Can't hook the game's camera code - writing memory instead.\n"); }
		bool ok = true;
		if (idx >= 0) ok &= Write(obj + 0xC, (int)rec);
		if (cam != curCam) ok &= Write(obj + 4, cam);
		return ok;
	}
	bool SetFocus(int idx) { return idx >= 0 && Set(-1, idx); }
	int Camera() { return Get<int>(CAM_ADDR, -1); }
	bool SetCamera(int c) { return c >= 0 && Set(c, -1); }
	float RecF(int idx, DWORD off) { return Get<float>(REC0 + idx * REC_SIZE + off, 0.f); }
};

// ---------------------------------------------------------------- settings
static char g_ini[MAX_PATH];

struct Settings {
	double minHold = 10.0;           // a shot stays at least this long (unless a crash happens)
	double maxHold = 40.0;           // after this the current shot loses half its score
	double incidentHold = 8.0;       // a crash stays a candidate this long
	double switchMargin = 1.5;       // a new shot must score this many times the current one
	double manualHold = 20.0;        // after you pick a car yourself (V), the director waits this long
	double qualHandoffM = 300.0;     // qualifying: switch focus this many metres before the next runner's stripe
	int qualPlayerCam = 1;           // qualifying: your own run (Race Control's `drive`) goes on this camera (your cockpit); 0 = off
	double battleGap = 0.8;          // seconds between two cars to call it a battle
	double battleMinAge = 4.0;       // a battle must last this long before it is worth a cut
	double shortTrackM = 1700;       // tracks shorter than this (metres) are short tracks ...
	double battleShortM = 8;         // ... where a battle means at the bumper or alongside (metres)
	double onboardHold = 25;         // in-car / roof shots stay at least this long
	int leadBattleDepth = 3;         // a battle for this position or better is a lead battle (0 = off) ...
	double leadBattleHold = 120;     // ... it doesn't fade and the director stays with it this long (s) - not in a pack
	int leadBattlePack = 6;          // a pack: this many cars within 1.5 s of the leader (superspeedway draft) = as before
	std::string artFolders = "";      // extra art folders (the usual places are found by themselves: AutoArtRoots)
	std::string companionFolder;      // the companion app's data folder (companion_folder): its series art and the active series in its config.json; blank = none
	std::string artSeries = "auto";  // a series folder name, or auto
	std::string overlayStyle = "classic";   // classic, or dash (TV in-car dash on on-board cameras)
	unsigned dashAccent = 0x3CC8C8;  // dash border / place colour
	unsigned dashAccent2 = 0x145A60; // the driver plate fades from the accent into this (was fixed dark teal)
	bool dashArtColors = false;      // dash look in each car's own colour (the art's colors.json), else the accent
	double dashSpeedMax = 220;       // mph at the end of the speed arc
	int settleLaps = 3;              // after the start / a restart the field is bunched up: for this many laps ...
	int settlePositions = 6;         // ... only battles up to this position count ...
	double settleHold = 15.0;        // ... and shots stay at least this long
	double revisitAfter = 45.0;      // a car we just left scores half for this long
	int tourDepth = 8;               // the camera tour visits the top this many when nothing else is on
	int overlayMinRunLaps = 5;       // overlays only this many laps into a green run ...
	double overlayGap = 150;         // ... and the tag / battle box at most once this often (s); the in-car dash
	                                 // shows on every on-board shot
	bool tagStory = true;            // classic driver tag: a line about this car's race so far
	double tagRotate = 6;            // ... changing to the next fact this often (seconds)
	std::vector<int> stageLaps;      // laps the stages end on (stage finish positions)
	bool crowd = false;              // crowd reactions on race events (needs the user's own sound files)
	double crowdVolume = 0.8;
	std::string crowdFolder = "sounds\\crowd";
	std::vector<int> crowdFavourites;   // car numbers the crowd is loudest for
	int crowdPassDepth = 10;         // passes for this position and better get a reaction
	double crowdCooldown = 6;        // seconds between two pass reactions
	std::string crowdEvents = "start, restart, lead, pass, crash, white, win";
	double crashDrop = 31.0;         // m/s lost within crash_window = crash / spin (31 m/s = 70 mph)
	double crashWindow = 1.5;        // game seconds
	// crash replays: each crash is logged with where it started (first contact / speed loss of the cars in it)
	bool replayAuto = false;         // roll the replay by itself (else only the Replay button / phone)
	bool replayCautionOnly = true;   // ... only under caution
	double replayPreroll = 4;        // the replay starts this long before the first sign (game s)
	double replayAfter = 8;          // the crash must be over this long (no car in it still sliding) first (real s)
	double replayMax = 20;           // longest replay (s)
	double replayWithin = 120;       // a crash older than this (game s) isn't replayed
	double crashDamage = 0.5;        // one contact adding this much body damage = a crash (rubs add 0.001 - 0.03)
	double replayWorthy = 1.5;       // a crash worth a replay: its damage + 0.5 a car past the first (see Worth)
	int replayAngles = 3;            // a multi-car crash: replayed once per car hit hardest, each from another camera
	double replayTake = 8;           // seconds of each angle after the first
	double replayCountdown = 5;      // the alert counts down this long before an automatic replay rolls
	int reelAngles = 1;              // angles per crash in the highlight reel
	// highlights besides crashes (racevision_highlights.cpp): passes for the front, multi-passes, bump and runs, the finish
	bool highlights = true;          // log them (Replays page, phone, the highlight reel); nothing goes on air by itself but the finish
	int hlMultiCars = 2;             // a multi-pass: one car by this many cars in one corner or one straight (superspeedways: last lap only)
	double hlGap = 180;              // passes, multi-passes and bump and runs: at most one in this many game s (the better one stays; the last lap is free)
	bool hlBump = true;              // a pass right after the car behind hit the back of the one it passed (not a crash)
	double hlBumpWindow = 3;         // ...within this many game s of the hit
	int hlBumpDepth = 10;            // ...for this position or better
	std::string superspeedways = "daytona; talladega";   // track folder names (a part of one; ; between): multi-passes there only on the last lap
	double replaySlowImpact = 1.0;   // crash replays: a slow-motion look (one, the hardest hit) only when its hardest single hit is this big
	double hlPreroll = 6;            // highlight replays start this long before the moment (game s)
	bool replayFinish = true;        // replay the finish by itself once the winner and the runner-up are across the line
	double replayFinishAfter = 6;    // ...this long after (real s)
	bool reelHighlights = true;      // the highlight reel plays these too (with the crashes, in race order)
	bool replayDrive = true;         // a replay asked for while you drive (Ctrl+F7, buttons): pause the race around it
	double contactJoin = 0.05;       // a contact this big near a crash going on = that car is in it
	double crashSpan = 20;           // a crash takes hits from its first this many game s only (its damage = theirs)
	bool customCams = true;          // own angles on Chase 2, Far chase 2 and In-car 3 (CustomCams)
	std::string chase2 = "4.5, -2.2, 1.0, 26, 4";      // back, side (+ left), height, turn (+ left), tilt (deg)
	std::string farChase2 = "15, 0, 7.5, 0, 20";
	std::string incar3 = "-0.25, -0.5, 0.05";         // eye offset: ahead, left, up (m) - right of the driver (camera 22)
	double droneHeight = 30;         // Drone (camera 23): the helicopter at this height (m)
	double droneDistance = 100;      // our helicopter cameras: how far behind they trail, % of the game's (smaller = closer)
	double droneTopMph = 175;        // ... their top speed (the game's: 78.4 m/s = 175 mph; slower cars than that pull away)
	double droneHoverM = 25;         // ... within this of the car they hold still (the game's 25 m)
	bool rearPan = true;             // Rear (11) turns toward the car behind / alongside
	double rearPanSpan = 110;        // ...up to this many degrees either way from straight back
	double rearPanFov = 105;         // ...with this wide a lens (degrees; the game's is 78)
	bool rearMount = true;           // Rear (11) rides on the car just ahead of the TV car, looking back at it
	double rearMountM = 45;          // ...when one is within this many metres ahead (else rear_fallback)
	double rearHoldM = 70;           // ...and stays on it while the TV car is no further back than this
	double rearPassM = 5;            // ...or alongside, until the TV car's nose is this far past it (then the next car)
	double rearBlockS = 1.5;         // ...or another car has been between them this long
	bool replayBanner = true;        // replays: the REPLAY banner (car, angle, time to live), even with overlays off
	bool replaySmart = true;         // crash replays: slow motion into the hit and out of it (the game's own 1/2 ... 1/17)
	int replaySlowMax = 5;           // ...the slowest it goes for a big hit: 1 = 1/2 speed ... 16 = 1/17
	bool replayTransition = true;    // a full-screen TV wipe going into a replay, between angles and back to live
	double replayTransitionMs = 1500; // ...this long (ms)
	bool hideCompanions = false;     // replays and wipes: hide the companion apps' overlay windows (a ticker, a track map ...), show them again at live
	std::string companionApps;       // ...only these apps' windows (part of the exe's path, ; between several); blank = every other app's overlays
	std::string companionKeep;       // ...but never these (same way)
	unsigned replayWipeColor = 0xFFFFFFFF;   // ...in this colour (replay_wipe_color; unset = the dash accent, as it was before it had its own)
	bool replayOverlays = true;      // replays: the driver tag on the replay's car, the dash on in-car angles
	double rearPanSmooth = 0.45;     // the rear camera's turn follows the TV car this smoothly (s; higher = softer)
	int rearFallback = 11;           // nobody close ahead: 11 = its own rear cam looking at the car behind, else this camera
	bool liveAfterClimbOut = true;   // getting out of your car starts the game's replay (20 s back): skip it back to live
	double followAfter = 5.0;        // switch from the opening camera to a follow camera after this
	double cameraEvery = 12.0;       // then change follow camera this often (0 = never)
	double tvCutGap = 1.5;           // no cut this close (s) to a TV1 / TV2 / spectator camera change on screen (0 = off)
	double tvCutWait = 4.0;          // ...waiting for it at most this long
	// dynamic cameras (racevision_dyncams.cpp): RaceVision places its own static cameras on the pit lane camera blocks
	bool dynCams = false;            // master switch (off: the director uses only the game's cameras)
	bool dynPit = true, dynCrash = true, dynRestart = true, dynBattle = true, dynFinish = true, dynLearn = true;
	double dynChance = 60;           // % of the shots that could have a dynamic camera that get one
	double dynHalfWidth = 0;         // the track's half width (m); 0 = learn it from the cars
	double dynYawFix = 0, dynPitchSign = 1;   // aim calibration: degrees added to the turn; -1 flips the tilt
	bool dynRig = true, dynAutoZoom = true;   // the follow rig (own thread, smooth) aims the dynamic cameras; the lens follows the car's distance
	double dynSmooth = 1.0;                   // the rig's smoothing times x (lower = quicker, tighter pans)
	bool overlays = true;
	bool cameraCall = true;          // change cameras through the game's own function (camera_call = 0: memory write)
	std::vector<int> onboard = { 1, 2, 3, 12, 13 };
	std::map<std::string, std::vector<int>> cams;
	std::map<std::string, std::string> raw;   // every key, for the overlay positions
};
static Settings S;

static std::string Trim(const std::string &s) {
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static std::vector<int> IntList(std::string v) {
	std::vector<int> list;
	for (char *t = strtok(&v[0], ", "); t; t = strtok(NULL, ", ")) list.push_back(atoi(t));
	return list;
}

static const char *DEFAULT_INI =
	"; RaceVision settings. Lines starting with ; are comments.\n"
	";\n"
	"; Camera numbers (mapped 2026-10-17):\n"
	";   1-3 cockpit, 4 chase, 5 far chase, 6 chase, 7 far chase, 8 rear chase,\n"
	";   9 (dark / unused), 10 front, 11 rear, 12 roof forward, 13 roof,\n"
	";   14 right side, 15 left side, 16 helicopter, 17 pit lane 1, 18 pit lane 2,\n"
	";   19 TV1, 20 TV2, 21 spectator\n"
	";\n"
	"; Each shot opens on a camera from cam_<shot> and, after follow_after seconds,\n"
	"; moves to cam_<shot>_follow (changing every camera_every seconds).\n"
	"; An empty list keeps whatever camera is showing.\n"
	"cam_incident = 16, 19\n"
	"cam_incident_follow = 20, 4\n"
	"cam_pass = 19, 20\n"
	"cam_pass_follow = 8, 13\n"
	"cam_battle = 19, 20\n"
	"cam_battle_follow = 8, 1, 13, 4\n"
	"; pit stops follow the car through pit road: entry, in the stall, leaving\n"
	"cam_pit = 19, 16\n"
	"cam_pit_stall = 17, 18\n"
	"cam_pit_exit = 19, 20\n"
	"cam_pit_follow =\n"
	"cam_leader = 20, 16\n"
	"cam_leader_follow = 13, 5, 1, 4\n"
	"cam_tour = 20, 19\n"
	"cam_tour_follow = 13, 1, 4\n"
	"cam_restart = 16\n"
	"cam_restart_follow = 19\n"
	"; single-car qualifying (run in practice): leaving the pit box, then the run itself\n"
	"cam_qual_exit = 17, 18\n"
	"cam_qual = 19, 20\n"
	"cam_qual_follow = 4, 13, 1\n"
	"; your own qualifying run: from Race Control's GO until you are stopped back in your box the camera stays\n"
	";   in your car on this camera (1 = cockpit); 0 = leave the camera to the director\n"
	"qual_player_cam = 1\n"
	"; camera_call = 1 changes cameras through the game's own function (keeps camera sounds right), 0 = memory write\n"
	"camera_call = 1\n"
	"; The phone's Cameras tab starts as the game's own camera list in stock order, with three of RaceVision's own cameras added: 23 Drone,\n"
	";   27 Overhead, 28 Pack view. cam_hidden lists the ones kept off that list (9 is the game's unused one; 22, 24, 25 and 26 are other\n"
	";   RaceVision angles): untick Hidden on the Cameras tab to bring one back. Hidden cameras still work in the lists below.\n"
	"cam_hidden = 9, 22, 24, 25, 26\n"
	"follow_after = 5\n"
	"camera_every = 12\n"
	"; TV1 / TV2 / spectator change camera by themselves as the car drives on (the track's camera file says where). No\n"
	";   cut this close (s) to such a change on screen - two cuts in a row look like a mistake; wait for it at most\n"
	";   tv_cut_wait s. tv_cut_gap = 0: off\n"
	"tv_cut_gap = 1.5\n"
	"tv_cut_wait = 4\n"
	"; cameras that count as on-board (the on-board gauges show on these)\n"
	"onboard_cams = 1, 2, 3, 12, 13\n"
	"\n"
	"; Timing (real seconds)\n"
	"min_hold = 10\n"
	"max_hold = 40\n"
	"incident_hold = 8\n"
	"switch_margin = 1.5\n"
	"revisit_after = 45\n"
	"manual_hold = 20\n"
	"\n"
	"; Detection\n"
	"battle_gap = 0.8\n"
	"battle_min_age = 4\n"
	"; short tracks (under short_track_m metres): a battle is a car at the bumper or alongside\n"
	"short_track_m = 1700\n"
	"battle_short_m = 8\n"
	"; in-car / roof shots stay at least this long\n"
	"onboard_hold = 25\n"
	"; lead battles: a battle for lead_battle_depth or better doesn't fade, and the director stays with it up to\n"
	";   lead_battle_hold s (only a crash, a restart or something far bigger cuts away). Not in a pack (lead_battle_pack\n"
	";   cars within 1.5 s of the leader = superspeedway draft): there it is directed as before. depth 0 = off\n"
	"lead_battle_depth = 3\n"
	"lead_battle_hold = 120\n"
	"lead_battle_pack = 6\n"
	"; start / restarts: for settle_laps laps only battles up to settle_positions, shots hold settle_hold s\n"
	"settle_laps = 3\n"
	"settle_positions = 6\n"
	"settle_hold = 15\n"
	"tour_depth = 8\n"
	"crash_drop = 31\n"
	"crash_window = 1.5\n"
	"\n"
	"; Crash replays (test). Every crash is logged with the moment it started (first contact or speed loss of the\n"
	";   cars in it). replay_auto = 1 rolls NR2003's own replay of it once the crash is over (replay_after s), from\n"
	";   replay_preroll s before that moment, on cam_replay, then goes back to live. Needs you out of your car.\n"
	"cam_replay = 19, 20\n"
	"replay_auto = 0\n"
	"replay_caution_only = 1\n"
	"replay_preroll = 4\n"
	"replay_after = 8\n"
	"; replay_max: the longest each angle runs on after its hit (s); it always starts replay_preroll before it\n"
	"replay_max = 20\n"
	"replay_within = 120\n"
	"; a contact adding crash_damage of body damage (0 - 1 a side, 4 sides) is a crash; contact_join pulls the cars hit\n"
	";   near it into it. The replay starts replay_preroll before the hardest hit and follows the car that took it.\n"
	"crash_damage = 0.5\n"
	"; replay_worthy: a crash scores its damage + 0.5 for every car past the first; the replay goes to the best one.\n"
	";   A multi-car crash plays replay_angles times (the cars hit hardest, each replay_take s, cameras from\n"
	";   cam_replay then cam_replay_angles). The alert counts replay_countdown s down first.\n"
	"replay_worthy = 1.5\n"
	"replay_angles = 3\n"
	"replay_take = 8\n"
	"replay_countdown = 5\n"
	"reel_angles = 1\n"
	"; in your car, Ctrl+F7 (or a replay button) pauses the race, plays the replay, and resumes it after\n"
	"replay_drive = 1\n"
	"cam_replay_angles = 16, 5, 21\n"
	"; highlights besides crashes, few and far between: a multi-pass (one car by multi_pass_cars cars in one corner or\n"
	";   one straight; on the superspeedways only on the last lap), a last-lap pass for the podium, a bump and run (the\n"
	";   nose of the car behind on the tail of the one ahead, then by it within bump_run_window s, for P1 to\n"
	";   bump_run_depth) and the finish. At most one pass-type highlight every highlight_gap game s (the better one\n"
	";   stays; the last lap is free). No slow motion on passes. They go on the Replays list,\n"
	";   the phone and the highlight reel (reel_highlights); only the finish replays by itself (replay_finish,\n"
	";   replay_finish_after s after the runner-up crosses). Their replays start highlight_preroll s before.\n"
	"highlights = 1\n"
	"multi_pass_cars = 2\n"
	"highlight_gap = 180\n"
	"highlight_bump = 1\n"
	"bump_run_window = 3\n"
	"bump_run_depth = 10\n"
	"superspeedways = daytona; talladega\n"
	"; crash replays get one slow-motion look (the hardest hit, from the first cam_replay camera, last) only when that\n"
	";   hit is replay_slow_impact or more (a single damage jump; 0.5 = a crash, 1+ = a big one)\n"
	"replay_slow_impact = 1.0\n"
	"highlight_preroll = 6\n"
	"replay_finish = 1\n"
	"replay_finish_after = 6\n"
	"reel_highlights = 1\n"
	"contact_join = 0.05\n"
	"; a crash counts the damage of the hits in its first crash_span game seconds only\n"
	"crash_span = 20\n"
	"\n"
	"; Own angles on the near-duplicate cameras. Chase 2 / Far chase 2: back, side (+ = left), height, turn (+ = left),\n"
	";   tilt (degrees, + = down). In-car right: the eye moved ahead, left, up (metres) from the driver's, then turn,\n"
	";   tilt, roll (degrees) and lens (degrees; empty = your own cockpit field of view).\n"
	"custom_cams = 1\n"
	"chase2_view = 4.5, -2.2, 1.0, 26, 4\n"
	"far_chase2_view = 15, 0, 7.5, 0, 20\n"
	"incar3_eye = -0.25, -0.5, 0.05\n"
	"; camera 22 = In-car right (In-car 1 with incar3_eye), 23 = Drone (the helicopter at drone_height m)\n"
	"drone_height = 30\n"
	"; 1 = no helicopter sound on the Drone (the Helicopter, 16, keeps it)\n"
	"drone_quiet = 1\n"
	"; RaceVision's helicopter cameras (the Drone): how far behind the car they trail (% of the game's helicopter; 50 =\n"
	";   about half as far), top speed (mph; the game's 175 - faster cars leave it behind until it jumps back), and the\n"
	";   distance (m) within which they hold still. The Helicopter (16) keeps the game's own.\n"
	"drone_distance = 100\n"
	"drone_top_mph = 175\n"
	"drone_hover_m = 25\n"
	"; Rear (11) turns toward the nearest car behind or alongside, up to rear_pan_span degrees, lens rear_pan_fov\n"
	"rear_pan = 1\n"
	"rear_pan_span = 110\n"
	"rear_pan_fov = 105\n"
	"; Rear (11) shows the TV car from the car just ahead of it (within rear_mount_m metres), turned to keep it in\n"
	";   view; the overlays stay on the TV car. Nobody that close ahead: rear_alone = 11 keeps the TV car's own rear\n"
	";   camera looking at the car behind (rear_pan), another number cuts to that camera. rear_mount = 0: off\n"
	"rear_mount = 1\n"
	"rear_mount_m = 45\n"
	"; it stays on that car while the TV car is up to rear_hold_m behind it or alongside; it lets go when the TV car is\n"
	";   rear_pass_m past its nose, or another car has been between them rear_block_s seconds. rear_pan_smooth = how\n"
	";   softly the camera turns after the TV car (seconds)\n"
	"rear_hold_m = 70\n"
	"rear_pass_m = 5\n"
	"rear_block_s = 1.5\n"
	"rear_pan_smooth = 0.45\n"
	"rear_alone = 11\n"
	"; getting out of your car makes NR2003 play its replay from 20 s back; 1 = go straight back to live instead\n"
	"live_after_climb_out = 1\n"
	"\n"
	"; Overlays (positions / sizes are saved here when you move them in edit mode, Ctrl+F10)\n"
	"overlays = 1\n"
	"; number cards, headshots and driver colours: series folders (series\\<id>\\numbers, headshots, colors.json).\n"
	"; Found by themselves: an art folder next to RaceVision.exe, AIMOD's art (while AIMOD runs) and the companion\n"
	"; app's art. art_folders adds more, separated by ;  art_series = auto uses the companion app's active series,\n"
	"; else the best match for the cars on track\n"
	"art_folders =\n"
	"art_series = auto\n"
	"; overlay_style: classic, or dash = a TV-style in-car dash (place, driver, speed arc, RPM) on on-board cameras\n"
	"overlay_style = classic\n"
	"dash_accent = #3CC8C8\n"
	"; the driver plate fades from dash_accent into dash_accent2 (the same colour twice = a solid plate)\n"
	"dash_accent2 = #145A60\n"
	"; dash_art_colors = 1: each car's dash in its own colour from the art (colors.json); cars without one keep the accent\n"
	"dash_art_colors = 0\n"
	"dash_speed_max = 220\n"
	"; overlays stay off under caution and early in a green run; on-board battles always get them\n"
	"overlay_min_run_laps = 5\n"
	"; the driver tag / battle box show at most once every overlay_gap seconds; the in-car dash shows on every\n"
	";   on-board shot (in-car / roof cameras)\n"
	"overlay_gap = 150\n"
	"; classic driver tag: tag_story = 1 adds a line about the race so far (start / now, since the restart,\n"
	";   stage finishes, last pit stop, laps led), changing every tag_rotate seconds\n"
	"tag_story = 1\n"
	"tag_rotate = 6\n"
	"; laps the stages end on, e.g. stage_laps = 60, 120 (empty = no stage positions)\n"
	"stage_laps =\n"
	"\n"
	"; Crowd reactions, played over the game sound. Sounds: crowd_folder\\<kind>_*.wav, kinds roar, cheer, pop, ooh, ahh\n"
	";   (your own recordings; RaceVision comes without any). Ctrl+F8 plays a test sound.\n"
	"crowd = 0\n"
	"crowd_volume = 0.8\n"
	"crowd_folder = sounds\\crowd\n"
	"; car numbers the crowd cheers loudest for (and groans for when they crash or lose a spot)\n"
	"crowd_favourites =\n"
	"; passes for this position or better get a cheer, at most one every crowd_cooldown seconds\n"
	"crowd_pass_depth = 10\n"
	"crowd_cooldown = 6\n"
	"; which events: start, restart, lead, pass, crash, white, win\n"
	"crowd_events = start, restart, lead, pass, crash, white, win\n"
	"\n"
	"; Phone remote: a web page for phones on the same network (the Phone page shows its QR code).\n"
	";   remote_key is the pairing code in the link; a new one locks out phones that had the old one.\n"
	"remote = 1\n"
	"remote_port = 8090\n"
	";   remote_scanner: phones can play AIMOD's scanner (AIMOD on this PC, its web server on aimod_port).\n"
	"remote_scanner = 1\n"
	"aimod_port = 8767\n"
	"\n";

static void LoadSettings() {
	FILE *f = fopen(g_ini, "r");
	if (!f) {
		f = fopen(g_ini, "w");
		if (f) { fputs(DEFAULT_INI, f); fclose(f); }
		f = fopen(g_ini, "r");
		if (!f) return;
	}
	char line[512];
	while (fgets(line, sizeof line, f)) {
		std::string l = Trim(line);
		if (l.empty() || l[0] == ';' || l[0] == '[') continue;
		size_t eq = l.find('=');
		if (eq == std::string::npos) continue;
		std::string k = Trim(l.substr(0, eq)), v = Trim(l.substr(eq + 1));
		S.raw[k] = v;
		if (k.rfind("cam_", 0) == 0) { S.cams[k.substr(4)] = IntList(v); continue; }
		if (k == "onboard_cams") { S.onboard = IntList(v); continue; }
		double d = atof(v.c_str());
		if (k == "min_hold") S.minHold = d;
		else if (k == "max_hold") S.maxHold = d;
		else if (k == "incident_hold") S.incidentHold = d;
		else if (k == "switch_margin") S.switchMargin = d;
		else if (k == "manual_hold") S.manualHold = d;
		else if (k == "qual_handoff_m") S.qualHandoffM = std::max(0.0, std::min(5000.0, d));
		else if (k == "qual_player_cam") S.qualPlayerCam = std::max(0, std::min(21, (int)d));
		else if (k == "battle_gap") S.battleGap = d;
		else if (k == "battle_min_age") S.battleMinAge = d;
		else if (k == "settle_laps") S.settleLaps = (int)d;
		else if (k == "short_track_m") S.shortTrackM = d;
		else if (k == "battle_short_m") S.battleShortM = d;
		else if (k == "onboard_hold") S.onboardHold = d;
		else if (k == "lead_battle_depth") S.leadBattleDepth = std::max(0, std::min(10, (int)d));
		else if (k == "lead_battle_hold") S.leadBattleHold = std::max(0.0, std::min(600.0, d));
		else if (k == "lead_battle_pack") S.leadBattlePack = std::max(2, std::min(40, (int)d));
		else if (k == "art_folders") S.artFolders = v;
		else if (k == "companion_folder") S.companionFolder = Trim(v);
		else if (k == "art_series") S.artSeries = v;
		else if (k == "overlay_style") S.overlayStyle = v;
		else if (k == "dash_accent") { unsigned rgb; if (sscanf(v.c_str() + (v[0] == '#'), "%x", &rgb) == 1) S.dashAccent = rgb; }
		else if (k == "dash_art_colors") S.dashArtColors = d != 0;
		else if (k == "dash_accent2") { unsigned rgb; if (sscanf(v.c_str() + (v[0] == '#'), "%x", &rgb) == 1) S.dashAccent2 = rgb; }
		else if (k == "dash_speed_max") S.dashSpeedMax = d;
		else if (k == "settle_positions") S.settlePositions = (int)d;
		else if (k == "settle_hold") S.settleHold = d;
		else if (k == "revisit_after") S.revisitAfter = d;
		else if (k == "tour_depth") S.tourDepth = (int)d;
		else if (k == "overlay_min_run_laps") S.overlayMinRunLaps = (int)d;
		else if (k == "overlay_gap") S.overlayGap = d;
		else if (k == "crash_drop") S.crashDrop = d;
		else if (k == "crash_window") S.crashWindow = d;
		else if (k == "replay_auto") S.replayAuto = d != 0;
		else if (k == "replay_caution_only") S.replayCautionOnly = d != 0;
		else if (k == "replay_preroll") S.replayPreroll = std::max(0.0, std::min(30.0, d));
		else if (k == "replay_after") S.replayAfter = std::max(0.0, std::min(120.0, d));
		else if (k == "replay_max") S.replayMax = std::max(4.0, std::min(60.0, d));
		else if (k == "replay_within") S.replayWithin = std::max(10.0, std::min(600.0, d));
		else if (k == "crash_damage") S.crashDamage = std::max(0.02, d);
		else if (k == "replay_worthy") S.replayWorthy = std::max(0.0, d);
		else if (k == "replay_angles") S.replayAngles = std::max(1, std::min(6, (int)d));
		else if (k == "replay_take") S.replayTake = std::max(3.0, std::min(30.0, d));
		else if (k == "replay_countdown") S.replayCountdown = std::max(0.0, std::min(30.0, d));
		else if (k == "reel_angles") S.reelAngles = std::max(1, std::min(6, (int)d));
		else if (k == "replay_drive") S.replayDrive = d != 0;
		else if (k == "highlights") S.highlights = d != 0;
		else if (k == "multi_pass_cars") S.hlMultiCars = std::max(2, std::min(20, (int)d));
		else if (k == "highlight_gap") S.hlGap = std::max(0.0, std::min(3600.0, d));
		else if (k == "bump_run_window") S.hlBumpWindow = std::max(0.5, std::min(10.0, d));
		else if (k == "bump_run_depth") S.hlBumpDepth = std::max(1, std::min(43, (int)d));
		else if (k == "superspeedways") S.superspeedways = v;
		else if (k == "replay_slow_impact") S.replaySlowImpact = std::max(0.0, d);
		else if (k == "highlight_bump") S.hlBump = d != 0;
		else if (k == "highlight_preroll") S.hlPreroll = std::max(1.0, std::min(20.0, d));
		else if (k == "replay_finish") S.replayFinish = d != 0;
		else if (k == "replay_finish_after") S.replayFinishAfter = std::max(0.0, std::min(60.0, d));
		else if (k == "reel_highlights") S.reelHighlights = d != 0;
		else if (k == "contact_join") S.contactJoin = std::max(0.0, d);
		else if (k == "crash_span") S.crashSpan = std::max(3.0, std::min(120.0, d));
		else if (k == "custom_cams") S.customCams = d != 0;
		else if (k == "chase2_view") S.chase2 = v;
		else if (k == "far_chase2_view") S.farChase2 = v;
		else if (k == "incar3_eye") S.incar3 = v;
		else if (k == "drone_height") S.droneHeight = std::max(5.0, std::min(200.0, d));
		else if (k == "drone_quiet") g_droneQuiet = d != 0;
		else if (k == "drone_distance") S.droneDistance = std::max(20.0, std::min(400.0, d));
		else if (k == "drone_top_mph") S.droneTopMph = std::max(60.0, std::min(400.0, d));
		else if (k == "drone_hover_m") S.droneHoverM = std::max(0.0, std::min(150.0, d));
		else if (k == "rear_pan") S.rearPan = d != 0;
		else if (k == "rear_pan_span") S.rearPanSpan = std::max(0.0, std::min(170.0, d));
		else if (k == "rear_pan_fov") S.rearPanFov = std::max(40.0, std::min(150.0, d));
		else if (k == "rear_mount") S.rearMount = d != 0;
		else if (k == "rear_mount_m") S.rearMountM = std::max(10.0, std::min(150.0, d));
		else if (k == "rear_hold_m") S.rearHoldM = std::max(10.0, std::min(200.0, d));
		else if (k == "rear_pass_m") S.rearPassM = std::max(0.0, std::min(30.0, d));
		else if (k == "rear_block_s") S.rearBlockS = std::max(0.0, std::min(10.0, d));
		else if (k == "replay_overlays") S.replayOverlays = d != 0;
		else if (k == "replay_banner") S.replayBanner = d != 0;
		else if (k == "replay_smart") S.replaySmart = d != 0;
		else if (k == "replay_slow_max") S.replaySlowMax = std::max(1, std::min(16, (int)d));
		else if (k == "replay_transition") S.replayTransition = d != 0;
		else if (k == "replay_wipe_color") { unsigned rgb; if (sscanf(v.c_str() + (v[0] == '#'), "%x", &rgb) == 1) S.replayWipeColor = rgb; }
		else if (k == "hide_companion_overlays") S.hideCompanions = d != 0;
		else if (k == "companion_apps") { S.companionApps = Trim(v); }
		else if (k == "companion_keep") S.companionKeep = Trim(v);
		else if (k == "replay_transition_ms") S.replayTransitionMs = std::max(400.0, std::min(5000.0, d));
		else if (k == "rear_pan_smooth") S.rearPanSmooth = std::max(0.05, std::min(3.0, d));
		else if (k == "rear_alone") S.rearFallback = d < 1 ? 11 : (int)d;
		else if (k == "live_after_climb_out") S.liveAfterClimbOut = d != 0;
		else if (k == "follow_after") S.followAfter = d;
		else if (k == "camera_every") S.cameraEvery = d;
		else if (k == "tv_cut_gap") S.tvCutGap = std::max(0.0, std::min(5.0, d));
		else if (k == "tv_cut_wait") S.tvCutWait = std::max(0.0, std::min(15.0, d));
		else if (k == "dyn_cams") S.dynCams = d != 0;
		else if (k == "dyn_pit") S.dynPit = d != 0;
		else if (k == "dyn_crash") S.dynCrash = d != 0;
		else if (k == "dyn_restart") S.dynRestart = d != 0;
		else if (k == "dyn_battle") S.dynBattle = d != 0;
		else if (k == "dyn_finish") S.dynFinish = d != 0;
		else if (k == "dyn_learn") S.dynLearn = d != 0;
		else if (k == "dyn_chance") S.dynChance = std::max(0.0, std::min(100.0, d));
		else if (k == "dyn_half_width") S.dynHalfWidth = std::max(0.0, std::min(60.0, d));
		else if (k == "dyn_yaw_fix") S.dynYawFix = std::max(-180.0, std::min(180.0, d));
		else if (k == "dyn_pitch_sign") S.dynPitchSign = d < 0 ? -1 : 1;
		else if (k == "dyn_rig") S.dynRig = d != 0;
		else if (k == "dyn_autozoom") S.dynAutoZoom = d != 0;
		else if (k == "dyn_smooth") S.dynSmooth = std::max(0.2, std::min(4.0, d));
		else if (k == "overlays") S.overlays = d != 0;
		else if (k == "camera_call") S.cameraCall = d != 0;
		else if (k == "tag_story") S.tagStory = d != 0;
		else if (k == "tag_rotate") S.tagRotate = std::max(2.0, d);
		else if (k == "stage_laps") S.stageLaps = IntList(v);
		else if (k == "crowd") S.crowd = d != 0;
		else if (k == "crowd_volume") S.crowdVolume = std::min(2.0, std::max(0.0, d));
		else if (k == "crowd_folder") S.crowdFolder = v;
		else if (k == "crowd_favourites") S.crowdFavourites = IntList(v);
		else if (k == "crowd_pass_depth") S.crowdPassDepth = (int)d;
		else if (k == "crowd_cooldown") S.crowdCooldown = d;
		else if (k == "crowd_events") S.crowdEvents = v;
	}
	fclose(f);
}

// replace (or add) one key in the ini, keeping everything else as it is
static void SaveKey(const std::string &key, const std::string &value) {
	std::vector<std::string> lines;
	FILE *f = fopen(g_ini, "r");
	char line[512];
	bool done = false;
	if (f) {
		while (fgets(line, sizeof line, f)) {
			std::string l = line;
			std::string t = Trim(l);
			size_t eq = t.find('=');
			if (!done && eq != std::string::npos && t[0] != ';' && Trim(t.substr(0, eq)) == key) {
				l = key + " = " + value + "\n"; done = true;
			}
			lines.push_back(l);
		}
		fclose(f);
	}
	if (!done) lines.push_back(key + " = " + value + "\n");
	f = fopen(g_ini, "w");
	if (!f) return;
	for (auto &l : lines) fputs(l.c_str(), f);
	fclose(f);
	S.raw[key] = value;
}

// ---------------------------------------------------------------- race state
static double Now() { return GetTickCount64() / 1000.0; }

struct Sample { double t; float v; };
// car numbers as painted: NR2003 stores "01" as 2001 and "00" as 2000 (some mods write "00" as 100); the art
// files, colors.json and what's shown on screen use the painted number
static std::string NumLabel(int v) {
	if (v == 100) return "00";
	if (v >= 2000) {
		int digits = v / 1000, n = v % 1000;
		if (digits >= 2 && digits <= 3 && n < (digits == 2 ? 100 : 1000)) {
			std::string s = std::to_string(n);
			while ((int)s.size() < digits) s = "0" + s;
			return s;
		}
	}
	return std::to_string(v);
}
static int NumKey(int v) { return v == 100 ? 2000 : v; }        // one key for "00", however the mod wrote it
static int RawNum(const std::string &text) {                     // "01" -> 2001, as NR2003 stores it
	std::string s;
	for (char ch : text) if (isdigit((unsigned char)ch)) s += ch;
	if (s.empty()) return -1;
	int n = atoi(s.c_str());
	return s.size() > 1 && s[0] == '0' ? (int)s.size() * 1000 + n : n;
}

struct Car {
	bool valid = false, pit = false, out = false;
	int number = 0;
	std::string first, last;
	int laps = 0;
	float dist = 0, speed = 0;       // metres on this lap, m/s
	float head = 0;                  // direction of travel (radians)
	// body damage, the game's four zones (rear, front, right, left - what AIMOD's damage column adds up); -1 = not
	// read yet / starts again (repaired in the pits, after a replay)
	float dmg[4] = {}, dmgTotal = -1, dmgStep = 0, dmgStepZ[4] = {};
	double hitStart = -1, hitLast = -1;   // a contact going on: game s of its first and latest damage gain
	float hitGain = 0, hitZ[4] = {}, hitSpeed = 0, hitPeak = 0, hitDist = 0;
	double hitPeakAt = -1;
	int hitNear = -1;
	double progress = 0;             // laps * track length + dist
	int pos = 0, stablePos = 0, candPos = 0;   // live order on track (battles, passes)
	int spos = 0;                    // official position: laps, then who crossed the line first
	unsigned cross = 0;              // game ms when it completed its last lap
	int scoredLaps = -1;
	long seq = 0;                    // order of line crossings when the table is not readable
	double candSince = 0;
	std::vector<Sample> hist;        // speed over the crash window (game time)
	struct Trace { double t; float v, dist, head, dmg; };
	std::vector<Trace> trace;        // the last 30 game s: speed, lap distance, direction of travel (crash log)
	double incidentAt = -1;          // real time
	const char *incidentWhy = "";
	double stoppedSince = -1;        // game time stopped on track
	double lastShown = -1e9;         // real time this car was last on screen
	double battleSince = -1;         // real time this car's current battle started
	int pitPhase = 0;                // 0 on track, 1 entering pit road, 2 in the stall, 3 leaving
	double pitPhaseAt = -1;          // real time the phase started
	double pitExitAt = -1;           // real time it rejoined the track
	int battleWith = -1;
	int pairWith = -1;               // the car one place ahead in the official order
	double passSince = -1;           // real time it got ahead of that car on track
	int gear = -9;                   // -1 reverse, 0 neutral, 1.. forward (-9 = unknown)
	unsigned aiState = 0;            // AI state object (single-car qualifying)
	int stallCount = 0, runEnd = 0, lapNum = 0;
	// qualifying delta: timed laps start at a line crossing made ON TRACK (never the out lap)
	unsigned qLastCross = 0;         // last crossing (game ms) we saw
	int qLapStart = -1;              // crossing that started the current timed lap, -1 = not on one
	std::vector<std::pair<float, int>> qTrace, qBestTrace;   // (lap distance m, ms into the lap)
	int qBestMs = 0, qLastLapMs = 0;
	double qLastLapAt = -1e9;        // real time the last timed lap finished
	float prevDist = -1;             // lap distance and real time last poll (measures the lap length)
	double prevT = 0;
	bool passed = false;
	// the race so far (for the driver tag)
	int startPos = 0;                // official position at the green flag
	int restartPos = 0;              // ... at the last green (start or restart)
	int stagePos[8] = {};            // finishing position of each stage
	int stageBase[8] = {};           // laps when the leader ended the stage (the car's next crossing counts)
	int lapsLed = 0, ledLap = -1;
	int pits = 0, pitLap = 0, pitInPos = 0, pitOutPos = 0;
	double pitStallAt = -1, pitStall = 0;   // game time in the stall
	bool pitOutPending = false;
	int storyLaps = -1;
};

static Car g_car[NUM_SLOTS];
static std::vector<int> g_order;    // slots by live order on track
static std::vector<int> g_score;    // slots by official position (what the tower shows)
static int g_greenLap = 0;          // leader lap when the current green run started
static float g_trackLen = 0;        // metres: track.ini length, or the longest lap distance seen
static float g_trackIniLen = 0;
static std::vector<float> g_lapLens;   // lap lengths measured at the line (where the lap distance resets)
static std::string g_trackName;
static int g_flag = 0, g_lastFlag = 0, g_session = 0, g_lapsTotal = 0, g_leaderLaps = 0;
static double g_simT = 0, g_restartUntil = -1, g_fieldSpeed = 0;
// the replay tape against the game clock, a sample a second: crashes are kept in game seconds, but the tape doesn't
// always get a frame per tick (tester's Atlanta race: the clock ran 1311 s while the tape got 854 s - replays from
// "clock seconds back" landed 15 laps early). A replay goes back by TAPE seconds, looked up here.
struct TapeAt { double t; int frames; };
static std::vector<TapeAt> g_tapeMap;
// the tape transport (racevision_tape.cpp) and the replay transition (full-screen TV wipe)
static void TpReset();
static void TpMarkTick(Game &g, int frames);
static void TpService(Game &g);
static void TpSetSpeed(Game &g, int step, bool slow);
static void TpQueue(const std::string &op, int n = 0, double v = 0);
static void CompanionHide();                                          // the companion apps' overlay windows out of sight (replays, wipes)
static void CompanionShow();
static void CompanionTick();
static std::string TpSpeedText(Game &g);
static const DWORD TP_IDX = 0xB8E5A2, TP_STEP = 0xB8E5AA, TP_SLOW = 0xB8E5AE;     // current tape frame; frames (or slow ticks) per step; slow flag
static int TpRate(Game &g) { int r = g.Get<int>(TICK_RATE_FLAG, 1) != 0 ? 36 : g.Get<int>(TICK_RATE_VALUE, 36); return r > 0 && r <= 10000 ? r : 36; }
static bool g_tpManual = false;            // the tape is in the transport's hands (scrub, speed, lap jump), not a crash take
static void TransitionStart(int kind, const std::string &sub);      // 0 replay, 1 back to live, 2 new angle
static void TransitionCover();                                        // waits until the screen is covered
static void TransitionRelease();                                      // the cut is made: uncover
struct TrGuard { bool on; TrGuard() : on(false) {} ~TrGuard() { if (on) TransitionRelease(); } };
static void TrBegin(TrGuard &tg, int kind, const std::string &sub) { if (!S.replayTransition) return; TransitionStart(kind, sub); tg.on = true; TransitionCover(); }
static int g_passCar = -1, g_passVictim = -1;
static double g_passAt = -1;
static int g_greens = 0;            // green flags this race (1 = the start)
static int g_firstLeaderLaps = -1;  // leader laps when we first saw this race (stages before it are unknown)
static bool g_stageDone[8] = {};
static bool g_greenNow = false;
static bool g_playerPit = true;     // your car's pit road flag (slot 0)
static int g_raceNo = 0;            // counts race sessions (ResetRace)
static std::map<int, std::vector<Sample>> g_gapHist;   // trailing slot -> gap history to the car ahead

// crash log: every crash / stopped car, with the cars caught up in it and the game time it really started
// (the first contact or speed loss, found afterwards in the cars' traces), so it can be replayed
struct Crash {
	int id = 0, lap = 0;
	double first = 0;                // game s: first sign of trouble (replays start replay_preroll before it)
	double lastAt = 0;               // game s: the last car caught up in it
	double realAt = 0;               // real time the last car got caught up (the crash is over replay_after later)
	std::vector<int> cars;           // in the order they got into it
	std::vector<double> firsts;      // each car's own first sign (game s), same order
	std::string how;                 // what the first sign was
	bool replayed = false;
	double impactAt = -1;            // game s of the hardest hit (the biggest single jump in damage) - the replay's guide
	float impact = 0;                // that jump
	int impactCar = -1;
	std::vector<float> spots;        // lap distances where its cars got hit (a contact near one of them joins it)
	float damage = 0;                // all the damage its contacts added
	std::vector<float> carHit, carPeak;   // per car (same order as cars): damage it took, its hardest jump
	std::vector<double> carPeakAt;        //   and when that was
	double worthyAt = -1;            // real time it became worth a replay (the alert)
	// a highlight that isn't a crash (racevision_highlights.cpp, kept in g_highs): the same replay machinery, with
	// impactAt = the moment (the pass, the bump, the line), impactCar = the car it is about, lastAt = its end
	int kind = 0;                    // HL_CRASH, or HL_PASS / HL_MULTI / HL_BUMP / HL_FINISH
	double score = 0;                // a highlight's worth (crashes: Worth)
	int other = -1;                  // the car passed / bumped / beaten to the line
	std::string what;                // "LAST LAP: PASS FOR P2", "2 CARS IN ONE MOVE", "WINS BY 0.214 S" ...
	bool lastLap = false;            // a pass-type highlight on the last lap (outside highlight_gap)
};
enum { HL_CRASH = 0, HL_PASS, HL_MULTI, HL_BUMP, HL_FINISH };
static const char *HlTitle(int kind) {
	static const char *t[] = { "CRASH", "PASS", "ON THE MOVE", "BUMP AND RUN", "THE FINISH" };
	return kind >= 0 && kind <= HL_FINISH ? t[kind] : "REPLAY";
}
static const char *HlKey(int kind) {
	static const char *t[] = { "crash", "pass", "multi", "bump", "finish" };
	return kind >= 0 && kind <= HL_FINISH ? t[kind] : "crash";
}
static std::string HlLabel(const Crash &c) { return std::string(c.kind ? "highlight" : "crash") + " #" + std::to_string(c.id); }
static void CarVecs(Crash &c) {
	c.carHit.resize(c.cars.size(), 0); c.carPeak.resize(c.cars.size(), 0); c.carPeakAt.resize(c.cars.size(), -1);
}
// how much a crash is worth showing again: the damage done, and more for every car caught up in it (a pile-up beats
// two cars touching - live 2026-10-26: a 2-car tangle got the replay over the big one). No damage read = by cars.
static bool g_dmgSeen = false;               // this race's damage can be read (a car's damage went up)
static double Worth(const Crash &c) {
	if (c.kind) return c.score;
	double n = (double)c.cars.size();
	if (c.damage > 0) return c.damage + 0.5 * (n - 1);
	return g_dmgSeen ? 0 : 0.8 * n;              // damage is read and nobody took any: not worth a replay
}
// every contact (damage going up), the last 2 minutes: the crash log looks back at them
struct Contact { int car; double start, end, peakAt; float gain, peak, dist; int nearCar; bool used; float rear = 0, front = 0; };
static std::vector<Contact> g_contacts;
static std::vector<Crash> g_crashes;
static int g_crashSeq = 0;
static std::vector<Crash> g_highs;          // the other highlights (ids from 1001, so they never meet a crash's)
static int g_hlSeq = 1000;
static void HighlightWatch();               // racevision_highlights.cpp: each poll, after the passes
static void HlReset();

static std::string Short(int i) {
	const Car &c = g_car[i];
	return c.last.empty() ? c.first : c.last;
}
static std::string Name(int i) {
	if (i < 0 || i >= NUM_SLOTS) return "?";
	char buf[96];
	snprintf(buf, sizeof buf, "#%s %s %s", NumLabel(g_car[i].number).c_str(), g_car[i].first.c_str(), g_car[i].last.c_str());
	return buf;
}

// game seconds from the tick clock (the old session clock only if the ticks can't be read)
static double GameSec(Game &g) {
	int ticks = g.Get<int>(GAME_TICKS, 0);
	if (ticks <= 0) return g.Get<double>(SESSION_TIME, -1);
	int rate = g.Get<int>(TICK_RATE_FLAG, 1) != 0 ? 36 : g.Get<int>(TICK_RATE_VALUE, 36);
	if (rate <= 0 || rate > 10000) rate = 36;
	return ticks / (double)rate;
}

// Game clock: the tick clock above. (The old session clock froze for minutes; the fallback below
// - real time while cars move and the clock stands still - is kept for that case.)
static double SimClock(Game &g) {
	static double lastSim = -1, lastReal = 0, base = 0;
	double t = GameSec(g), now = Now();
	if (t != lastSim) { lastSim = t; lastReal = now; base = t; return t; }
	if (g_fieldSpeed > 5 && now - lastReal > 0.5) return base + (now - lastReal);
	return base;
}

// metres a is ahead of b on track (negative = behind), from the lap distances only: the lap
// counter rolls over at a different point than the distance, so it is left out here
static double Ahead(int a, int b) {
	double L = std::max(g_trackLen, 400.0f), d = g_car[a].dist - g_car[b].dist;
	while (d > L / 2) d -= L;
	while (d < -L / 2) d += L;
	return d;
}
// gap in seconds from b back to a
static double Gap(int a, int b) { return Ahead(a, b) / std::max(g_car[b].speed, 10.0f); }
// two cars on the same lap (or within one line crossing of it)
static bool SameLap(int a, int b) { return fabs(g_car[a].progress - g_car[b].progress) < std::max(g_trackLen, 400.0f) * 0.5; }

static bool g_planReset = false;    // ResetRace: the replay plan and reel go too (they live further down)
static void ResetRace() {
	for (int i = 0; i < NUM_SLOTS; i++) {
		g_car[i].hist.clear(); g_car[i].trace.clear(); g_car[i].incidentAt = -1; g_car[i].stablePos = g_car[i].candPos = 0;
		g_car[i].battleSince = -1; g_car[i].stoppedSince = -1; g_car[i].progress = 0;
	}
	g_trackLen = g_trackIniLen; g_passCar = -1; g_gapHist.clear(); g_score.clear(); g_greenLap = 0;
	g_lapLens.clear();
	for (int i = 0; i < NUM_SLOTS; i++) {
		Car &c = g_car[i];
		c.scoredLaps = -1; c.spos = 0;
		c.startPos = c.restartPos = 0; c.lapsLed = 0; c.ledLap = -1; c.pits = c.pitLap = c.pitInPos = c.pitOutPos = 0;
		c.pitStallAt = -1; c.pitStall = 0; c.pitOutPending = false; c.storyLaps = -1;
		for (int s = 0; s < 8; s++) c.stagePos[s] = c.stageBase[s] = 0;
	}
	g_greens = 0; g_firstLeaderLaps = -1; g_greenNow = false; g_raceNo++;
	g_crashes.clear(); g_crashSeq = 0; g_contacts.clear(); g_planReset = true; g_dmgSeen = false;
	g_highs.clear(); g_hlSeq = 1000; HlReset();
	g_tapeMap.clear(); TpReset();
	for (bool &d : g_stageDone) d = false;
}

// what the driver tag tells about each car's race: start, restarts, stages, pit stops, laps led
static void RaceStory() {
	if (g_session != SESSION_RACE || g_score.empty()) return;
	if (g_firstLeaderLaps < 0) g_firstLeaderLaps = g_leaderLaps;
	// the grid: the order before the green, if we saw the start
	if (g_greens == 0 && g_flag != FLAG_GREEN && g_leaderLaps <= 1 && g_firstLeaderLaps <= 1)
		for (int i : g_score) g_car[i].startPos = g_car[i].spos;
	if (g_greenNow) {
		g_greens++;
		for (int i : g_score) g_car[i].restartPos = g_car[i].spos;
	}
	// stages: the leader ends it, everyone else at their next line crossing
	for (size_t s = 0; s < S.stageLaps.size() && s < 8; s++) {
		int L = S.stageLaps[s];
		if (!g_stageDone[s] && g_leaderLaps >= L && g_firstLeaderLaps < L) {
			g_stageDone[s] = true;
			for (int i : g_score) g_car[i].stageBase[s] = g_car[i].laps;
			for (int i : g_score) if (g_car[i].laps >= L) g_car[i].stagePos[s] = g_car[i].spos;
		}
	}
	int leader = g_score[0];
	for (int i : g_score) {
		Car &c = g_car[i];
		bool crossed = c.storyLaps >= 0 && c.laps != c.storyLaps;
		c.storyLaps = c.laps;
		if (crossed) {
			for (size_t s = 0; s < S.stageLaps.size() && s < 8; s++)
				if (g_stageDone[s] && !c.stagePos[s] && c.laps > c.stageBase[s]) c.stagePos[s] = c.spos;
			if (c.pitOutPending) { c.pitOutPos = c.spos; c.pitOutPending = false; }
		}
		if (i == leader && c.laps > c.ledLap && c.laps > 0) { if (c.ledLap >= 0) c.lapsLed++; c.ledLap = c.laps; }
	}
}

// "track_length = 0.533m" (miles) in tracks\<folder>\track.ini
static float TrackIniLength(const std::string &track) {
	std::string p = g_gameDir + "\\tracks\\" + track + "\\track.ini";
	FILE *f = fopen(p.c_str(), "r");
	if (!f) return 0;
	char line[512];
	float len = 0;
	while (fgets(line, sizeof line, f)) {
		std::string l = Trim(line);
		if (_strnicmp(l.c_str(), "track_length", 12) != 0 || l.find('=') == std::string::npos) continue;
		std::string k = Trim(l.substr(0, l.find('=')));
		if (_stricmp(k.c_str(), "track_length") != 0) continue;
		const char *v = l.c_str() + l.find('=') + 1;
		char *end;
		double x = strtod(v, &end);
		while (*end == ' ') end++;
		len = (float)((*end == 'k' || *end == 'K') ? x * 1000 : x * 1609.344);   // "m" = miles, "km"
		break;
	}
	fclose(f);
	return len > 100 && len < 50000 ? len : 0;
}

// A crash is one car losing far more speed than the cars around it. When they all slow together - the field checking
// up behind the pace car as the caution comes out, or an accordion in the pack - it is not (live 2026-10-26: 24 cars
// logged as one crash when a caution came out at Talladega). The cars within 150 m (else the whole field) are compared:
// they lost at least 60 % as much speed, and this car is still at 60 % of their speed or more = a checkup. Cars that
// were already much slower (crashed, stopped) don't count.
// The first direction kink in car i's trace between game times from and to (-1 if none): its turn rate jumping by
// over 0.6 rad/s from one sample to the next - a hit or a spin; a steady corner or a lane change turns it smoothly.
static double Kink(int i, double from, double to) {
	auto &tr = g_car[i].trace;
	for (size_t j = 2; j < tr.size(); j++) {
		if (tr[j].t < from || tr[j].t > to) continue;
		double dt1 = tr[j].t - tr[j - 1].t, dt0 = tr[j - 1].t - tr[j - 2].t;
		if (dt1 <= 0.02 || dt0 <= 0.02 || tr[j - 1].v < 10) continue;
		auto turn = [](float a, float b) { double d = a - b; const double PI = 3.14159265358979; while (d > PI) d -= 2 * PI; while (d < -PI) d += 2 * PI; return d; };
		double w1 = turn(tr[j].head, tr[j - 1].head) / dt1, w0 = turn(tr[j - 1].head, tr[j - 2].head) / dt0;
		if (fabs(w1 - w0) > 0.6) return tr[j - 1].t;
	}
	return -1;
}

static bool Checkup(int i, float drop) {
	// a car that got knocked off its line (hit, spun) next to another one that did, or that ended up clearly slower
	// than the cars around it, crashed, whatever the rest did: in a big wreck they all lose speed together too.
	// (One car swerving on its own as the field checks up is still a checkup.)
	double from = g_simT - S.crashWindow - 1.5;
	bool kinked = Kink(i, from, g_simT) >= 0, pair = false;
	if (kinked)
		for (int j : g_order)
			if (j != i && g_car[j].valid && !g_car[j].pit && fabs(Ahead(j, i)) < 200 && Kink(j, from, g_simT) >= 0) pair = true;
	if (pair) return false;
	std::vector<float> nd, nv, fd, fv;
	float myTop = g_car[i].speed;
	for (auto &s : g_car[i].hist) myTop = std::max(myTop, s.v);
	for (int j : g_order) {
		const Car &o = g_car[j];
		if (j == i || !o.valid || o.pit || o.out) continue;
		float top = o.speed;
		for (auto &s : o.hist) top = std::max(top, s.v);
		if (top < 0.7f * myTop) continue;                // already slow (crashed, stopped): it has nothing left to lose
		fd.push_back(top - o.speed); fv.push_back(o.speed);
		if (fabs(Ahead(j, i)) < 150) { nd.push_back(top - o.speed); nv.push_back(o.speed); }
	}
	std::vector<float> &d = nd.size() >= 2 ? nd : fd, &v = nd.size() >= 2 ? nv : fv;
	if (d.size() < 2) return false;
	std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
	std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
	float medDrop = d[d.size() / 2], medV = v[v.size() / 2];
	bool together = medDrop >= 0.6f * drop && g_car[i].speed >= (kinked ? 0.85f : 0.6f) * medV;
	if (together) {
		static double lastLog = -100;
		if (Now() - lastLog > 10) {
			lastLog = Now();
			Log("           checkup, not a crash: %s lost %.0f mph, the cars %s %.0f mph\n", Name(i).c_str(), drop * 2.237f,
			    nd.size() >= 2 ? "around it" : "on track", medDrop * 2.237f);
		}
	}
	return together;
}

// ---- crash log
// The trace sample of car i nearest game time t (nullptr if it has none within 0.3 s)
static const Car::Trace *TraceAt(int i, double t) {
	const Car::Trace *best = nullptr;
	for (auto &s : g_car[i].trace) if (!best || fabs(s.t - t) < fabs(best->t - t)) best = &s;
	return best && fabs(best->t - t) < 0.3 ? best : nullptr;
}
static float Wrap(float d) {                         // metres a - b around one lap
	float L = std::max(g_trackLen, 400.0f);
	while (d > L / 2) d -= L;
	while (d < -L / 2) d += L;
	return d;
}

// When did car i's trouble start, for an incident seen at game time T? The speed loss starts at the last sample
// still at the peak speed before the drop; a hit shows earlier as a sudden kink in the direction of travel (a
// steady corner turns it smoothly). Also names the nearest car at that moment (likely the one it touched).
static double FirstSign(int i, double T, bool stopped, std::string &how, int &nearCar) {
	auto &tr = g_car[i].trace;
	nearCar = -1;
	if (tr.size() < 3) { how = "no trace"; return T; }
	double from = T - (stopped ? 12.0 : S.crashWindow + 3.0);
	size_t peak = 0; bool any = false;
	for (size_t k = 0; k < tr.size(); k++)
		if (tr[k].t >= from && tr[k].t <= T && (!any || tr[k].v > tr[peak].v)) { peak = k; any = true; }
	if (!any) { how = "no trace"; return T; }
	size_t k = peak;
	while (k + 1 < tr.size() && tr[k + 1].t <= T && tr[k + 1].v > tr[peak].v - 1.5f) k++;
	double loss = tr[k].t;
	double kink = Kink(i, loss - 1.5, loss + 0.2);
	double first = kink >= 0 && kink < loss ? kink : loss;
	const Car::Trace *me = TraceAt(i, first);
	float bestGap = 8;                                  // within 8 m (lap distance) counts as alongside / at the bumper
	for (int j = 0; me && j < NUM_SLOTS; j++) {
		if (j == i || !g_car[j].valid) continue;
		const Car::Trace *o = TraceAt(j, first);
		if (o && fabs(Wrap(o->dist - me->dist)) < bestGap) { bestGap = fabs(Wrap(o->dist - me->dist)); nearCar = j; }
	}
	char b[160];
	snprintf(b, sizeof b, "%s %.1fs before it showed%s%s", first == kink ? "a kink in its line" : "speed loss", T - first,
	         nearCar >= 0 ? ", next to " : "", nearCar >= 0 ? Name(nearCar).c_str() : "");
	how = b;
	return first;
}

// damage car i gained since game time from (-1 if it can't tell)
static float DmgGain(int i, double from) {
	const Car &c = g_car[i];
	if (c.dmgTotal < 0) return -1;
	for (auto &s : c.trace) if (s.t >= from && s.dmg >= 0) return std::max(0.0f, c.dmgTotal - s.dmg);
	return -1;
}

static std::string DmgText(int i, double from) {
	float d = DmgGain(i, from);
	if (d < 0) return "";
	char b[40]; snprintf(b, sizeof b, ", damage +%.3f", d); return b;
}

struct Crash; static void CrashSweepContacts(Crash &cr);
// a car just crashed / stopped: add it to the crash it belongs to (one still going on, close by on track or with a
// car it was next to), or start a new one
static void CrashNote(int i, const char *why) {
	if (g_session != SESSION_RACE) return;
	for (auto &c : g_crashes)                        // still in one (a stopped car is seen again every incident_hold)
		if (g_simT - c.lastAt < 60 && std::find(c.cars.begin(), c.cars.end(), i) != c.cars.end()) return;
	std::string how; int nearCar = -1;
	double first = FirstSign(i, g_simT, strcmp(why, "STOPPED") == 0, how, nearCar);
	Crash *into = nullptr;
	for (auto &c : g_crashes) {
		if (g_simT - c.lastAt > 10) continue;
		for (int o : c.cars) if (o == i || o == nearCar || fabs(Ahead(o, i)) < 300) { into = &c; break; }
		if (into) break;
	}
	SYSTEMTIME st; GetLocalTime(&st);
	if (into) {
		if (std::find(into->cars.begin(), into->cars.end(), i) != into->cars.end()) return;
		into->cars.push_back(i); into->firsts.push_back(first);
		{ const Car::Trace *at = TraceAt(i, first); into->spots.push_back(at ? at->dist : g_car[i].dist); }
		into->lastAt = g_simT; into->realAt = Now();
		if (first < into->first) { into->first = first; into->how = Name(i) + ": " + how; }
		Log("[%02d:%02d:%02d] CRASH #%d  + %s (%s, %s%s)\n", st.wHour, st.wMinute, st.wSecond, into->id, Name(i).c_str(), why, how.c_str(), DmgText(i, first - 1).c_str());
		return;
	}
	Crash c;
	c.id = ++g_crashSeq; c.lap = g_leaderLaps; c.first = first; c.lastAt = g_simT; c.realAt = Now();
	c.cars.push_back(i); c.firsts.push_back(first); c.how = Name(i) + ": " + how;
	{ const Car::Trace *at = TraceAt(i, first); c.spots.push_back(at ? at->dist : g_car[i].dist); }
	g_crashes.push_back(c);
	if (g_crashes.size() > 40) g_crashes.erase(g_crashes.begin());
	CrashSweepContacts(g_crashes.back());
	Log("[%02d:%02d:%02d] CRASH #%d  lap %d, P%d %s (%s) - first sign: %s%s\n", st.wHour, st.wMinute, st.wSecond, c.id, c.lap,
	    g_car[i].spos, Name(i).c_str(), why, how.c_str(), DmgText(i, first - 1).c_str());
}

// The cars caught up in a crash that never lost crash_drop of speed themselves - knocked sideways and carried on,
// or slowed with everyone else behind it (live 2026-10-26, Talladega: one car logged for a multi-car wreck, so the
// replay followed whoever happened to trip the detector). While a crash is going on, a car within 200 m of one in
// it whose line kinked (a hit or a spin) since just before the crash started joins it.
static void CrashSweep() {
	if (g_session != SESSION_RACE) return;
	double now = Now();
	for (auto &cr : g_crashes) {
		if (g_simT - cr.lastAt > 8 || g_simT - cr.first > 40) continue;
		for (int j : g_order) {
			Car &o = g_car[j];
			if (!o.valid || o.pit || o.out) continue;
			bool in = false;
			for (auto &c2 : g_crashes)
				if (g_simT - c2.lastAt < 60 && std::find(c2.cars.begin(), c2.cars.end(), j) != c2.cars.end()) in = true;
			if (in) continue;
			bool close = false;
			for (int k : cr.cars) if (g_car[k].valid && fabs(Ahead(k, j)) < 200) close = true;
			if (!close) continue;
			double kink = Kink(j, cr.first - 1.0, g_simT);
			if (kink < 0) continue;
			cr.cars.push_back(j); cr.firsts.push_back(kink);
			cr.lastAt = g_simT; cr.realAt = now;
			if (kink < cr.first) { cr.first = kink; cr.how = Name(j) + ": a kink in its line"; }
			if (now - o.incidentAt > S.incidentHold) { o.incidentAt = now; o.incidentWhy = "CRASH"; }
			SYSTEMTIME st; GetLocalTime(&st);
			Log("[%02d:%02d:%02d] CRASH #%d  + %s (in it: its line kinked %.1fs after the crash started, %.0f mph now%s)\n",
			    st.wHour, st.wMinute, st.wSecond, cr.id, Name(j).c_str(), kink - cr.first, o.speed * 2.237f, DmgText(j, kink - 1).c_str());
		}
	}
}

// Contact log (test, 2026-10-26): every time a car's body damage goes up, one line once it has stopped going up for
// 0.5 s - how much, which side, how fast it was going and the car next to it when it started. To learn what a rub
// and what a wreck look like before damage is used to tell crashes from checkups.
static void DamageCrash(Contact &ct);
static void ContactWatch() {
	static const char *zn[4] = { "rear", "front", "right", "left" };
	static double lastT = -1;
	if (lastT > g_simT) lastT = -1;
	for (int i : g_order) {
		Car &c = g_car[i];
		if (c.dmgStep > 0) {
			if (c.hitStart < 0) {
				c.hitStart = g_simT; c.hitGain = 0; c.hitSpeed = c.speed; c.hitNear = -1; c.hitPeak = 0; c.hitDist = c.dist;
				for (float &z : c.hitZ) z = 0;
				float best = 12;
				for (int j : g_order)
					if (j != i && g_car[j].valid && fabs(Ahead(j, i)) < best) { best = (float)fabs(Ahead(j, i)); c.hitNear = j; }
			}
			c.hitGain += c.dmgStep; c.hitLast = g_simT;
			if (c.dmgStep > c.hitPeak) {                  // the hardest jump: between the last read and this one
				c.hitPeak = c.dmgStep;
				c.hitPeakAt = lastT > 0 && g_simT - lastT < 0.5 ? (lastT + g_simT) / 2 : g_simT;
			}
			for (int k = 0; k < 4; k++) c.hitZ[k] += c.dmgStepZ[k];
		}
		if (c.hitStart >= 0 && g_simT - c.hitLast > 0.5) {
			std::string where;
			for (int k = 0; k < 4; k++)
				if (c.hitZ[k] > 0.0005f) { char b[32]; snprintf(b, sizeof b, "%s%s %.3f", where.empty() ? "" : ", ", zn[k], c.hitZ[k]); where += b; }
			SYSTEMTIME st; GetLocalTime(&st);
			Log("[%02d:%02d:%02d] CONTACT  %-28s +%.3f (%s) over %.1fs at %.0f mph%s%s%s, lap %d, damage now %.3f\n",
			    st.wHour, st.wMinute, st.wSecond, Name(i).c_str(), c.hitGain, where.c_str(), c.hitLast - c.hitStart,
			    c.hitSpeed * 2.237f, c.hitNear >= 0 ? ", next to " : "", c.hitNear >= 0 ? Name(c.hitNear).c_str() : "",
			    c.pit ? ", on pit road" : "", c.laps, c.dmgTotal);
			double st0 = c.hitStart;
			c.hitStart = -1;
			Contact ct = { i, st0, c.hitLast, c.hitPeakAt, c.hitGain, c.hitPeak, c.hitDist, c.hitNear, false };
			ct.rear = c.hitZ[0]; ct.front = c.hitZ[1];     // (bump and runs: the nose of the one behind, the tail of the one ahead)
			g_contacts.push_back(ct);
			while (!g_contacts.empty() && g_simT - g_contacts.front().end > 120) g_contacts.erase(g_contacts.begin());
			DamageCrash(g_contacts.back());
		}
	}
	lastT = g_simT;
}

// A contact into the crash log. A crash going on (its last car within 10 s) takes any contact of contact_join or more
// within 400 m of where one of its cars was hit; else a contact of crash_damage or more starts a new crash, and the
// smaller contacts just before it close by join it. Each crash keeps its hardest hit - the biggest single jump in
// damage - to start the replay from and to follow (live 2026-10-26, Talladega: the speed log found one of three
// wrecks, 13 s late; the damage log had every hit, the hardest +1.0 in one 50 ms read).
static bool NearSpots(const Crash &cr, float dist) {
	for (float d : cr.spots) if (fabs(Wrap(dist - d)) < 400) return true;
	return false;
}
static void CrashTakes(Crash &cr, Contact &ct, bool log) {
	ct.used = true;
	if (ct.peak > cr.impact) { cr.impact = ct.peak; cr.impactAt = ct.peakAt; cr.impactCar = ct.car; }
	cr.spots.push_back(ct.dist);
	if (ct.start < cr.first) { cr.first = ct.start; cr.how = Name(ct.car) + ": hit"; }
	cr.lastAt = std::max(cr.lastAt, ct.end); cr.realAt = Now();
	bool had = std::find(cr.cars.begin(), cr.cars.end(), ct.car) != cr.cars.end();
	if (!had) { cr.cars.push_back(ct.car); cr.firsts.push_back(ct.start); }
	CarVecs(cr);
	size_t k = std::find(cr.cars.begin(), cr.cars.end(), ct.car) - cr.cars.begin();
	cr.damage += ct.gain; cr.carHit[k] += ct.gain;
	if (ct.peak > cr.carPeak[k]) { cr.carPeak[k] = ct.peak; cr.carPeakAt[k] = ct.peakAt; }
	Car &c = g_car[ct.car];
	if (!c.pit && Now() - c.incidentAt > S.incidentHold) { c.incidentAt = Now(); c.incidentWhy = "CRASH"; }
	if (!log) return;
	SYSTEMTIME st; GetLocalTime(&st);
	Log("[%02d:%02d:%02d] CRASH #%d  %s %s hit +%.3f (hardest jump +%.3f)%s%s - hardest in the crash: +%.3f %s\n",
	    st.wHour, st.wMinute, st.wSecond, cr.id, had ? "  " : "+", Name(ct.car).c_str(), ct.gain, ct.peak,
	    ct.nearCar >= 0 ? ", next to " : "", ct.nearCar >= 0 ? Name(ct.nearCar).c_str() : "", cr.impact,
	    cr.impactCar >= 0 ? Short(cr.impactCar).c_str() : "");
}
// the smaller contacts already logged that belong to crash cr (close by, from 10 s before it on)
static void CrashSweepContacts(Crash &cr) {
	for (auto &o : g_contacts)
		if (!o.used && o.gain >= S.contactJoin && o.end >= cr.first - 5 && o.start <= cr.first + S.crashSpan && NearSpots(cr, o.dist))
			CrashTakes(cr, o, true);
}
static void DamageCrash(Contact &ct) {
	if (g_session != SESSION_RACE || ct.gain < S.contactJoin) return;
	for (auto &cr : g_crashes)
		if (ct.start - cr.lastAt < 10 && ct.start > cr.first - 30 && ct.start - cr.first <= S.crashSpan && (NearSpots(cr, ct.dist) ||
		    std::find(cr.cars.begin(), cr.cars.end(), ct.car) != cr.cars.end())) {
			CrashTakes(cr, ct, true);
			return;
		}
	if (ct.gain < S.crashDamage) return;
	Crash c;
	c.id = ++g_crashSeq; c.lap = g_leaderLaps; c.first = ct.start; c.lastAt = ct.end; c.realAt = Now();
	g_crashes.push_back(c);
	if (g_crashes.size() > 40) g_crashes.erase(g_crashes.begin());
	Crash &cr = g_crashes.back();
	SYSTEMTIME st; GetLocalTime(&st);
	Log("[%02d:%02d:%02d] CRASH #%d  lap %d, P%d %s hit +%.3f (hardest jump +%.3f)%s%s\n", st.wHour, st.wMinute, st.wSecond,
	    cr.id, cr.lap, g_car[ct.car].spos, Name(ct.car).c_str(), ct.gain, ct.peak,
	    ct.nearCar >= 0 ? ", next to " : "", ct.nearCar >= 0 ? Name(ct.nearCar).c_str() : "");
	CrashTakes(cr, ct, false);
	CrashSweepContacts(cr);
}

// the car to follow in a crash's replay: the one whose trouble started first (it set the wreck off)
static int CrashLead(const Crash &c) {
	int best = -1; double t = 0;
	for (size_t k = 0; k < c.cars.size(); k++) {
		double f = k < c.firsts.size() ? c.firsts[k] : c.first;
		if (best < 0 || f < t) { best = c.cars[k]; t = f; }
	}
	return best;
}

static void Poll(Game &g) {
	char tn[33] = {};
	if (g.Read(TRACK_NAME, tn, 32) && tn[0] && g_trackName != tn) {
		g_trackName = tn;
		g_trackIniLen = TrackIniLength(g_trackName);
		g_trackLen = g_trackIniLen;
		if (g_trackIniLen > 0) Log("Track: %s, %.0f m (track.ini)\n", tn, g_trackIniLen);
		else Log("Track: %s (no length in track.ini, measuring it)\n", tn);
	}
	g_flag = g.Get<int>(FLAG_STATE, 0);
	g_session = g.Get<unsigned char>(SESSION_STATE, 0);
	g_lapsTotal = g.Get<int>(LAPS_TOTAL, 0);
	g_greenNow = g_flag == FLAG_GREEN && g_lastFlag != FLAG_GREEN && g_lastFlag != 0;
	if (g_greenNow) { g_restartUntil = Now() + 12; g_greenLap = g_leaderLaps; }
	g_lastFlag = g_flag;
	unsigned char pitRoad[NUM_SLOTS] = {};
	g.Read(ON_PIT_ROAD, pitRoad, NUM_SLOTS);
	g_playerPit = pitRoad[0] != 0;

	static std::vector<unsigned char> buf(BLOCK_SIZE);
	static double lastNames = -100;
	bool names = Now() - lastNames > 5;
	if (names) lastNames = Now();
	std::vector<float> speeds;
	for (int i = 0; i < NUM_SLOTS; i++) {
		Car &c = g_car[i];
		if (i == PACE_SLOT || !g.Read(BLOCK0 + i * BLOCK_SIZE, buf.data(), BLOCK_SIZE)) { c.valid = false; continue; }
		auto F = [&](int off) { float v; memcpy(&v, &buf[off], 4); return v; };
		float grip = F(OFF_GRIP);
		bool gripOk = grip > 0.01f && grip <= 1.05f;
		if (!gripOk && i != 0) { c.valid = false; continue; }
		if (names || (c.first.empty() && c.last.empty())) {
			char de[DRIVER_SIZE];
			if (g.Read(DRIVER0 + i * DRIVER_SIZE, de, DRIVER_SIZE)) {
				c.first.assign(de, strnlen(de, 16));
				c.last.assign(de + 16, strnlen(de + 16, 20));
				memcpy(&c.number, de + 156, 4);
			}
		}
		if (c.number < 0 || _stricmp((c.first + " " + c.last).c_str(), "pace car") == 0) { c.valid = false; continue; }
		// a roster under the full field leaves blank AI filler slots (no name, number 0) that still carry a grip figure:
		// not cars - they sit in the pits and would otherwise take the camera, the log and the director list
		if (i != 0 && c.first.find_first_not_of(" 	") == std::string::npos && c.last.find_first_not_of(" 	") == std::string::npos) { c.valid = false; continue; }
		// the player's car (slot 0): NR2003 keeps no AI grip figure for it, so it stays out of the TV rotation, but its
		// name is read for PlayerDrive (Race Control's `drive` = your own qualifying run)
		if (!gripOk) { c.valid = false; continue; }
		c.valid = true;
		memcpy(&c.aiState, &buf[OFF_AI_STATE], 4);
		memcpy(&c.stallCount, &buf[OFF_STALL_COUNT], 4);
		memcpy(&c.runEnd, &buf[OFF_RUN_END], 4);
		memcpy(&c.lapNum, &buf[OFF_LAP_NUM], 4);
		int laps; memcpy(&laps, &buf[OFF_LAPS_DONE], 4);
		c.laps = (laps >= 0 && laps < 10000) ? laps : 0;
		float d = F(OFF_LAP_DIST);
		c.dist = (d == d && d >= 0 && d < 100000) ? d : 0;
		float vx = F(OFF_VEL), vy = F(OFF_VEL + 4);
		float sp = sqrtf(vx * vx + vy * vy);
		c.speed = (sp == sp && sp < 200) ? sp : 0;
		if (sp > 1 && sp == sp) c.head = atan2f(vy, vx);
		c.pit = pitRoad[i] != 0;
		c.out = buf[OFF_REASON_OUT] > 0;
		{
			float z[4] = { F(OFF_DAMAGE + 16), F(OFF_DAMAGE + 20), F(OFF_DAMAGE + 48), F(OFF_DAMAGE + 52) }, tot = 0;
			bool ok = true;
			for (float v : z) { if (!(v >= 0 && v < 100)) ok = false; tot += v; }
			c.dmgStep = 0;
			if (!ok) c.dmgTotal = -1;
			else {
				if (c.dmgTotal >= 0 && tot > c.dmgTotal + 0.0005f && !c.pit) {   // (pit road: repairs change it)
					c.dmgStep = tot - c.dmgTotal; g_dmgSeen = true;
					for (int k = 0; k < 4; k++) c.dmgStepZ[k] = std::max(0.0f, z[k] - c.dmg[k]);
				}
				for (int k = 0; k < 4; k++) c.dmg[k] = z[k];
				c.dmgTotal = tot;            // (lower than before = repaired: just the new starting point)
			}
		}
		c.gear = buf[OFF_GEAR] <= 10 ? (int)buf[OFF_GEAR] - 1 : -9;   // -1 reverse, 0 neutral, 1.. forward
		// the lap length the game's lap distance really uses: at the line it drops from about the lap length
		// to near 0; last reading + distance driven since - new reading = one lap. The median of these
		// replaces track.ini's figure, which can be off by a few car lengths
		double tnow = Now();
		if (c.prevDist > 0 && g_trackLen > 400 && c.prevDist > g_trackLen * 0.75f && c.dist < g_trackLen * 0.25f &&
		    tnow - c.prevT < 0.5 && c.speed > 20) {
			float lap = c.prevDist + c.speed * (float)(tnow - c.prevT) - c.dist;
			if (lap > g_trackLen * 0.9f && lap < g_trackLen * 1.1f) {
				g_lapLens.push_back(lap);
				if (g_lapLens.size() > 41) g_lapLens.erase(g_lapLens.begin());
			}
		}
		c.prevDist = c.dist; c.prevT = tnow;
		if (g_lapLens.size() >= 5) {
			std::vector<float> l = g_lapLens;
			std::nth_element(l.begin(), l.begin() + l.size() / 2, l.end());
			g_trackLen = l[l.size() / 2];
		} else if (c.dist > g_trackLen) g_trackLen = c.dist;     // a lap distance longer than track.ini says wins
		if (!c.out) speeds.push_back(c.speed);
	}
	if (speeds.empty()) { g_order.clear(); return; }
	std::sort(speeds.begin(), speeds.end());
	g_fieldSpeed = speeds[speeds.size() / 2];
	g_simT = SimClock(g);
	double now = Now();
	float L = std::max(g_trackLen, 400.0f);

	// race order
	g_order.clear();
	g_leaderLaps = 0;
	for (int i = 0; i < NUM_SLOTS; i++) {
		Car &c = g_car[i];
		if (!c.valid || c.out) { c.pos = 0; continue; }
		// the lap counter and the lap distance roll over on different frames at the line
		// (seen by AIMOD): a one-lap jump against the last reading is that, not real
		double p = (double)c.laps * L + c.dist;
		if (c.progress > 0 && fabs(p - c.progress) > L * 0.5) p += L * floor((c.progress - p) / L + 0.5);
		c.progress = p;
		g_order.push_back(i);
		g_leaderLaps = std::max(g_leaderLaps, c.laps);
	}
	std::sort(g_order.begin(), g_order.end(), [](int a, int b) { return g_car[a].progress > g_car[b].progress; });
	for (size_t p = 0; p < g_order.size(); p++) g_car[g_order[p]].pos = (int)p + 1;

	// official order: laps completed, then who crossed the line first (changes only at the line)
	unsigned hdr[4] = {};
	bool table = g.Read(SCORING_HDR, hdr, 16) && hdr[1] > 0 && hdr[1] <= 4096 && hdr[2] > 0 && hdr[2] <= 14 && hdr[3];
	static long seq = 0;
	bool exact = true;
	for (int i : g_order) {                    // live order: first sight mid-race = further round crossed first
		Car &c = g_car[i];
		c.cross = 0;
		if (table && c.laps >= 1 && (unsigned)c.laps < hdr[1]) {
			unsigned e = 0;
			if (g.Read(hdr[3] + ((DWORD)i << (hdr[2] + 2)) + c.laps * 4, &e, 4)) c.cross = e & CROSS_MASK;
		}
		if (c.laps > 0 && !c.cross) exact = false;
		if (c.scoredLaps != c.laps) { c.scoredLaps = c.laps; c.seq = ++seq; }
	}
	// qualifying (practice): timed laps and their distance/time trace, on the game's session clock
	// (the crossing times are on the same clock, measured live 2026-10-20: within a few ms)
	if (g_session == SESSION_PRACTICE) {
		int nowMs = (int)(GameSec(g) * 1000);
		for (int i : g_order) {
			Car &c = g_car[i];
			unsigned s = c.aiState;
			// An in-lap can finish the timed lap already under way, but it must never start
			// another timed lap or keep a live delta running on pit road.
			bool onTrack = s == AI_PRACTICE;
			if (c.cross && c.cross != c.qLastCross) {                 // crossed the line
				if (c.qLapStart >= 0) {
					int lap = (int)c.cross - c.qLapStart;
					if (lap > 5000 && lap < 400000) {
						c.qLastLapMs = lap; c.qLastLapAt = Now();
						if (!c.qBestMs || lap < c.qBestMs) { c.qBestMs = lap; c.qBestTrace = c.qTrace; }
					}
				}
				c.qLapStart = onTrack ? (int)c.cross : -1;
				c.qTrace.clear();
				c.qLastCross = c.cross;
			}
			if (!onTrack) { c.qLapStart = -1; c.qTrace.clear(); continue; }
			if (c.qLapStart >= 0 && c.qTrace.size() < 6000 && (c.qTrace.empty() || c.dist > c.qTrace.back().first))
				c.qTrace.push_back({ c.dist, nowMs - c.qLapStart });
		}
	}
	g_score = g_order;
	std::stable_sort(g_score.begin(), g_score.end(), [exact](int a, int b) {
		const Car &x = g_car[a], &y = g_car[b];
		if (x.laps != y.laps) return x.laps > y.laps;
		if (x.laps == 0) return false;                       // before the start: keep the live order
		return exact ? x.cross < y.cross : x.seq < y.seq;
	});
	for (size_t p = 0; p < g_score.size(); p++) g_car[g_score[p]].spos = (int)p + 1;
	RaceStory();

	for (int i : g_order) {
		Car &c = g_car[i];
		// crash / spin: a big loss of speed within the window, off pit road, field at speed
		if (c.hist.empty() || g_simT > c.hist.back().t) c.hist.push_back({ g_simT, c.speed });
		while (c.hist.size() > 2 && g_simT - c.hist.front().t > S.crashWindow) c.hist.erase(c.hist.begin());
		float top = 0;
		for (auto &s : c.hist) top = std::max(top, s.v);
		if (c.trace.empty() || g_simT > c.trace.back().t) c.trace.push_back({ g_simT, c.speed, c.dist, c.head, c.dmgTotal });
		while (c.trace.size() > 2 && g_simT - c.trace.front().t > 30) c.trace.erase(c.trace.begin());
		if (!c.pit && g_fieldSpeed > 25 && top - c.speed > S.crashDrop && now - c.incidentAt > S.incidentHold &&
		    !Checkup(i, top - c.speed)) {
			c.incidentAt = now; c.incidentWhy = "CRASH";
			CrashNote(i, "CRASH");
		}
		// pit road phases: entering -> in the stall (stopped) -> leaving -> back on track
		int phase = !c.pit ? 0 : c.speed < 0.5f ? 2 : (c.pitPhase >= 2 ? 3 : 1);
		if (phase != c.pitPhase) {
			if (phase == 0 && c.pitPhase == 3) c.pitExitAt = now;
			if (g_session == SESSION_RACE) {
				if (phase == 1 && c.pitPhase == 0) { c.pitInPos = c.spos; c.pitLap = c.laps + 1; c.pitStall = 0; c.pitOutPos = 0; c.pitStallAt = -1; }
				if (phase == 2 && c.pitStallAt < 0) { c.pitStallAt = g_simT; c.pits++; }
				if (c.pitPhase == 2 && c.pitStallAt >= 0) c.pitStall = g_simT - c.pitStallAt;
				if (phase == 0 && c.pitPhase == 3) c.pitOutPending = true;
			}
			c.pitPhase = phase; c.pitPhaseAt = now;
		}
		// stopped on track while the field is moving
		if (!c.pit && c.speed < 3 && g_fieldSpeed > 20) {
			if (c.stoppedSince < 0) c.stoppedSince = g_simT;
			else if (g_simT - c.stoppedSince > 2 && now - c.incidentAt > S.incidentHold) {
				// under caution a car stopped with no hit and no damage is stacked up behind the pace car or the cars
				// in front (live 2026-10-27: 10 cars logged as a crash), not crashed
				bool hurt = g_flag == FLAG_GREEN || !g_dmgSeen || DmgGain(i, g_simT - 15) > 0.02f || Kink(i, g_simT - 15, g_simT) >= 0;
				c.incidentAt = now;
				if (hurt) { c.incidentWhy = "STOPPED"; CrashNote(i, "STOPPED"); }
			}
		} else c.stoppedSince = -1;

	}
	CrashSweep();
	ContactWatch();
	// official neighbours: gap history (closing / pulling away), and passes made on track
	// before the line (the car behind gets ahead on track and stays there 2 s)
	for (size_t p = 1; p < g_score.size(); p++) {
		int a = g_score[p - 1], b = g_score[p];
		Car &cb = g_car[b];
		double d = Ahead(a, b);
		auto &h = g_gapHist[b];
		if (cb.pairWith != a) { h.clear(); cb.pairWith = a; cb.passSince = -1; cb.passed = false; }
		h.push_back({ now, (float)(fabs(d) / std::max(cb.speed, 10.0f)) });
		while (h.size() > 2 && now - h.front().t > 6) h.erase(h.begin());
		bool racing = g_flag == FLAG_GREEN && !g_car[a].pit && !cb.pit && cb.speed > 20 && fabs(d) < 60 && SameLap(a, b);
		if (racing && d < -2) {
			if (cb.passSince < 0) cb.passSince = now;
			else if (now - cb.passSince > 2 && !cb.passed) { cb.passed = true; g_passCar = b; g_passVictim = a; g_passAt = now; }
		} else if (d > 0) { cb.passSince = -1; cb.passed = false; }
	}
	HighlightWatch();
}

// change of gap over the last few seconds, seconds per second (negative = closing)
static double GapTrend(int slot) {
	auto it = g_gapHist.find(slot);
	if (it == g_gapHist.end() || it->second.size() < 2) return 0;
	auto &h = it->second;
	double dt = h.back().t - h.front().t;
	return dt > 1 ? (h.back().v - h.front().v) / dt : 0;
}

// ---------------------------------------------------------------- director
enum Kind { kTour, kLeader, kPit, kBattle, kPass, kRestart, kIncident, kManual };
static const char *KindName[] = { "RUNNING", "LEADER", "PIT", "BATTLE", "PASS", "RESTART", "INCIDENT", "MANUAL" };
static const char *KindKey[] = { "tour", "leader", "pit", "battle", "pass", "restart", "incident", "" };

struct Cand { int car = -1; Kind kind = kTour; double score = 0; int other = -1; std::string why; };
static std::vector<Cand> g_cands;           // the director's last look at the race (the phone's Story list)
struct Shot { int car = -1; Kind kind = kTour; int other = -1; double since = 0, camAt = 0; bool following = false; int phase = 0; std::string why; };
static Shot g_shot;
// dynamic cameras (racevision_dyncams.cpp): the director asks for one when it cuts; 0 / the camera it passed in = none
static int DynPick(Game &g, int kind, int car, int other, int phase, int cam);
static bool DynPitDecision(int &car, std::string &why);
static void DynAbort(Game &g);
// notes for the director view's timeline (racevision_timeline.cpp): scores at a cut, why the camera changed, TvHold's wait
static double g_tlBest = -1, g_tlCur = -1;
static std::string g_tlCamWhy, g_tlNote;
static double g_tlNoteAt = -100;
static void TlNote(const std::string &s) { g_tlNote = s; g_tlNoteAt = Now(); }
static int g_lastCam = -1;
static int g_mount = -1;                    // Rear (11): the car the camera rides on, when that isn't g_shot.car
static int g_mountWas = -1;                 // ...the one before (the game shows a new focus a frame or so late)
static double g_mountAt = -100;             // ...when it last changed
static void MountIs(int m) { if (m != g_mount) { g_mountWas = g_mount; g_mount = m; g_mountAt = Now(); } }
static std::map<std::string, size_t> g_camNext;

static double PosWeight(int pos) {
	if (pos <= 0) return 0.2;
	double late = (g_lapsTotal > 0 && g_lapsTotal - g_leaderLaps <= 10) ? 5.0 : 8.0;   // late race: the front counts more
	return 0.25 + 0.75 * exp(-(pos - 1) / late);
}

// right after the start or a restart the field is bunched up and everything looks like
// a battle: settle_laps laps of calmer directing
static bool Settling() {
	return g_flag == FLAG_GREEN && g_greenLap >= 0 && g_leaderLaps - g_greenLap < S.settleLaps;
}

// a pack race at the front (superspeedway draft): lead_battle_pack cars within 1.5 s of the leader
static bool PackAtFront() {
	if (g_score.size() < 2) return false;
	int leader = g_score[0], n = 1;
	for (size_t p = 1; p < g_score.size() && n < S.leadBattlePack; p++) {
		int c = g_score[p];
		if (!SameLap(leader, c) || g_car[c].pit) continue;
		if (fabs(Gap(leader, c)) < 1.5) n++;
	}
	return n >= S.leadBattlePack;
}
// a battle between these two for lead_battle_depth or better, away from a pack
static bool LeadBattle(int a, int b) {
	if (S.leadBattleDepth <= 0 || a < 0 || b < 0) return false;
	int front = std::min(g_car[a].spos, g_car[b].spos);
	return front > 0 && front <= S.leadBattleDepth && !PackAtFront();
}

static std::vector<Cand> Candidates() {
	std::vector<Cand> out;
	double now = Now();
	char why[160];
	if (g_score.empty() || g_session != SESSION_RACE) return out;
	auto novelty = [&](int car) {          // cars we just left score lower for a while
		double since = now - g_car[car].lastShown;
		return (car != g_shot.car && since < S.revisitAfter) ? 0.5 : 1.0;
	};
	bool lastLap = g_lapsTotal > 0 && g_leaderLaps >= g_lapsTotal - 1;
	int leader = g_score[0];
	bool settle = Settling();

	// crashes / stopped cars
	for (int i : g_order) {
		Car &c = g_car[i];
		if (c.incidentAt > 0 && now - c.incidentAt < S.incidentHold) {
			Cand k; k.car = i; k.kind = kIncident;
			k.score = 100 * (0.6 + 0.4 * PosWeight(c.spos)) * (1 - 0.4 * (now - c.incidentAt) / S.incidentHold);
			snprintf(why, sizeof why, "%s, %.0f mph", c.incidentWhy, c.speed * 2.237f); k.why = why;
			out.push_back(k);
		}
	}
	// restart: the leader and the front rows
	if (now < g_restartUntil) {
		Cand k; k.car = leader; k.kind = kRestart; k.score = 90; k.why = "green flag";
		out.push_back(k);
	}
	// last lap: stay with the leader
	if (lastLap) {
		Cand k; k.car = leader; k.kind = kLeader; k.score = 95; k.why = "final lap";
		out.push_back(k);
	}
	// a pass on track, just now
	if (g_passCar >= 0 && now - g_passAt < 3.0 && Ahead(g_passCar, g_passVictim) > 0 &&
	    (!settle || std::min(g_car[g_passCar].spos, g_car[g_passVictim].spos) <= S.settlePositions)) {
		Cand k; k.car = g_passCar; k.kind = kPass; k.other = g_passVictim;
		k.score = 65 * PosWeight(std::min(g_car[g_passCar].spos, g_car[g_passVictim].spos)) * novelty(g_passCar);
		k.why = "gets by " + Short(g_passVictim);
		out.push_back(k);
	}
	// battles (green flag, same lap): the car behind, scored by gap, trend, position,
	// freshness; a battle has to last battle_min_age seconds before it is worth a cut
	if (g_flag == FLAG_GREEN) {
		for (size_t p = 1; p < g_score.size(); p++) {
			int a = g_score[p - 1], b = g_score[p];
			Car &cb = g_car[b];
			if (g_car[a].pit || cb.pit || g_car[a].speed < 20 || cb.speed < 20 || !SameLap(a, b)) { cb.battleSince = -1; continue; }
			double d = Ahead(a, b), gap = fabs(d) / std::max(cb.speed, 10.0f);
			// short tracks: only at the bumper or alongside; elsewhere a time gap
			bool shortTrack = g_trackLen > 0 && g_trackLen < S.shortTrackM;
			double close = shortTrack ? fabs(d) / S.battleShortM : gap / S.battleGap;   // 0 = touching, 1 = limit
			if (close > 1) { cb.battleSince = -1; continue; }
			int lead = d >= 0 ? a : b, chase = d >= 0 ? b : a;      // as they are on track right now
			if (cb.battleWith != a || cb.battleSince < 0) { cb.battleWith = a; cb.battleSince = now; }
			double age = now - cb.battleSince;
			if (age < S.battleMinAge && chase != g_shot.car && lead != g_shot.car) continue;
			double fresh = age < 60 ? 1.0 : std::max(0.4, 1 - (age - 60) / 90);
			if (age < S.leadBattleHold && LeadBattle(a, b)) fresh = 1.0;   // a lead battle doesn't go stale
			double closing = std::min(0.3, std::max(-0.3, -GapTrend(b) * 2));   // closing in = more interesting
			int front = (int)p;                                       // fighting for this position
			if (settle && front > S.settlePositions) continue;      // bunched up: only the front matters
			Cand k; k.car = chase; k.kind = kBattle; k.other = lead;
			k.score = 50 * (1.2 - close * 0.6 + closing) * PosWeight(front) * fresh * novelty(chase)
			          * (settle ? 0.6 : 1.0);
			snprintf(why, sizeof why, "%.2fs %s %s", gap, closing > 0.05 ? "closing on" : "behind", Short(lead).c_str());
			k.why = why;
			out.push_back(k);
		}
	}
	// pit road. Under caution the stops are the story: follow the leaders in, through the
	// stall and back out, then whoever wins the race off pit road. Under green only the
	// front is worth it. A stop being shown is kept to the end (see Direct).
	bool caution = g_flag != FLAG_GREEN;
	int firstOff = -1;
	for (int i : g_score) {                    // best placed car that just rejoined from a caution stop
		Car &c = g_car[i];
		if (caution && c.pitExitAt > 0 && now - c.pitExitAt < 6) { firstOff = i; break; }
	}
	for (int i : g_order) {
		Car &c = g_car[i];
		if (c.pitPhase == 0) continue;
		static const char *phaseWhy[] = { "", "entering pit road", "in the pit stall", "leaving pit road" };
		double phaseBase[] = { 0, 30, 40, 36 };
		Cand k; k.car = i; k.kind = kPit;
		k.score = phaseBase[c.pitPhase] * (caution ? 1.0 : 0.55) * PosWeight(c.spos) * novelty(i);
		if (caution && c.spos == 1) k.score *= 1.4;
		k.why = phaseWhy[c.pitPhase];
		out.push_back(k);
	}
	if (firstOff >= 0) {
		Cand k; k.car = firstOff; k.kind = kPit; k.score = 45 * PosWeight(g_car[firstOff].spos);
		k.why = "first off pit road";
		out.push_back(k);
	}
	{   // the pit decision (dynamic cameras): under caution the leader comes up to pit entry - who dives in, who stays out
		int dc = -1; std::string dw;
		if (DynPitDecision(dc, dw)) { Cand k; k.car = dc; k.kind = kPit; k.score = 55 * PosWeight(g_car[dc].spos); k.why = dw; out.push_back(k); }
	}
	// the leader, and a tour through the top runners when nothing else is on
	{
		Cand k; k.car = leader; k.kind = kLeader;
		double lead = g_score.size() > 1 ? fabs(Gap(leader, g_score[1])) : 0;
		k.score = (g_lapsTotal > 0 && g_lapsTotal - g_leaderLaps <= 10 ? 28 : 18) * novelty(leader);
		snprintf(why, sizeof why, "leads by %.1fs", lead); k.why = why;
		out.push_back(k);
		for (size_t p = 1; p < g_score.size() && p < (size_t)S.tourDepth; p++) {
			int i = g_score[p];
			Cand t; t.car = i; t.kind = kTour; t.score = 14 * PosWeight(g_car[i].spos) * novelty(i);
			t.why = "";
			out.push_back(t);
		}
	}
	return out;
}

// pit shots pick the camera by where the car is on pit road
static std::string PitCamKey(Kind kind, int phase) {
	if (kind != kPit) return KindKey[kind];
	return phase == 2 ? "pit_stall" : phase == 3 || phase == 0 ? "pit_exit" : "pit";
}

static int PeekCam(const std::string &key) {      // the camera NextCam would give, without moving on
	auto it = S.cams.find(key);
	if (it == S.cams.end() || it->second.empty()) return -1;
	return it->second[g_camNext[key] % it->second.size()];
}

static int NextCam(const std::string &key) {
	auto it = S.cams.find(key);
	if (it == S.cams.end() || it->second.empty()) return -1;
	size_t &n = g_camNext[key];
	return it->second[n++ % it->second.size()];
}

// Rear (11) can't show the car it rides on (live 2026-10-27: it hunted for a car behind to look at). For the TV car it
// rides on the car just ahead instead and turns back to the TV car (RearPan); g_shot.car stays the TV car, so the
// overlays and the director keep talking about it. The car ahead is kept while the TV car is behind or alongside it.
// Rear (11) on the car ahead, with fixed rules (live 2026-10-27: "floppy" - it changed car all the time in the pack):
//  - it picks the nearest car 4..rear_mount_m ahead of the TV car that sees it: no other car on the line between them
//    (world positions; a car closer than 1.6 m to that line is in the way), cars in the TV car's lane first;
//  - it stays on that car while the TV car is up to rear_hold_m behind it or alongside (the camera turns after it as it
//    pulls up beside, and follows it back if it fades), and lets go only when the TV car is rear_pass_m past its nose
//    (it passed), the car pits or drops out, or another car has been in between for rear_block_s;
//  - then the next car ahead that sees the TV car, or the TV car's own rear camera when there is none.
static bool CarXY(Game &g, int i, float &x, float &y) {
	const int POS = 6622;                        // car block: world position x, y (m) (OFF_POS in racevision_cams.cpp)
	return g.Read(BLOCK0 + i * BLOCK_SIZE + POS, &x, 4) && g.Read(BLOCK0 + i * BLOCK_SIZE + POS + 4, &y, 4);
}
static bool MountHolds(int m, int car) {         // by the rules above, leaving out "in the way"
	if (m < 0 || m == car || !g_car[m].valid || g_car[m].pit) return false;
	double a = Ahead(m, car);
	return a > -S.rearPassM && a < S.rearHoldM;
}
static bool InTheWay(Game &g, int m, int car) {  // another car on the line from the TV car to the one it's seen from
	float ax, ay, bx, by;
	if (!CarXY(g, car, ax, ay) || !CarXY(g, m, bx, by)) return false;
	double gap = Ahead(m, car);
	float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
	if (len2 < 1) return false;
	for (int j : g_order) {
		if (j == m || j == car || !g_car[j].valid || g_car[j].pit) continue;
		double aj = Ahead(j, car);
		if (aj < 1 || aj > gap - 1) continue;    // not between them
		float x, y;
		if (!CarXY(g, j, x, y)) continue;
		float t = ((x - ax) * dx + (y - ay) * dy) / len2;
		if (t <= 0.05f || t >= 0.95f) continue;
		float px = ax + t * dx - x, py = ay + t * dy - y;
		if (px * px + py * py < 1.6f * 1.6f) return true;
	}
	return false;
}
static int RearPick(Game &g, int car, int skip = -1) {   // a new car to ride on; -1 = none
	if (car < 0 || !g_car[car].valid || g_car[car].pit) return -1;
	float cx, cy, h = g_car[car].head;
	bool world = CarXY(g, car, cx, cy);
	int best = -1, other = g_shot.car == car ? g_shot.other : -1;
	double bestScore = 1e9;
	for (int j : g_order) {
		if (j == car || j == skip || !g_car[j].valid || g_car[j].pit || g_car[j].speed < 10) continue;
		double a = Ahead(j, car);
		if (a < 4 || a > S.rearMountM) continue;
		if (InTheWay(g, j, car)) continue;
		double score = a;
		float x, y;
		if (world && CarXY(g, j, x, y)) score += 2 * fabs(-(x - cx) * sinf(h) + (y - cy) * cosf(h));   // off its lane
		if (j == other) score -= 10;             // a battle: the car it's chasing
		if (score < bestScore) { bestScore = score; best = j; }
	}
	return best;
}
static int RearMountFor(Game &g, int car) {      // (the opening pick: the car it rides on now when that still holds)
	if (MountHolds(g_mount, car) && !InTheWay(g, g_mount, car)) return g_mount;
	return RearPick(g, car);
}
// camera c (-1 = keep the camera) on the TV car `car` (-1 = keep the car); c comes back as the camera used
static bool SetShotCam(Game &g, int &c, int car) {
	int now = c >= 0 ? c : g.Camera();
	if (S.rearMount && GameCam(now) == 11 && car >= 0) {
		int m = RearMountFor(g, car);
		if (m < 0) { if (S.rearFallback != 11) c = S.rearFallback; m = car; }   // (11: its own rear cam, the car behind)
		bool ok = g.Set(c, m);
		if (ok) MountIs(m != car ? m : -1);
		return ok;
	}
	if (car < 0 && g_mount >= 0) car = g_shot.car;   // off the rear camera: back onto the TV car itself
	bool ok = g.Set(c, car);
	if (ok && (car >= 0 || c >= 0)) MountIs(-1);
	return ok;
}
static void SetCam(Game &g, int c) {
	if (c < 0) return;
	if ((S.rearMount && GameCam(c) == 11) || g_mount >= 0) { if (SetShotCam(g, c, g_shot.car)) g_lastCam = c; return; }
	if (c != g.Camera() && g.SetCamera(c)) g_lastCam = c;
}

static void Cut(Game &g, const Cand &k) {
	double now = Now();
	if (g_shot.car >= 0) g_car[g_shot.car].lastShown = now;
	int cam = NextCam(PitCamKey(k.kind, g_car[k.car].pitPhase));
	g_shot.car = k.car; g_shot.other = k.other;     // (RearMountFor looks at the battle partner)
	cam = DynPick(g, k.kind, k.car, k.other, g_car[k.car].pitPhase, cam);       // a dynamic camera, or the game's
	bool ok = SetShotCam(g, cam, k.car);
	if (!ok) DynAbort(g);
	if (ok && cam >= 0) g_lastCam = cam;
	SYSTEMTIME st; GetLocalTime(&st);
	Log("[%02d:%02d:%02d] %-8s P%-2d %-28s %s%s", st.wHour, st.wMinute, st.wSecond, KindName[k.kind], g_car[k.car].spos,
	       Name(k.car).c_str(), k.why.c_str(), ok ? "" : "  (focus write FAILED)");
	if (cam >= 0) Log("  cam %d", cam);
	Log("\n");
	g_shot.car = k.car; g_shot.kind = k.kind; g_shot.other = k.other; g_shot.why = k.why;
	g_shot.since = g_shot.camAt = now; g_shot.following = false; g_shot.phase = g_car[k.car].pitPhase;
}

static bool IsOnboard(int cam);

// ---------------------------------------------------------------- single-car qualifying
// The Single-Car Qualifying tool (later Race Control) runs qualifying inside PRACTICE: every AI
// car is held in its pit box (stall countdown set huge) and one car at a time rolls off, runs its
// out lap + timed lap(s) and comes back in; the next car rolls off when that one reaches pit road.
// We recognise it from the held cars and broadcast each run from the box to pit road.
static bool g_qual = false;
static int g_qualCar = -1, g_qualPhase = 0;      // 1 rolling off pit road, 2 on track (incl. in-lap)
static double g_qualPhaseAt = 0;

static int QualCam(const char *key, const char *fallback) {
	int c = NextCam(key);
	return c >= 0 ? c : NextCam(fallback);
}

// ---- knockout group qualifying (many cars on track at once) ----
// The qualifying tool writes %PROGRAMDATA%\NR2003 Companions\qualifying_state.txt every ~0.1 s:
// mode, phase (waiting / running / checkered / break / done), round, cutoff (cars that advance,
// 0 in the final round), on_track slots, board = slot:best_ms in order.
// Single car runs also name `focus` (the car Race Control's timing box is on: a result during its in-lap, the car on
// a timed lap, or the next car on its way to the green) and `focus_kind` (result / live / waiting / none).
// Both modes name `drive`: the player's car while the player drives a run (from the GO until stopped back in the box),
// else -1. That car goes in its cockpit ahead of everything else (PlayerDrive).
struct KoState { bool on = false; std::string mode, phase, focusKind; int round = 0, rounds = 0, cutoff = 0, focus = -1, drive = -1; double timeLeft = 0; std::vector<int> order; std::vector<std::pair<int, int>> board; };
static KoState g_ko;
static std::map<int, int> g_koBest;              // best time per car we last saw (to spot improvements)
static int g_koRound = -1;

static void ReadKo() {
	static double last = -10;
	double now = Now();
	if (now - last < 1.0) return;
	last = now;
	// a read that fails (the file being swapped, or locked) keeps the last good state for up to 3 s: dropping it
	// at once made the single-car mode fall back to its own pick, a car leaving its pit box
	static double lastGood = -10;
	if (now - lastGood > 3) g_ko.on = false;
	char base[MAX_PATH];
	DWORD n = GetEnvironmentVariableA("PROGRAMDATA", base, MAX_PATH);
	if (!n || n >= MAX_PATH) return;
	std::string path = std::string(base) + "\\NR2003 Companions\\qualifying_state.txt";
	WIN32_FILE_ATTRIBUTE_DATA fa;
	if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) return;
	FILETIME ft; GetSystemTimeAsFileTime(&ft);
	ULONGLONG wr = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
	ULONGLONG nw = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	if (nw > wr && nw - wr > 50000000ULL) { g_ko.on = false; return; }   // not updated for 5 s: the tool isn't running
	FILE *f = fopen(path.c_str(), "r");
	if (!f) return;
	KoState k;
	char line[4096];
	while (fgets(line, sizeof line, f)) {
		std::string l = Trim(line);
		size_t sp = l.find(' ');
		std::string key = l.substr(0, sp), val = sp == std::string::npos ? "" : l.substr(sp + 1);
		if (key == "mode") { k.mode = val; k.on = val == "knockout" || val == "single"; }
		else if (key == "rounds") k.rounds = atoi(val.c_str());
		else if (key == "time_left") k.timeLeft = atof(val.c_str());
		else if (key == "phase") k.phase = val;
		else if (key == "round") k.round = atoi(val.c_str());
		else if (key == "cutoff") k.cutoff = atoi(val.c_str());
		else if (key == "focus") k.focus = atoi(val.c_str());
		else if (key == "focus_kind") k.focusKind = val;
		else if (key == "drive") k.drive = atoi(val.c_str());
		else if (key == "order") {
			const char *s = val.c_str();
			while (*s) {
				while (*s == ' ') s++;
				if (!*s) break;
				int slot, used = 0;
				if (sscanf(s, "%d%n", &slot, &used) != 1 || used <= 0) break;
				if (slot >= 0 && slot < NUM_SLOTS) k.order.push_back(slot);
				s += used;
			}
		}
		else if (key == "board") {
			const char *s = val.c_str();
			while (*s) {
				int slot, ms, used = 0;
				if (sscanf(s, "%d:%d%n", &slot, &ms, &used) != 2) break;
				if (slot >= 0 && slot < NUM_SLOTS) k.board.push_back({ slot, ms });
				s += used; while (*s == ' ') s++;
			}
		}
	}
	fclose(f);
	if (k.mode.empty()) return;                      // caught mid-write: keep the last good state
	g_ko = k;
	lastGood = now;
}

static bool KoDirect(Game &g) {
	ReadKo();
	if (!g_ko.on || g_ko.mode != "knockout" || g_session != SESSION_PRACTICE ||
	    (g_ko.phase != "running" && g_ko.phase != "checkered")) return false;
	double now = Now(), age = now - g_shot.since;
	if (g_ko.round != g_koRound) {
		g_koRound = g_ko.round; g_koBest.clear();
		Log("Knockout qualifying round %d: following the cars on track.\n", g_ko.round);
	}
	auto onTrack = [](int c) {
		unsigned s = g_car[c].aiState;
		return g_car[c].valid && (s == AI_PRACTICE || s == AI_INLAP_S || s == AI_INLAP_F);
	};
	std::map<int, int> pos;
	for (size_t i = 0; i < g_ko.board.size(); i++) pos[g_ko.board[i].first] = (int)i + 1;
	// a car that just went top 3, or above the cut line, goes on screen now
	int hot = -1; char why[96] = "";
	for (auto &e : g_ko.board) {
		auto it = g_koBest.find(e.first);
		bool better = it == g_koBest.end() || e.second < it->second;
		if (better && onTrack(e.first)) {
			int p = pos[e.first];
			if (p <= 3 || (g_ko.cutoff > 0 && p <= g_ko.cutoff)) {
				if (hot < 0 || p < pos[hot]) {
					hot = e.first;
					snprintf(why, sizeof why, "P%d %.3f%s", p, e.second / 1000.0,
					         g_ko.cutoff > 0 && p <= g_ko.cutoff && p > 3 ? " - inside the cut" : "");
				}
			}
		}
	}
	bool first = g_koBest.empty();
	for (auto &e : g_ko.board) g_koBest[e.first] = e.second;
	if (first) hot = -1;                                    // don't treat the first read as news
	if (g_shot.kind == kManual && age < S.manualHold) return true;
	int pick = -1;
	if (hot >= 0 && hot != g_shot.car && age > 2.0) pick = hot;
	else if (g_shot.car < 0 || !onTrack(g_shot.car) || age > S.minHold * 1.5) {
		// rotate: the bubble (around the cut), then the fastest car, then anyone on track not seen lately
		std::vector<int> pool;
		if (g_ko.cutoff > 0)
			for (auto &e : g_ko.board) { int p = pos[e.first]; if (p >= g_ko.cutoff - 1 && p <= g_ko.cutoff + 2 && onTrack(e.first)) pool.push_back(e.first); }
		if (!g_ko.board.empty() && onTrack(g_ko.board[0].first)) pool.push_back(g_ko.board[0].first);
		for (int i = 0; i < NUM_SLOTS; i++) if (onTrack(i)) pool.push_back(i);
		for (int c : pool)
			if (c != g_shot.car && now - g_car[c].lastShown > S.revisitAfter) { pick = c; break; }
		if (pick < 0) for (int c : pool) if (c != g_shot.car) { pick = c; break; }
		if (pick >= 0) {
			int p = pos.count(pick) ? pos[pick] : 0;
			if (p) snprintf(why, sizeof why, "P%d%s", p, g_ko.cutoff > 0 && p >= g_ko.cutoff - 1 && p <= g_ko.cutoff + 2 ? " - on the bubble" : "");
			else snprintf(why, sizeof why, "no time yet");
		}
	}
	if (pick >= 0) {
		if (g_shot.car >= 0) g_car[g_shot.car].lastShown = now;
		int cam = QualCam("qual", "tour");
		bool ok = g.Set(cam, pick);
		if (ok && cam >= 0) g_lastCam = cam;
		SYSTEMTIME st; GetLocalTime(&st);
		Log("[%02d:%02d:%02d] KNOCKOUT R%d %-26s %s  cam %d\n", st.wHour, st.wMinute, st.wSecond, g_ko.round,
		    Name(pick).c_str(), why, cam);
		g_shot.car = pick; g_shot.kind = kTour; g_shot.other = -1; g_shot.since = g_shot.camAt = now;
		g_shot.why = why; g_shot.following = false;
		return true;
	}
	// same car: opening camera, then follow cameras
	bool onCam = IsOnboard(g_lastCam);
	if (!g_shot.following && age > S.followAfter) {
		g_shot.following = true; g_shot.camAt = now;
		SetCam(g, QualCam("qual_follow", "tour_follow"));
	} else if (g_shot.following && S.cameraEvery > 0 &&
	           now - g_shot.camAt > (onCam ? std::max(S.cameraEvery, S.onboardHold) : S.cameraEvery)) {
		g_shot.camAt = now;
		SetCam(g, QualCam("qual_follow", "tour_follow"));
	}
	return true;
}

// true while the player drives their own qualifying run: the camera sits in their car (qual_player_cam, the cockpit)
// from Race Control's GO until they are stopped back in their box, whatever the timing box shows. You can still change
// the camera yourself; only the car is kept. Afterwards the runs pick up with whoever Race Control has on track now.
static int g_driveCar = -1;
static bool PlayerDrive(Game &g) {
	ReadKo();
	int d = g_ko.on && g_session == SESSION_PRACTICE && S.qualPlayerCam > 0 ? g_ko.drive : -1;
	// (your car is never `valid` - no AI grip figure for it - so slot 0 only needs a driver)
	if (d < 0 || d >= NUM_SLOTS || !(g_car[d].valid || (d == 0 && g_car[0].number >= 0 && !g_car[0].last.empty()))) d = -1;
	double now = Now();
	if (d < 0) {
		if (g_driveCar >= 0) {
			Log("           %s is back in the box - back to the qualifying runs\n", Name(g_driveCar).c_str());
			g_car[g_driveCar].lastShown = now;
			g_driveCar = -1; g_qualCar = -1; g_qualPhase = 0;
			g_shot.kind = kTour; g_shot.since = now - S.manualHold - S.minHold - 1;   // free to cut at once
		}
		return false;
	}
	if (d != g_driveCar) {
		static double failLogged = -100;
		bool ok = g.Set(S.qualPlayerCam, d);
		SYSTEMTIME st; GetLocalTime(&st);
		if (ok) {
			g_driveCar = d; g_lastCam = S.qualPlayerCam;
			Log("[%02d:%02d:%02d] QUAL     %-28s your run - in your car, cam %d\n", st.wHour, st.wMinute, st.wSecond, Name(d).c_str(), S.qualPlayerCam);
		} else if (now - failLogged > 10) {              // tried again on the next decision
			failLogged = now;
			Log("[%02d:%02d:%02d] QUAL     %-28s your run - camera write FAILED, retrying\n", st.wHour, st.wMinute, st.wSecond, Name(d).c_str());
		}
	} else if (g.Focus() != d)
		g.SetFocus(d);                                   // something moved the picture off your car: back to it
	g_shot.car = d; g_shot.kind = kManual; g_shot.other = -1; g_shot.since = now; g_shot.why = "your run";
	return true;
}

// true while single-car qualifying owns the broadcast
static bool QualDirect(Game &g) {
	if (KoDirect(g)) return true;                // knockout rounds: many cars on track
	auto leaving = [](unsigned s) { return s == AI_LEAVE_S || s == AI_LEAVE_F; };
	auto running = [](unsigned s) { return s == AI_PRACTICE || s == AI_INLAP_S || s == AI_INLAP_F; };
	auto done = [](unsigned s) { return s == AI_ENTER_S || s == AI_ENTER_F || s == AI_WAIT_S || s == AI_WAIT_F; };
	int held = 0;
	if (g_session == SESSION_PRACTICE)
		for (int i = 0; i < NUM_SLOTS; i++)
			if (g_car[i].valid && (g_car[i].aiState == AI_WAIT_S || g_car[i].aiState == AI_WAIT_F) &&
			    g_car[i].stallCount >= HELD_COUNT) held++;
	bool on = g_session == SESSION_PRACTICE && (held >= 3 || (g_qual && held >= 1));
	if (on != g_qual) {
		g_qual = on; g_qualCar = -1; g_qualPhase = 0;
		Log(on ? "Single-car qualifying session: following each run from the pit box to pit road.\n"
		       : "Single-car qualifying over.\n");
	}
	if (!on) return false;
	double now = Now();
	// Race Control names the car its timing box is on: the camera goes to the same car, so the box and the picture
	// change over together (the result through the in-lap, then the next car before it takes the green).
	bool boxLed = g_ko.on && g_ko.mode == "single" && g_ko.phase == "running" && g_ko.focus >= 0 && g_ko.focus < NUM_SLOTS &&
	              g_car[g_ko.focus].valid;
	if (boxLed && g_ko.focus != g_qualCar) {
		if (g_qualCar >= 0) Log("           handing off from %s to %s (timing box: %s)\n", Name(g_qualCar).c_str(), Name(g_ko.focus).c_str(), g_ko.focusKind.c_str());
		g_qualCar = g_ko.focus; g_qualPhase = 0;
	}
	// Stay with the current driver through every timed lap. Once its final lap
	// completes, hold the result briefly, then hand off when the next runner is
	// already near the stripe (rather than waiting for the previous car to pit).
	if (!boxLed && g_qualCar >= 0 && (g_car[g_qualCar].aiState == AI_INLAP_S || g_car[g_qualCar].aiState == AI_INLAP_F)) {
		Car &finished = g_car[g_qualCar];
		bool resultHeld = now - finished.qLastLapAt >= 4.0;
		int next = -1;
		for (int i = 0; resultHeld && i < NUM_SLOTS && next < 0; i++) {
			const Car &candidate = g_car[i];
			if (i == g_qualCar || !candidate.valid || candidate.aiState != AI_PRACTICE || candidate.runEnd <= candidate.lapNum + 5) continue;
			// Hand off with a configurable setup shot before the stripe - or at once if it is already past the
			// stripe on a timed lap (it crossed during the result hold). Track length unknown: after the hold.
			if (g_trackLen <= 400 || candidate.dist >= g_trackLen - (float)S.qualHandoffM || candidate.qLapStart >= 0) next = i;
		}
		if (next >= 0) {
			Log("           handing off from %s to %s near the start line\n", Name(g_qualCar).c_str(), Name(next).c_str());
			g_qualCar = next; g_qualPhase = 0;
		}
	}
	// the current run ends when the car reaches pit road (or is back in its box)
	if (g_qualCar >= 0 && (!g_car[g_qualCar].valid || (done(g_car[g_qualCar].aiState) && !(boxLed && g_ko.focus == g_qualCar)))) {
		Log("           %s is on pit road - run over\n", Name(g_qualCar).c_str());
		g_qualCar = -1; g_qualPhase = 0;
	}
	if (g_qualCar < 0) {
		// next runner: a car rolling off (held cars have the huge countdown, so anyone leaving
		// the box now was released by the tool); else a car out on a run (run-end lap far ahead)
		int pick = -1;
		for (int i = 0; i < NUM_SLOTS && pick < 0; i++)
			if (g_car[i].valid && leaving(g_car[i].aiState)) pick = i;
		for (int i = 0; i < NUM_SLOTS && pick < 0; i++)
			if (g_car[i].valid && running(g_car[i].aiState) && g_car[i].runEnd > g_car[i].lapNum + 5) pick = i;
		if (pick < 0) return true;                   // between runs: leave the picture alone
		g_qualCar = pick; g_qualPhase = 0;
	}
	Car &c = g_car[g_qualCar];
	int phase = leaving(c.aiState) ? 1 : 2;
	// a car you picked (or NR2003 put back on screen - it keeps the view on your car while you drive) holds the
	// picture for manual_hold, whoever it is: otherwise the director and the game fight over it every decision
	if (g_shot.kind == kManual && now - g_shot.since < S.manualHold) return true;
	if (g_shot.car != g_qualCar || phase != g_qualPhase) {
		int cam = phase == 1 ? QualCam("qual_exit", "pit_exit") : QualCam("qual", "tour");
		if (g_shot.car != g_qualCar) {
			if (g_shot.car >= 0) g_car[g_shot.car].lastShown = now;
			bool ok = g.Set(cam, g_qualCar);
			if (ok && cam >= 0) g_lastCam = cam;
			SYSTEMTIME st; GetLocalTime(&st);
			Log("[%02d:%02d:%02d] QUAL     %-28s %s  cam %d\n", st.wHour, st.wMinute, st.wSecond, Name(g_qualCar).c_str(),
			    phase == 1 ? "rolls off pit road" : "on a qualifying run", cam);
			g_shot.car = g_qualCar; g_shot.kind = kTour; g_shot.other = -1; g_shot.since = now;
			g_shot.why = "qualifying run"; g_shot.phase = 0;
		} else
			SetCam(g, cam);
		g_qualPhase = phase; g_qualPhaseAt = now; g_shot.camAt = now; g_shot.following = false;
		return true;
	}
	// on track: opening camera, then follow cameras changing every camera_every seconds
	if (phase == 2) {
		bool onCam = IsOnboard(g_lastCam);
		if (!g_shot.following && now - g_qualPhaseAt > S.followAfter) {
			g_shot.following = true; g_shot.camAt = now;
			SetCam(g, QualCam("qual_follow", "tour_follow"));
		} else if (g_shot.following && S.cameraEvery > 0 &&
		           now - g_shot.camAt > (onCam ? std::max(S.cameraEvery, S.onboardHold) : S.cameraEvery)) {
			g_shot.camAt = now;
			SetCam(g, QualCam("qual_follow", "tour_follow"));
		}
	}
	return true;
}

// You are driving: in your car (the game's in-car state) in a race, or out on track in practice. The director then
// never touches the view - live 2026-10-26 (North Wilkesboro): it waited its 60 s after you picked your own car, then
// cut to the leader half a lap into the race. Overlays stay off too.
static bool g_youDrive = false;
static bool YouDrive(Game &g) {
	bool in = g.h && g.Get<int>(DRIVER_STATE, -1) == 3 && (g_session == SESSION_RACE || !g_playerPit);
	if (in != g_youDrive) {
		g_youDrive = in;
		Log(in ? "           you're driving - the director leaves your view alone until you get out or pit\n"
		       : "           you're not driving now - the director carries on\n");
		if (!in) g_shot.since = Now() - S.manualHold - S.minHold - 1;   // free to cut at once
	}
	return in;
}

// each loop: while Rear (11) is on, keep it on the car just ahead of the TV car (another one when the TV car gets by,
// rear_fallback when nobody is close ahead); off it, the camera goes back onto the TV car itself
static void RearKeep(Game &g) {
	static double blockedSince = -1, ownSince = -1;
	if (g_youDrive || g_driveCar >= 0) { MountIs(-1); return; }
	int cam = g.Camera(), f = g.Focus(), s = g_shot.car;
	double now = Now();
	if (S.rearMount && cam == 11 && s >= 0) {
		int m = g_mount;
		const char *why = "";
		if (m >= 0) {                            // the rules for letting go
			bool blocked = InTheWay(g, m, s);
			if (!blocked) blockedSince = -1; else if (blockedSince < 0) blockedSince = now;
			double a = Ahead(m, s);
			if (!g_car[m].valid || g_car[m].pit) why = "it pitted";
			else if (a <= -S.rearPassM) why = "got by";
			else if (a >= S.rearHoldM) why = "dropped back";
			else if (blocked && now - blockedSince >= S.rearBlockS) why = "a car got in between";
			if (*why) { int was = m; m = RearPick(g, s, was); blockedSince = -1;
				Log("           rear camera: %s %s - %s\n", Name(s).c_str(), why, m >= 0 ? ("now from " + Name(m)).c_str() : "its own, looking behind"); }
		} else if (now - g_mountAt > 2) {        // on its own rear camera: a car ahead that sees it (checked every loop)
			m = RearPick(g, s);
			if (m >= 0) {                        // only once it has been that way a moment (no flicker at the edge)
				if (ownSince < 0) ownSince = now;
				if (now - ownSince < 1.0) m = -1;
			} else ownSince = -1;
		}
		if (m < 0) {
			ownSince = m < 0 && g_mount >= 0 ? -1 : ownSince;
			if (S.rearFallback == 11) {          // its own rear camera, looking at the car behind
				if (g_mount >= 0 || (f != s && now - g_mountAt > 1)) { if (g.Set(-1, s)) MountIs(-1); }
				return;
			}
			int c = S.rearFallback;
			if (SetShotCam(g, c, s)) { g_lastCam = c; Log("           rear camera: nobody close ahead of %s - camera %d\n", Name(s).c_str(), c); }
			return;
		}
		if (m != g_mount) {
			if (g.Set(-1, m)) { if (!*why) Log("           rear camera: %s seen from %s\n", Name(s).c_str(), Name(m).c_str()); MountIs(m); ownSince = -1; }
		} else if (f != m && f != g_mountWas && now - g_mountAt > 1) g.Set(-1, m);   // something moved it: back on
		return;
	}
	blockedSince = ownSince = -1;
	if (g_mount >= 0) {
		if (f == g_mount && s >= 0) g.Set(-1, s);
		MountIs(-1);
	}
}

// you picked a car yourself in the game (V / the car keys): it becomes the shot's car (the overlays show it)
static bool FollowPicked(Game &g, bool paused = false) {     // paused: directing is paused (Ctrl+F11 / phone)
	double now = Now();
	int f = g.Focus();
	if (f < 0 || f >= NUM_SLOTS || !g_car[f].valid || f == g_shot.car || f == g_mount ||
	    (f == g_mountWas && now - g_mountAt < 2)) return false;                     // (not the rear camera's last car)
	if (g_shot.car < 0 && !paused) return false;                                 // (the director's first pick)
	if (g_shot.car >= 0) g_car[g_shot.car].lastShown = now;
	g_shot.car = f; g_shot.kind = kManual; g_shot.other = -1; g_shot.since = now; g_shot.why = "";
	if (paused) Log("           you picked %s\n", Name(f).c_str());
	else Log("           you picked %s - director waits %.0fs\n", Name(f).c_str(), S.manualHold);
	return true;
}

static bool TvHold(Game &g, int cam, int car);   // racevision_trackcams.cpp: wait for a TV camera change on screen?
static void Direct(Game &g) {
	if (PlayerDrive(g)) return;                  // your own qualifying run: in your car until you're back in the box
	if (YouDrive(g)) return;                     // your own race / practice laps: hands off
	double now = Now(), age = now - g_shot.since;
	if (FollowPicked(g)) return;                 // you picked a car yourself: leave it a while
	if (QualDirect(g)) return;                   // single-car qualifying in practice
	std::vector<Cand> cands = Candidates();
	g_cands = cands;
	if (g_shot.kind == kManual && age < S.manualHold) return;
	if (cands.empty()) return;
	const Cand *best = &cands[0];
	double cur = 0;
	for (auto &k : cands) {
		if (k.score > best->score) best = &k;
		if (k.car == g_shot.car) cur = std::max(cur, k.score);
	}
	// a lead battle on screen: the pair's score counts (either car the chaser), and it isn't a "long shot" until
	// lead_battle_hold (live 2026-11-01: the director kept cutting away from battles for the lead)
	bool leadShot = (g_shot.kind == kBattle || g_shot.kind == kPass) && g_shot.other >= 0 && LeadBattle(g_shot.car, g_shot.other);
	if (leadShot)
		for (auto &k : cands)
			if ((k.kind == kBattle || k.kind == kPass) &&
			    ((k.car == g_shot.car && k.other == g_shot.other) || (k.car == g_shot.other && k.other == g_shot.car)))
				cur = std::max(cur, k.score);
	if (age > (leadShot ? std::max(S.maxHold, S.leadBattleHold) : S.maxHold)) cur *= 0.5;
	Car &sc = g_car[g_shot.car >= 0 ? g_shot.car : 0];
	bool pitStory = g_shot.kind == kPit && g_shot.car >= 0 && (sc.pitPhase != 0 || now - sc.pitExitAt < 3);
	if (pitStory && (g_flag != FLAG_GREEN || sc.spos <= 10)) cur = std::max(cur, 60.0);   // stay with the stop until it rejoins
	double minHold = Settling() ? std::max(S.minHold, S.settleHold) : S.minHold;
	bool onCam = IsOnboard(g_lastCam);
	if (onCam) minHold = std::max(minHold, S.onboardHold);
	// a pit stop being shown: change camera as the car moves along pit road
	if (pitStory && sc.pitPhase != g_shot.phase) {
		g_shot.phase = sc.pitPhase; g_shot.camAt = now; g_shot.following = true;
		g_tlCamWhy = sc.pitPhase == 1 ? "pit road: coming in" : sc.pitPhase == 2 ? "pit road: in the stall" : sc.pitPhase == 3 ? "pit road: leaving" : "pit road: back on track";
		SetCam(g, DynPick(g, kPit, g_shot.car, g_shot.other, sc.pitPhase, NextCam(PitCamKey(kPit, sc.pitPhase))));
	}

	// a battle being shown keeps its two cars: the same pair with the roles swapped (one got
	// alongside / ahead on track) is still this shot, not a reason to cut to the other car
	bool battleShot = (g_shot.kind == kBattle || g_shot.kind == kPass) && g_shot.other >= 0;
	if (battleShot && best->car == g_shot.other && best->other == g_shot.car && best->kind != kIncident) {
		for (auto &k : cands) if (k.car == g_shot.other && k.other == g_shot.car) cur = std::max(cur, k.score);
		best = nullptr;
		for (auto &k : cands) if (k.car == g_shot.car && (!best || k.score > best->score)) best = &k;
		if (!best) return;
	}
	if (best->car == g_shot.car) {
		// same car: keep the shot, maybe now for a better reason; vary the camera. The battle
		// partner stays the same for the whole shot (the focus car can be in two battles at once,
		// with the car ahead and the car behind); it changes only when that battle is over.
		const Cand *pair = nullptr;
		// the battle on screen is not over the moment the gap ticks past battle_gap: it lasts
		// until the gap is 1.5 times that (twice battle_short_m on short tracks)
		auto alive = [&]() {
			int o = g_shot.other, c = g_shot.car;
			if (o < 0 || !g_car[o].valid || g_car[o].pit || g_car[c].pit || !SameLap(o, c)) return false;
			double d = fabs(Ahead(o, c));
			bool st = g_trackLen > 0 && g_trackLen < S.shortTrackM;
			return st ? d < S.battleShortM * 2 : d / std::max(g_car[c].speed, 10.0f) < S.battleGap * 1.5;
		};
		if (battleShot)
			for (auto &k : cands)
				if ((k.kind == kBattle || k.kind == kPass) &&
				    ((k.car == g_shot.car && k.other == g_shot.other) || (k.car == g_shot.other && k.other == g_shot.car)))
					pair = &k;
		bool keep = battleShot && (pair || alive());
		int was = g_shot.other;
		if (best->kind > g_shot.kind && !(keep && (best->kind == kPass || best->kind == kBattle) && best->other != g_shot.other)) {
			g_shot.kind = best->kind; g_shot.other = best->other; g_shot.why = best->why;
		} else if (battleShot) {
			if (pair) g_shot.why = pair->why;
			else if (!keep && (best->kind == kBattle || best->kind == kPass)) {     // that battle is over: a new partner
				g_shot.kind = best->kind; g_shot.other = best->other; g_shot.why = best->why;
			}
		} else if (best->kind == g_shot.kind) g_shot.why = best->why;
		if (was >= 0 && g_shot.other >= 0 && g_shot.other != was)
			Log("           battle partner now %s (was %s)\n", Name(g_shot.other).c_str(), Name(was).c_str());
		std::string key = KindKey[g_shot.kind];
		if (g_shot.kind == kPit) return;            // pit cameras follow the phases above
		if (!g_shot.following && age > S.followAfter) {
			if (TvHold(g, PeekCam(key + "_follow"), g_shot.car)) return;
			g_shot.following = true; g_shot.camAt = now;
			{ char wb[96]; snprintf(wb, sizeof wb, "follow camera (shot %.0f s old, follow_after %.0f)", age, S.followAfter); g_tlCamWhy = wb; }
			SetCam(g, DynPick(g, g_shot.kind, g_shot.car, g_shot.other, g_car[g_shot.car].pitPhase, NextCam(key + "_follow")));
		} else if (g_shot.following && S.cameraEvery > 0 &&
		           now - g_shot.camAt > (onCam ? std::max(S.cameraEvery, S.onboardHold) : S.cameraEvery)) {
			if (TvHold(g, PeekCam(key + "_follow"), g_shot.car)) return;
			g_shot.camAt = now;
			{ char wb[96]; snprintf(wb, sizeof wb, "next follow camera (every %.0f s)", onCam ? std::max(S.cameraEvery, S.onboardHold) : S.cameraEvery); g_tlCamWhy = wb; }
			SetCam(g, DynPick(g, g_shot.kind, g_shot.car, g_shot.other, g_car[g_shot.car].pitPhase, NextCam(key + "_follow")));
		}
		return;
	}
	bool crash = best->kind == kIncident && g_shot.kind != kIncident;
	if (g_shot.car < 0 || (crash && age > 1.0) ||
	    (age > minHold && best->score > cur * S.switchMargin + 3) ||
	    (age > minHold && cur == 0)) {
		// not right on top of a TV camera change on screen (crashes never wait)
		if (best->kind != kIncident && g_shot.car >= 0 &&
		    TvHold(g, PeekCam(PitCamKey(best->kind, g_car[best->car].pitPhase)), best->car)) return;
		g_tlBest = best->score; g_tlCur = cur;
		Cut(g, *best);
	}
}

// ---------------------------------------------------------------- crash replays
// NR2003's own replay tape: from live, FN_REPLAY_PLAY rolls the tape from N seconds back (what the replay key does),
// FN_REPLAY_STOP goes back to live. Both run on the game's main thread through the camera hook. While the tape
// plays, the car data in memory may be the replay's, so Poll and the director stand still until it is back to live.
// A replay is one or more takes: the crash's hardest hit first, then (a multi-car crash) the next cars hit hardest,
// each from another camera - new angles on the same wreck.
static const char *CamName(int c);
struct Take { int car = -1, cam = -1; double guide = 0, len = 0, pre = -1; bool slow = false; };   // slow: the one slow-motion look   // guide: game s of the car's hardest hit; pre: starts this long before it (-1 = replay_preroll)
static double Pre(const Take &t) { return t.pre >= 0 ? t.pre : S.replayPreroll; }
static std::vector<Take> HighlightTakes(Crash &c);   // racevision_highlights.cpp
struct ReplayRun {
	bool paused = false;             // RaceVision paused the race for this replay (you were driving): resume after
	int phase = 0; int crash = -1, car = -1, cam = -1; double at = 0, len = 0, back = 0; int ticks0 = 0, frames0 = 0;
	std::vector<Take> takes; int take = 0; bool byHand = false, reel = false; double began = 0;
	double tape0 = 0;                // game s the tape showed when this take started (moves with it in real time)
	double stopSince = -1;           // the car on screen standing still since (real time)
	int frame0 = 0, endF = 0, curN = 0;   // smart slow motion: the tape frame the take began at / should end at, the slow step now (0 = normal)
	double guideF = -1;              // the tape frame of this take's hardest hit
	std::string speedTxt;            // what the banner says about the speed
};
static ReplayRun g_rp;                     // phase 0 = live, 1 = the tape is rolling
static int g_replayFails = 0;              // after 2 failed starts replays stay off until NR2003 restarts
static DWORD g_replaySigPid = 0;
static bool g_replaySigOk = false;
struct ReplayPlan { int crash = -1; double at = 0; };
static ReplayPlan g_plan;                  // an automatic replay counting down (the alert shows it)
static std::vector<int> g_reel;            // highlight reel: crashes still to play
static double g_reelNext = 0;
static volatile int g_reqReplayId = -1;    // play this crash (phone list) / -2 the highlight reel
static volatile bool g_reqReel = false;

static Crash *CrashById(int id) {             // (a highlight too: they share the replay machinery)
	for (auto &c : g_crashes) if (c.id == id) return &c;
	for (auto &c : g_highs) if (c.id == id) return &c;
	return nullptr;
}

static bool ReplaySigOk(Game &g) {
	if (g_replaySigPid != g.pid) {             // the tape functions are where they were found (this NR2003.exe)
		unsigned char a[8], b[8];
		g_replaySigOk = g.Read(FN_REPLAY_PLAY, a, 8) && g.Read(FN_REPLAY_STOP, b, 8) &&
		                !memcmp(a, SIG_REPLAY_PLAY, 8) && !memcmp(b, SIG_REPLAY_STOP, 8);
		g_replaySigPid = g.pid; g_replayFails = 0;
		if (!g_replaySigOk) Log("Replays: this NR2003.exe's replay code isn't the one RaceVision knows - replays off.\n");
	}
	return g_replaySigOk;
}

static bool GamePause(Game &g) {
	unsigned char b[13];
	if (!g.Read(FN_PAUSE, b, 13)) return false;
	for (int k = 0; k < 13; k++) if (SIG_PAUSE[k] >= 0 && b[k] != SIG_PAUSE[k]) return false;
	g.MainThreadCall(FN_PAUSE, 0, 0, 0);
	for (int j = 0; j < 40; j++) { if (g.Get<unsigned char>(SIM_STATE, 0) == 3) return true; Sleep(25); }
	return false;
}
static bool GameResume(Game &g) {
	if (g.Get<unsigned char>(SIM_STATE, 0) != 3) return true;
	g.MainThreadCall(FN_RESUME, 0, 0, 0);
	for (int j = 0; j < 40; j++) { if (g.Get<unsigned char>(SIM_STATE, 0) != 3) return true; Sleep(25); }
	return false;
}

// can a replay roll now? (why not, in words, for the log). drive: you may be in your car (the race gets paused)
static bool ReplayReady(Game &g, std::string &why, bool drive = false) {
	if (!g.h || !g.camOk) { why = "no camera control"; return false; }
	if (!ReplaySigOk(g)) { why = "unknown NR2003.exe"; return false; }
	if (g_replayFails >= 2) { why = "the replay didn't start twice - off until NR2003 restarts"; return false; }
	if (g.Get<int>(RP_MODE, -1) != 0) { why = "a replay is already playing"; return false; }
	int gs = g.Get<int>(GAME_STATE, -1);
	if (gs != 1 && gs != 4 && gs != 5) { why = "the game isn't taking replay keys (state " + std::to_string(gs) + ")"; return false; }
	if (g.Get<int>(DRIVER_STATE, -1) == 3 && !(drive && S.replayDrive)) { why = "you're in your car (Ctrl+F7 pauses the race and plays it)"; return false; }
	return true;
}

static std::string CrashWho(const Crash &c, size_t most = 99) {
	std::string who;
	std::vector<size_t> ord;                       // the replay's first car, then the most damage first
	for (size_t k = 0; k < c.cars.size(); k++) ord.push_back(k);
	auto hit = [&](size_t k) { return k < c.carHit.size() ? c.carHit[k] : 0.f; };
	std::stable_sort(ord.begin(), ord.end(), [&](size_t x, size_t y) {
		bool fx = c.cars[x] == c.impactCar, fy = c.cars[y] == c.impactCar;
		return fx != fy ? fx : hit(x) > hit(y);
	});
	for (size_t j = 0; j < ord.size() && j < most; j++)
		who += (who.empty() ? "" : ", ") + std::string("#") + NumLabel(g_car[c.cars[ord[j]]].number) + " " + Short(c.cars[ord[j]]);
	if (c.cars.size() > most) who += " +" + std::to_string(c.cars.size() - most);
	return who;
}

// the takes of crash c: its cars hit hardest first (each at its own hardest hit), at most `angles`, each camera
// different from the ones before it where the lists allow
static std::vector<Take> BuildTakes(Crash &c, int angles) {
	CarVecs(c);
	std::vector<size_t> order;
	for (size_t k = 0; k < c.cars.size(); k++) if (c.carPeak[k] >= S.contactJoin && c.carPeakAt[k] >= 0) order.push_back(k);
	std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return c.carPeak[x] > c.carPeak[y]; });
	std::vector<int> cams = S.cams["replay"];
	for (int x : S.cams["replay_angles"]) if (std::find(cams.begin(), cams.end(), x) == cams.end()) cams.push_back(x);
	int first = NextCam("replay");
	std::vector<Take> t;
	double guide0 = c.impactAt >= 0 ? c.impactAt : c.first;
	Take t0; t0.car = c.impactCar >= 0 ? c.impactCar : CrashLead(c); t0.cam = first; t0.guide = guide0;
	double end = std::max(std::min(c.lastAt, guide0 + 10.0) + 4.0, guide0 + 8.0);
	t0.len = S.replayPreroll + std::min(end - guide0, S.replayMax);
	t.push_back(t0);
	for (size_t k : order) {
		if ((int)t.size() >= angles) break;
		int car = c.cars[k];
		bool dup = false;
		for (auto &x : t) if (x.car == car) dup = true;
		if (dup) continue;
		Take n; n.car = car; n.guide = c.carPeakAt[k];
		double after = std::max(S.replayTake, std::min(c.lastAt, n.guide + 10.0) + 3.0 - n.guide);   // through the crash's end
		n.len = S.replayPreroll + std::min(after, S.replayMax);
		n.cam = -1;
		for (size_t j = 0; j < cams.size() && n.cam < 0; j++) {     // the next camera in the list not used yet
			int cand = cams[(std::find(cams.begin(), cams.end(), t.back().cam) - cams.begin() + 1 + j) % cams.size()];
			bool used = false;
			for (auto &x : t) if (x.cam == cand) used = true;
			if (!used) n.cam = cand;
		}
		if (n.cam < 0) n.cam = cams.empty() ? -1 : cams[t.size() % cams.size()];
		t.push_back(n);
	}
	return t;
}

// a sample of the tape against the game clock (live, race, about once a second); a shorter tape or an earlier clock
// is a new tape (the race started over)
static void TapeSample(Game &g) {
	if (g_session != SESSION_RACE || g.Get<int>(RP_MODE, -1) != 0) return;
	int f = g.Get<int>(RP_FRAMES, -1);
	if (f <= 0) return;
	if (!g_tapeMap.empty() && (f < g_tapeMap.back().frames || g_simT < g_tapeMap.back().t)) { g_tapeMap.clear(); TpReset(); }
	TpMarkTick(g, f);
	if (!g_tapeMap.empty() && g_simT - g_tapeMap.back().t < 1.0) return;
	g_tapeMap.push_back({ g_simT, f });
}
// the tape frame that holds game second t (between samples: in proportion; outside them: rate frames a second);
// -1 without samples
static double TapeFrameAt(double t, int rate) {
	if (g_tapeMap.empty()) return -1;
	auto it = std::lower_bound(g_tapeMap.begin(), g_tapeMap.end(), t, [](const TapeAt &a, double v) { return a.t < v; });
	if (it == g_tapeMap.begin()) return it->frames - (it->t - t) * rate;
	if (it == g_tapeMap.end()) return g_tapeMap.back().frames + (t - g_tapeMap.back().t) * rate;
	const TapeAt &a = *(it - 1), &b = *it;
	return b.t - a.t > 0 ? a.frames + (b.frames - a.frames) * (t - a.t) / (b.t - a.t) : a.frames;
}

// roll take k of the replay in g_rp (from live: Play; between takes: Stop, then Play again)
static bool RollTake(Game &g, int k) {
	SYSTEMTIME st; GetLocalTime(&st);
	Crash *c = CrashById(g_rp.crash);
	if (!c || k >= (int)g_rp.takes.size()) return false;
	Take &tk = g_rp.takes[k];
	TrGuard tg;
	if (k > 0) TrBegin(tg, 2, "ANGLE " + std::to_string(k + 1));     // (the first take rides the wipe ReplayStart began)
	if (g.Get<int>(RP_MODE, -1) != 0) {
		g.MainThreadCall(FN_REPLAY_STOP, 1, 0, 0);
		bool live = false;
		for (int j = 0; j < 80 && !live; j++) { Sleep(25); live = g.Get<int>(RP_MODE, -1) == 0; }
		if (!live) return false;
	}
	double simNow = GameSec(g), start = tk.guide - Pre(tk);
	double back = simNow - start, len = tk.len;
	int frames = g.Get<int>(RP_FRAMES, 0), rate = g.Get<int>(TICK_RATE_FLAG, 1) != 0 ? 36 : g.Get<int>(TICK_RATE_VALUE, 36);
	if (rate <= 0 || rate > 10000) rate = 36;
	double fStart = TapeFrameAt(start, rate);
	if (fStart >= 0) {                                          // tape seconds, not clock seconds (see g_tapeMap)
		double tapeBack = (frames - fStart) / rate;
		if (fabs(tapeBack - back) > 3)
			Log("           the tape has %.0fs since then, the race clock %.0fs - going back by the tape\n", tapeBack, back);
		back = tapeBack;
	}
	double back0 = back;
	double onTape = frames / (double)rate - 1;
	if (back > onTape) {                                        // the tape doesn't go back that far: as far as it does
		Log("           tape holds %.0fs, the replay would start %.0fs ago\n", onTape, back);
		if (onTape < back - Pre(tk) - 1) { Log("[%02d:%02d:%02d] REPLAY   %s is off the tape\n", st.wHour, st.wMinute, st.wSecond, HlLabel(*c).c_str()); return false; }
		len -= back - onTape;
		back = onTape;
	}
	if (back < 1 || len < 2) return false;
	float fb = (float)back; DWORD a0; memcpy(&a0, &fb, 4);
	bool ran = g.MainThreadCall(FN_REPLAY_PLAY, a0, 0, 1);       // (seconds back, not paused, 1 = like the key)
	int mode = -1;
	for (int j = 0; j < 40 && mode != 1; j++) { Sleep(25); mode = g.Get<int>(RP_MODE, -1); }
	if (mode == 1 && !ran) Log("           (the replay call was slow to report back, but the tape is rolling)\n");
	if (mode != 1) {
		g_replayFails++;
		Log("[%02d:%02d:%02d] REPLAY   %s: the replay did not start (%s, tape mode %d)\n", st.wHour, st.wMinute, st.wSecond,
		    HlLabel(*c).c_str(), ran ? "called" : "call not run", mode);
		return false;
	}
	bool camOk = tk.car >= 0 && g.Set(tk.cam, tk.car, true);
	if (tk.car >= 0 && g.Get<int>(REPLAY_REC0 + tk.car * REC_SIZE + REC_IDX, -1) != tk.car)   // not the layout we expect: say so
		Log("           (replay car record %d reads %d)\n", tk.car, g.Get<int>(REPLAY_REC0 + tk.car * REC_SIZE + REC_IDX, -1));
	if (camOk && tk.cam >= 0) g_lastCam = tk.cam;
	g_rp.phase = 1; g_rp.take = k; g_rp.car = tk.car; g_rp.cam = tk.cam; g_rp.at = Now(); g_rp.len = len; g_rp.back = back;
	g_rp.tape0 = start + (back0 - back); g_rp.stopSince = -1;    // the game second the tape starts at
	{   // the speed: normal from here (the game keeps the last one it used), smart slow motion takes over from the tick
		g_tpManual = false;
		TpSetSpeed(g, 1, false);
		g_rp.curN = 0; g_rp.frame0 = g.Get<int>(TP_IDX, 0);
		double gf = TapeFrameAt(tk.guide, rate);
		g_rp.guideF = gf >= 0 ? gf : g_rp.frame0 + (tk.guide - g_rp.tape0) * rate;
		g_rp.endF = g_rp.frame0 + (int)(len * rate);
	}
	if (k == 0) { g_rp.frames0 = frames; g_rp.began = Now(); }
	float hit = 0;
	CarVecs(*c);
	for (size_t j = 0; j < c->cars.size(); j++) if (c->cars[j] == tk.car) hit = c->carPeak[j];
	Log("[%02d:%02d:%02d] REPLAY   %s (%s%s%s) angle %d of %d: %s, cam %d (%s), from %.1fs back = %.1fs before its %s (+%.3f), %.0fs%s%s%s\n",
	    st.wHour, st.wMinute, st.wSecond, HlLabel(*c).c_str(), c->kind ? c->what.c_str() : "", c->kind ? ": " : "", CrashWho(*c, 6).c_str(), k + 1, (int)g_rp.takes.size(),
	    tk.car >= 0 ? Name(tk.car).c_str() : "-", tk.cam, tk.cam >= 0 ? CamName(tk.cam) : "-", back, tk.guide - g_rp.tape0,
	    c->kind ? "moment" : "hardest hit", hit, len,
	    g_rp.byHand ? ", by hand" : "", g_reel.empty() ? "" : ", highlight reel", camOk ? "" : "  (camera FAILED)");
	if (k == 0) Log("           tape %d frames at %d/s, rolling %d, race %s, worth %.1f\n", frames, rate, g.Get<int>(RP_ROLLING, -1),
	                g.Get<unsigned char>(SIM_STATE, 0) == 3 ? "paused" : "running", Worth(*c));
	return true;
}

static void ReplayEnd(Game &g, const char *why);
static void ReplayStart(Game &g, int crashId, bool byHand, int angles = -1, bool drive = false) {
	SYSTEMTIME st; GetLocalTime(&st);
	Crash *c = CrashById(crashId);
	if (!c) { Log("[%02d:%02d:%02d] REPLAY   no crash logged yet\n", st.wHour, st.wMinute, st.wSecond); return; }
	c->replayed = true;
	std::string why;
	if (!ReplayReady(g, why, drive)) { Log("[%02d:%02d:%02d] REPLAY   %s not replayed: %s\n", st.wHour, st.wMinute, st.wSecond, HlLabel(*c).c_str(), why.c_str()); return; }
	bool pausedHere = false;
	if (g.Get<int>(DRIVER_STATE, -1) == 3) {                     // you're driving: the race waits while you watch
		if (g.Get<unsigned char>(SIM_STATE, 0) != 3) {
			if (!GamePause(g)) { Log("[%02d:%02d:%02d] REPLAY   %s not replayed: couldn't pause the race\n", st.wHour, st.wMinute, st.wSecond, HlLabel(*c).c_str()); return; }
			pausedHere = true;
		}
		Log("[%02d:%02d:%02d] REPLAY   race paused for the replay (you're in your car)\n", st.wHour, st.wMinute, st.wSecond);
	}
	TrGuard tg;
	TrBegin(tg, 0, std::string(c->kind ? (c->what.empty() ? HlTitle(c->kind) : c->what.c_str()) : "CRASH") + "  -  " + CrashWho(*c, 2));
	g_rp = ReplayRun(); g_tpManual = false;
	g_rp.paused = pausedHere;
	g_rp.crash = c->id; g_rp.byHand = byHand; g_rp.ticks0 = g.Get<int>(GAME_TICKS, 0);
	g_rp.takes = c->kind ? HighlightTakes(*c) : BuildTakes(*c, angles > 0 ? angles : S.replayAngles);
	// smart slow motion: ONE slow look, last, after the real-speed angles - only for a big hit (its hardest single damage
	// jump replay_slow_impact or more) or a photo finish, and never for a pass. It is the hardest hit, from the best
	// angle: the first replay camera (user, 2026-11-11: not every crash, and not a second angle of each)
	bool slowOk = c->kind == HL_CRASH ? c->impact >= S.replaySlowImpact && c->impactCar >= 0 && c->impactAt >= 0
	                                  : c->kind == HL_FINISH && c->score >= 8.5;
	if (S.replaySmart && angles <= 0 && slowOk && !g_rp.takes.empty()) {
		Take d;
		d.car = c->kind == HL_CRASH ? c->impactCar : g_rp.takes[0].car;
		d.guide = c->kind == HL_CRASH ? c->impactAt : g_rp.takes[0].guide;
		const std::vector<int> &cams = S.cams["replay"];
		d.cam = !cams.empty() ? cams[0] : g_rp.takes[0].cam;
		d.pre = 2.0; d.len = d.pre + 4.0; d.slow = true;
		g_rp.takes.push_back(d);
	}
	for (int k = 0; k < (int)g_rp.takes.size(); k++)          // a take that can't roll (off the tape): the next one
		if (RollTake(g, k)) return;
	if (g.Get<int>(RP_MODE, -1) == 1) { g_rp.phase = 1; ReplayEnd(g, "no take would roll"); }
	else if (g_rp.paused) GameResume(g);
	g_rp.phase = 0; g_rp.paused = false;
}
// the live camera following a replay car record (left so by an older build, or anything else): put it on that car's
// live record
static void LiveCameraCheck(Game &g) {
	DWORD rec = g.Get<DWORD>(FOCUS_ADDR, 0);
	if (rec < REPLAY_REC0 || (rec - REPLAY_REC0) % REC_SIZE || (rec - REPLAY_REC0) / REC_SIZE >= NUM_SLOTS) return;
	int idx = (int)((rec - REPLAY_REC0) / REC_SIZE), cam = g.Camera();
	bool was = g.camOk;
	g.camOk = true;                                    // (Focus() can't read a replay record: camOk may be off)
	bool ok = g.Set(cam > 0 ? cam : 19, idx);
	g.camOk = was || ok;
	Log("           the live camera was still on the replay's car %s - %s\n", Name(idx).c_str(), ok ? "put back on the live car" : "COULDN'T fix it");
}

// back to live, and the director starts afresh (no stale speeds or gaps from before the replay)
static void ReplayEnd(Game &g, const char *why) {
	SYSTEMTIME st; GetLocalTime(&st);
	TrGuard tg;
	if (g.Get<int>(RP_MODE, -1) == 1) {
		if (g_reel.empty() && g_rp.phase) TrBegin(tg, 1, "BACK TO THE RACE");
		TpSetSpeed(g, 1, false);                         // (the game plays the next replay at the speed this one ended at)
	}
	g_tpManual = false;
	bool live = g.Get<int>(RP_MODE, -1) == 0;
	for (int k = 0; k < 2 && !live; k++) {               // (a second Stop only once the first one has had 2 s)
		g.MainThreadCall(FN_REPLAY_STOP, 1, 0, 0);
		for (int j = 0; j < 80 && !live; j++) { Sleep(25); live = g.Get<int>(RP_MODE, -1) == 0; }
	}
	int ticks = g.Get<int>(GAME_TICKS, 0), rate = g.Get<int>(TICK_RATE_FLAG, 1) != 0 ? 36 : g.Get<int>(TICK_RATE_VALUE, 36);
	if (rate <= 0) rate = 36;
	double ran = (ticks - g_rp.ticks0) / (double)rate, shown = Now() - (g_rp.began > 0 ? g_rp.began : g_rp.at);
	Log("[%02d:%02d:%02d] REPLAY   over (%s), %s - %.0fs shown, the race %s (%.1f game s), tape %d -> %d frames\n",
	    st.wHour, st.wMinute, st.wSecond, why, live ? "back to live" : "STILL IN THE REPLAY (press the replay end key)",
	    shown, ran > shown * 0.5 ? "ran on" : "waited", ran, g_rp.frames0, g.Get<int>(RP_FRAMES, 0));
	g_rp.phase = 0;
	if (g_rp.paused) {
		g_rp.paused = false;
		bool on = GameResume(g);
		Log(on ? "           race resumed\n" : "           couldn't resume the race - press the pause key\n");
	}
	for (int i = 0; i < NUM_SLOTS; i++) {
		Car &c = g_car[i];
		c.hist.clear(); c.trace.clear(); c.prevDist = -1; c.stoppedSince = -1; c.passSince = -1; c.passed = false; c.battleSince = -1;
		c.dmgTotal = -1; c.hitStart = -1;
	}
	g_gapHist.clear(); g_passCar = -1;
	LiveCameraCheck(g);
	int f = g.Focus();
	if (f >= 0 && f != g_mount) g_shot.car = f;
	g_shot.kind = kTour; g_shot.other = -1; g_shot.why = ""; g_shot.since = Now() - S.maxHold - 1;   // free to cut at once
}

// while the tape rolls: end it at its length, or when it stopped by itself (the replay keys, or the tape end)
static void ReplayTick(Game &g) {
	if (g_rp.phase != 1) return;
	if (g.Get<int>(RP_MODE, 0) != 1) {
		g_rp.phase = 0;
		ReplayEnd(g, "the game left the replay");
		return;
	}
	g_rp.speedTxt = TpSpeedText(g);
	if (g_tpManual || g_rp.takes.empty()) return;        // the transport has the tape: no timed end, no angle changes
	// the car on screen has come to rest after its hit (its replay speed under 2 for 1.5 s): its story is told
	double now = Now(), tapeNow = g_rp.tape0 + (now - g_rp.at);
	const Take &cur = g_rp.takes[g_rp.take];
	int idx = g.Get<int>(TP_IDX, 0), rate = TpRate(g);
	bool smart = S.replaySmart && g_rp.endF > 0 && g_rp.guideF >= 0;
	if (smart) {     // slow motion into the hit and out of it; the take runs by tape frames, not by the clock
		tapeNow = g_rp.tape0 + (idx - g_rp.frame0) / (double)rate;
		const Crash *cr = CrashById(g_rp.crash);
		double worth = cr ? Worth(*cr) : 3;
		int nMax = S.replaySlowMax;
		if (cr && cr->kind) nMax = std::min(nMax, cr->kind == HL_FINISH ? 4 : 2);   // a pass, a bump: a gentle slow look
		else if (worth < 2) nMax = std::min(nMax, 2); else if (worth < 5) nMax = std::min(nMax, 4);
		double d = (idx - g_rp.guideF) / (double)rate;           // seconds from the hit on the tape
		int want = 0;
		bool slowHere = cur.slow;                                  // the one slow-motion look (ReplayStart)
		if (!slowHere) want = 0;
		else if (d >= -1.6 && d < -0.8) want = 1;
		else if (d >= -0.8 && d < -0.25) want = std::max(1, nMax / 2);
		else if (d >= -0.25 && d < 1.4) want = nMax;              // ...and straight back to full speed after the hit
		if (want != g_rp.curN) { g_rp.curN = want; if (want) TpSetSpeed(g, want, true); else TpSetSpeed(g, 1, false); }
	}
	bool stopped = false;
	const Crash *rc = CrashById(g_rp.crash);
	double over = std::max(cur.guide + 4.0, rc ? std::min(rc->lastAt, cur.guide + 10.0) + 2.0 : cur.guide + 4.0);
	if (g_rp.car >= 0 && tapeNow > over) {
		float v = g.Get<float>(REPLAY_REC0 + g_rp.car * REC_SIZE + REC_MPH, 99.f);
		if (v == v && fabs(v) < 2) { if (g_rp.stopSince < 0) g_rp.stopSince = now; }
		else g_rp.stopSince = -1;
		stopped = g_rp.stopSince >= 0 && now - g_rp.stopSince >= 1.5;
	}
	bool timeUp = smart ? (idx >= g_rp.endF || now - g_rp.at > g_rp.len * 5 + 10) : now - g_rp.at >= g_rp.len;
	if (!timeUp && !stopped) return;
	SYSTEMTIME st; GetLocalTime(&st);
	if (stopped && g_rp.take + 1 < (int)g_rp.takes.size())
		Log("[%02d:%02d:%02d] REPLAY   %s has stopped - next angle\n", st.wHour, st.wMinute, st.wSecond, Name(g_rp.car).c_str());
	for (int k = g_rp.take + 1; k < (int)g_rp.takes.size(); k++) {  // the next angle
		Take &nx = g_rp.takes[k];
		if (nx.guide >= tapeNow + 1.5 && nx.guide - tapeNow < 15) {
			// its hit is still to come on the tape: just cut to that car on its camera, no rewind
			bool camOk = nx.car >= 0 && g.Set(nx.cam, nx.car, true);
			if (camOk && nx.cam >= 0) g_lastCam = nx.cam;
			g_rp.take = k; g_rp.car = nx.car; g_rp.cam = nx.cam; g_rp.at = now; g_rp.tape0 = tapeNow; g_rp.stopSince = -1;
			g_rp.len = nx.guide - tapeNow + nx.len - Pre(nx);      // (its hit still to come: no preroll needed)
			g_rp.frame0 = idx; g_rp.endF = idx + (int)(g_rp.len * rate);
			{ double gf = TapeFrameAt(nx.guide, rate); g_rp.guideF = gf >= 0 ? gf : idx + (nx.guide - tapeNow) * rate; }
			Log("[%02d:%02d:%02d] REPLAY   angle %d of %d: %s, cam %d (%s), its hardest hit %.1fs on%s\n", st.wHour, st.wMinute, st.wSecond,
			    k + 1, (int)g_rp.takes.size(), Name(nx.car).c_str(), nx.cam, nx.cam >= 0 ? CamName(nx.cam) : "-", nx.guide - tapeNow,
			    camOk ? "" : "  (camera FAILED)");
			return;
		}
		if (RollTake(g, k)) return;
	}
	ReplayEnd(g, "done");
	if (!g_reel.empty()) g_reelNext = Now() + 1.0;
}

// You got out of your car: NR2003 then plays its replay from 20 s back (its GetOutOfCar does that), the cars it shows
// are the replay's, and the director can't direct in it (live 2026-10-26: you had to skip to live and pick a car by
// hand). So within a few seconds of climbing out, a replay RaceVision didn't start is stopped: back to live.
static void ClimbOutWatch(Game &g) {
	static int was = -1;
	static double outAt = -100;
	int ds = g.Get<int>(DRIVER_STATE, -1);
	if (was == 3 && ds != 3) { outAt = Now(); Log("           you got out of your car\n"); }
	was = ds;
	if (!S.liveAfterClimbOut || g_rp.phase || Now() - outAt > 8 || !g.camOk || !ReplaySigOk(g)) return;
	if (g.Get<int>(RP_MODE, 0) != 1) return;
	outAt = -100;
	g.MainThreadCall(FN_REPLAY_STOP, 1, 0, 0);
	bool live = false;
	for (int j = 0; j < 20 && !live; j++) { Sleep(25); live = g.Get<int>(RP_MODE, -1) == 0; }
	Log(live ? "           skipped the game's climb-out replay - back to live\n" : "           couldn't stop the game's climb-out replay\n");
	if (!live) return;
	LiveCameraCheck(g);
	for (int i = 0; i < NUM_SLOTS; i++) { g_car[i].hist.clear(); g_car[i].prevDist = -1; }
	int f = g.Focus();
	if (f >= 0 && f != g_mount) g_shot.car = f;
	g_shot.kind = kTour; g_shot.other = -1; g_shot.why = ""; g_shot.since = Now() - S.maxHold - 1;   // free to cut at once
}

// replay_auto: the crash most worth showing that nobody has seen again yet, once it is over, when the race allows
// it - after the alert has counted replay_countdown down
// -1 = no replay now, -2 = wait (the crash is still going on), else the crash to replay
static int ReplayPick() {
	if (!S.replayAuto || g_rp.phase || g_session != SESSION_RACE || g_score.empty() || !g_reel.empty()) return -1;
	if (S.replayCautionOnly && (g_flag == FLAG_GREEN || g_flag == 0)) return -1;
	double now = Now();
	if (now < g_restartUntil || (g_lapsTotal > 0 && g_leaderLaps >= g_lapsTotal - 1)) return -1;
	if (g_shot.kind == kManual && now - g_shot.since < S.manualHold) return -1;
	if (g_youDrive) return -1;
	for (auto &c : g_crashes) if (now - c.realAt < S.replayAfter && !c.replayed && Worth(c) >= S.replayWorthy) return -2;   // one still going on
	const Crash *best = nullptr;
	for (auto &c : g_crashes) {
		if (c.replayed || g_simT - c.first > S.replayWithin || Worth(c) < S.replayWorthy) continue;
		if (!best || Worth(c) > Worth(*best)) best = &c;
	}
	if (!best) return -1;
	for (int o : best->cars) {                              // a car in it still losing speed fast
		float top = 0;
		for (auto &sm : g_car[o].hist) top = std::max(top, sm.v);
		if (g_car[o].valid && top - g_car[o].speed > 10) return -2;
	}
	return best->id;
}
static void ReplayAuto(Game &g) {
	int id = ReplayPick();
	if (id == -2 && g_plan.crash >= 0) { g_plan.at = std::max(g_plan.at, Now() + 1.0); return; }   // hold at 1
	if (id < 0) { g_plan.crash = -1; return; }
	if (g_plan.crash != id) {
		g_plan.crash = id; g_plan.at = Now() + S.replayCountdown;
		SYSTEMTIME st; GetLocalTime(&st);
		Log("[%02d:%02d:%02d] REPLAY   crash #%d in %.0fs\n", st.wHour, st.wMinute, st.wSecond, id, S.replayCountdown);
	}
	if (Now() < g_plan.at) return;
	g_plan.crash = -1;
	ReplayStart(g, id, false);
}

static double g_remindUntil = 0;           // you're driving: the next caution reminds you of a crash to watch
static int g_remindCrash = -1;
// a crash just became worth a replay: say so (the alert overlay shows it for a few seconds)
static void CrashWorthWatch() {
	static int lastFlag = -1;
	if (g_flag != lastFlag) {
		bool caution = lastFlag == FLAG_GREEN && g_flag != FLAG_GREEN && g_flag != 0;
		lastFlag = g_flag;
		if (caution && g_youDrive && S.replayDrive) {
			const Crash *best = nullptr;
			for (auto &c : g_crashes)
				if (!c.replayed && Worth(c) >= S.replayWorthy && g_simT - c.first <= S.replayWithin * 3 && (!best || Worth(c) > Worth(*best))) best = &c;
			if (best) {
				g_remindCrash = best->id; g_remindUntil = Now() + 12;
				Log("           caution: crash #%d is waiting - Ctrl+F7 to watch it\n", best->id);
			}
		}
	}
	for (auto &c : g_crashes)
		if (c.worthyAt < 0 && Worth(c) >= S.replayWorthy) {
			c.worthyAt = Now();
			SYSTEMTIME st; GetLocalTime(&st);
			Log("[%02d:%02d:%02d] REPLAY-WORTHY crash #%d, lap %d: %s - damage %.2f, worth %.1f\n", st.wHour, st.wMinute, st.wSecond,
			    c.id, c.lap, CrashWho(c, 8).c_str(), c.damage, Worth(c));
		}
}

// the highlight reel: every crash worth a replay this race (on the tape), and the other highlights (reel_highlights),
// in race order, reel_angles each
static void ReelStart() {
	g_reel.clear();
	std::vector<std::pair<double, int>> all;
	int crashes = 0;
	for (auto &c : g_crashes) if (Worth(c) >= S.replayWorthy) { all.push_back({ c.first, c.id }); crashes++; }
	if (S.reelHighlights) for (auto &c : g_highs) all.push_back({ c.first, c.id });
	std::stable_sort(all.begin(), all.end(), [](const std::pair<double, int> &a, const std::pair<double, int> &b) { return a.first < b.first; });
	for (auto &x : all) g_reel.push_back(x.second);
	SYSTEMTIME st; GetLocalTime(&st);
	Log("[%02d:%02d:%02d] REPLAY   highlight reel: %d crash%s, %d other highlight%s\n", st.wHour, st.wMinute, st.wSecond,
	    crashes, crashes == 1 ? "" : "es", (int)g_reel.size() - crashes, (int)g_reel.size() - crashes == 1 ? "" : "s");
	g_reelNext = Now();
}
static void ReelTick(Game &g) {
	if (g_reel.empty() || g_rp.phase || Now() < g_reelNext) return;
	int id = g_reel.front();
	g_reel.erase(g_reel.begin());
	ReplayStart(g, id, true, S.reelAngles);
	if (g_rp.phase) g_rp.reel = true;
	else {
		if (g.Get<int>(DRIVER_STATE, -1) == 3 || !g.camOk) { g_reel.clear(); Log("           highlight reel stopped\n"); }
		else g_reelNext = Now() + 0.5;
	}
}

// ---------------------------------------------------------------- overlays
using namespace Gdiplus;

struct Panel {
	const char *key;
	int baseW, baseH;
	int x = 40, y = 40;
	float scale = 1.0f;
	HWND hwnd = NULL;
	bool shown = false;
};
static Panel g_tag = { "tag", 560, 126 }, g_battle = { "battle", 420, 110 }, g_onboard = { "onboard", 300, 120 };
static Panel g_dash = { "dash", 660, 140 };
static Panel g_alert = { "alert", 460, 100 };       // crash alert / replay countdown / REPLAY bug
static Panel g_qualBox = { "qual", 420, 96 };       // qualifying: pole + live delta of the car on screen
static void DrawQual(Graphics &gr, bool sample);
static void DrawAlert(Graphics &gr, bool sample);
static Panel *g_panels[] = { &g_tag, &g_battle, &g_onboard, &g_dash, &g_alert };   // g_qualBox: Race Control shows qualifying timing now
static bool g_edit = false, g_overlaysOn = true;
static Game *g_game = nullptr;

static Color C(int a, int r, int gg, int b) { return Color((BYTE)a, (BYTE)r, (BYTE)gg, (BYTE)b); }

// ---- driver art from a companion app's series folders (read-only):
//   <root>\series\<id>\numbers\<n>.png, headshots\<n>.png, colors.json {"<n>": "#rrggbb"}
//   <root>\images\defaultheadshot.png;  the companion app's active series is in its config.json
static std::vector<std::string> g_artRoots;
static std::string g_artSeriesDir, g_artSeries, g_artHow;
static std::map<int, Color> g_colors;
static std::map<std::string, Image *> g_imgs;

static bool IsDir(const std::string &p) { DWORD a = GetFileAttributesA(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static bool IsFile(const std::string &p) { DWORD a = GetFileAttributesA(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static std::string ReadAll(const std::string &p) {
	std::string out; FILE *f = fopen(p.c_str(), "rb");
	if (!f) return out;
	char b[4096]; size_t n;
	while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
	fclose(f); return out;
}
// every "key": "value" string pair in a JSON text (enough for config.json / colors.json)
static std::vector<std::pair<std::string, std::string>> JsonPairs(const std::string &t) {
	std::vector<std::pair<std::string, std::string>> out;
	size_t i = 0;
	auto str = [&](size_t &j, std::string &o) {
		if (j >= t.size() || t[j] != '"') return false;
		size_t e = t.find('"', j + 1); if (e == std::string::npos) return false;
		o = t.substr(j + 1, e - j - 1); j = e + 1; return true;
	};
	while ((i = t.find('"', i)) != std::string::npos) {
		std::string k, v; size_t j = i;
		if (!str(j, k)) break;
		size_t c = j; while (c < t.size() && (t[c] == ' ' || t[c] == '\t' || t[c] == '\r' || t[c] == '\n')) c++;
		if (c < t.size() && t[c] == ':') {
			c++; while (c < t.size() && (t[c] == ' ' || t[c] == '\t' || t[c] == '\r' || t[c] == '\n')) c++;
			if (str(c, v)) { out.push_back({ k, v }); i = c; continue; }
		}
		i = j;
	}
	return out;
}

// art folders RaceVision finds by itself: "art" next to RaceVision.exe, AIMOD's art next to a running AIMOD
// (remembered once seen), and the companion app's folder
static std::vector<std::string> AutoArtRoots() {
	static std::string aimod;
	std::vector<std::string> out;
	char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
	std::string mine = exe; mine = mine.substr(0, mine.find_last_of('\\'));
	out.push_back(mine + "\\art");
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32 e = { sizeof e };
	if (snap != INVALID_HANDLE_VALUE && Process32First(snap, &e)) {
		do {
			if (_strnicmp(e.szExeFile, "AIMOD", 5) != 0) continue;
			HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
			char path[MAX_PATH]; DWORD n = MAX_PATH;
			if (h && QueryFullProcessImageNameA(h, 0, path, &n)) {
				std::string dir = path; dir = dir.substr(0, dir.find_last_of('\\')) + "\\art";
				if (IsDir(dir + "\\series")) aimod = dir;
			}
			if (h) CloseHandle(h);
		} while (Process32Next(snap, &e));
	}
	if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
	if (!aimod.empty()) out.push_back(aimod);
	if (!S.companionFolder.empty()) out.push_back(S.companionFolder);
	return out;
}

static void ArtRoots() {
	g_artRoots.clear();
	auto add = [](const std::string &dir) {
		for (auto &r : g_artRoots) if (_stricmp(r.c_str(), dir.c_str()) == 0) return;
		if (IsDir(dir + "\\series")) g_artRoots.push_back(dir);
	};
	std::string list = S.artFolders + ";";
	size_t a = 0, b;
	while ((b = list.find(';', a)) != std::string::npos) {
		std::string one = Trim(list.substr(a, b - a)); a = b + 1;
		if (one.empty()) continue;
		char ex[MAX_PATH]; ExpandEnvironmentStringsA(one.c_str(), ex, MAX_PATH);
		add(ex);
	}
	for (auto &r : AutoArtRoots()) add(r);
}

static std::string SeriesDir(const std::string &id) {
	for (auto &r : g_artRoots) if (IsDir(r + "\\series\\" + id)) return r + "\\series\\" + id;
	return "";
}

// which series: the one in the settings, else the companion app's active one, else the
// series whose number cards cover the most cars on track
static void PickSeries() {
	std::string pick, how;
	if (_stricmp(S.artSeries.c_str(), "auto") != 0 && !SeriesDir(S.artSeries).empty()) { pick = S.artSeries; how = "from the settings"; }
	if (pick.empty()) {
		std::string ex = S.companionFolder.empty() ? std::string() : S.companionFolder + "\\config.json";
		for (auto &kv : JsonPairs(ex.empty() ? std::string() : ReadAll(ex)))
			if (kv.first == "series" && !SeriesDir(kv.second).empty()) { pick = kv.second; how = "the companion app's active series"; break; }
	}
	if (pick.empty()) {
		int best = 0;
		for (auto &r : g_artRoots) {
			WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((r + "\\series\\*").c_str(), &fd);
			if (h == INVALID_HANDLE_VALUE) continue;
			do {
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
				int n = 0;
				for (int i : g_order) n += IsFile(r + "\\series\\" + fd.cFileName + "\\numbers\\" + NumLabel(g_car[i].number) + ".png");
				if (n > best) { best = n; pick = fd.cFileName; how = "matched to the cars on track"; }
			} while (FindNextFileA(h, &fd));
			FindClose(h);
		}
	}
	if (pick == g_artSeries) return;
	g_artSeries = pick; g_artSeriesDir = SeriesDir(pick); g_artHow = how;
	g_colors.clear();
	for (auto &kv : JsonPairs(ReadAll(g_artSeriesDir + "\\colors.json"))) {
		unsigned rgb = 0;
		if (kv.second.size() == 7 && kv.second[0] == '#' && sscanf(kv.second.c_str() + 1, "%x", &rgb) == 1 && _stricmp(kv.second.c_str(), "#888888") != 0)
			g_colors[NumKey(RawNum(kv.first))] = C(255, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
	}
	if (!pick.empty()) Log("Art: series %s (%s), %d driver colours\n", pick.c_str(), how.c_str(), (int)g_colors.size());
	else Log("Art: no series folder found (art_folders in racevision.ini)\n");
}

static Image *Img(const std::string &p) {
	auto it = g_imgs.find(p);
	if (it != g_imgs.end()) return it->second;
	Image *img = nullptr;
	if (IsFile(p)) {
		std::wstring w(p.begin(), p.end());
		img = Image::FromFile(w.c_str());
		if (img && img->GetLastStatus() != Ok) { delete img; img = nullptr; }
	}
	return g_imgs[p] = img;
}
// the car the overlays show: the replay's car while the tape rolls (2026-10-27), else the TV car
static int OvCar() { return g_rp.phase && g_rp.car >= 0 && g_rp.car < NUM_SLOTS ? g_rp.car : g_shot.car; }
static bool OvReplay() { return g_rp.phase && g_rp.car >= 0 && g_rp.car < NUM_SLOTS; }
// a reading of its car record: the replay's record while the tape rolls (the live one keeps the race)
static float OvRec(int i, DWORD off) { return OvReplay() ? g_game->Get<float>(REPLAY_REC0 + i * REC_SIZE + off, 0.f) : g_game->RecF(i, off); }
static Image *NumberArt(int n) { return g_artSeriesDir.empty() ? nullptr : Img(g_artSeriesDir + "\\numbers\\" + NumLabel(n) + ".png"); }
static Image *Headshot(int n) {
	Image *img = g_artSeriesDir.empty() ? nullptr : Img(g_artSeriesDir + "\\headshots\\" + NumLabel(n) + ".png");
	for (size_t r = 0; !img && r < g_artRoots.size(); r++) img = Img(g_artRoots[r] + "\\images\\defaultheadshot.png");
	return img;
}
// draw an image inside a box, keeping its shape; returns the width used
static float DrawFit(Graphics &gr, Image *img, float x, float y, float w, float h, bool alignRight = false) {
	if (!img) return 0;
	float iw = (float)img->GetWidth(), ih = (float)img->GetHeight(), k = std::min(w / iw, h / ih);
	float dw = iw * k, dh = ih * k;
	gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
	gr.DrawImage(img, RectF(alignRight ? x + w - dw : x, y + (h - dh) / 2, dw, dh));
	return dw;
}
static void FillRound(Graphics &gr, const Brush &br, float x, float y, float w, float h, float r) {
	GraphicsPath p;
	p.AddArc(x, y, r * 2, r * 2, 180, 90);
	p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
	p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
	p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
	p.CloseFigure();
	gr.FillPath(&br, &p);
}
// width of the letters themselves (no GDI+ margin), trailing spaces included
static float InkWidth(Graphics &gr, const std::wstring &ws, const Font &font) {
	StringFormat tf(StringFormat::GenericTypographic());
	tf.SetFormatFlags(tf.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
	RectF r;
	gr.MeasureString(ws.c_str(), -1, &font, PointF(0, 0), &tf, &r);
	return r.Width;
}
// how far the drawn letters really reach from where the text starts. MeasureString leaves out GDI+'s
// made-up bold / italic (the condensed font has neither), so the text is drawn off screen and looked at;
// remembered, as the same strings come round every frame
static float InkReach(const std::wstring &ws, const Font &font) {
	static std::map<std::wstring, float> seen;
	size_t end = ws.find_last_not_of(L' ');
	if (end == std::wstring::npos) return 0;
	FontFamily fam; font.GetFamily(&fam);
	wchar_t name[LF_FACESIZE] = L""; fam.GetFamilyName(name);
	wchar_t head[96]; swprintf(head, 96, L"%s|%.2f|%d|", name, font.GetSize(), font.GetStyle());
	std::wstring key = head + ws.substr(0, end + 1);
	auto it = seen.find(key);
	if (it != seen.end()) return it->second;
	float size = font.GetSize();
	int pw = (int)(size * (end + 4)), ph = (int)(size * 2.5f), ox = (int)size;
	Bitmap bmp(pw, ph, PixelFormat32bppARGB);
	{
		Graphics g(&bmp);
		g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
		g.Clear(Color(0, 0, 0, 0));
		SolidBrush br(Color(255, 255, 255, 255));
		StringFormat tf(StringFormat::GenericTypographic());
		tf.SetFormatFlags(tf.GetFormatFlags() | StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
		g.DrawString(ws.c_str(), (INT)end + 1, &font, PointF((float)ox, size * 0.25f), &tf, &br);
	}
	int right = ox;
	BitmapData bd; Rect all(0, 0, pw, ph);
	if (bmp.LockBits(&all, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok) {
		for (int y = 0; y < ph; y++) {
			const DWORD *row = (const DWORD *)((const BYTE *)bd.Scan0 + y * bd.Stride);
			for (int x = pw - 1; x > right; x--) if (row[x] >> 24) { right = x + 1; break; }
		}
		bmp.UnlockBits(&bd);
	}
	if (seen.size() > 4000) seen.clear();                  // speeds / gaps change all the time
	return seen[key] = (float)(right - ox);
}

// draws s with its ink starting at x, squeezed to sx of its width and sy of its height (kept on the same middle)
static void DrawSqueezed(Graphics &gr, const std::wstring &ws, const Font &font, const Brush &br, float x, float y,
                         float size, float sx, float sy) {
	StringFormat tf(StringFormat::GenericTypographic());
	tf.SetFormatFlags(tf.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
	GraphicsState st = gr.Save();
	gr.TranslateTransform(x, y + size * 0.6f * (1 - sy));
	gr.ScaleTransform(sx, sy);
	gr.DrawString(ws.c_str(), -1, &font, PointF(0, 0), &tf, &br);
	gr.Restore(st);
}
// Overlay text never runs past its room (the box w, else maxW from x) and never wraps: text that is too long
// is condensed sideways first, as TV graphics do, down to 72%, and only then made smaller as well.
static void FitText(Graphics &gr, const std::string &s, const FontFamily &fam, float size, int style, Color col,
                    float x, float y, StringAlignment align, float w, float maxW) {
	std::wstring ws(s.begin(), s.end());
	Font font(&fam, size, style, UnitPixel);
	SolidBrush br(col);
	StringFormat fmt; fmt.SetAlignment(align); fmt.SetFormatFlags(StringFormatFlagsNoWrap);
	float pad = size / 6;                               // GDI+ leaves this margin either side of DrawString text
	float room = (w > 0 ? w : maxW) - 2 * pad;
	if (room <= 0) {
		if (w > 0) gr.DrawString(ws.c_str(), -1, &font, RectF(x, y, w, size * 1.6f), &fmt, &br);
		else gr.DrawString(ws.c_str(), -1, &font, PointF(x, y), &fmt, &br);
		return;
	}
	// drawn the way it was measured (plain DrawString lays hinted text out a little wider than it measures)
	float ink = InkReach(ws, font), k = ink > room ? room / ink : 1;
	float sx = k, sy = std::min(1.f, k / 0.72f), used = ink * k;      // past 72% it gets shorter too
	float left = w <= 0 || align == StringAlignmentNear ? x + pad
	           : align == StringAlignmentCenter ? x + (w - used) / 2 : x + w - pad - used;
	DrawSqueezed(gr, ws, font, br, left, y, size, sx, sy);
}

static void Text(Graphics &gr, const std::string &s, float x, float y, float size, Color col, bool bold = true,
                 StringAlignment align = StringAlignmentNear, float w = 0, float maxW = 0) {
	FontFamily fam(L"Segoe UI");
	FitText(gr, s, fam, size, bold ? FontStyleBold : FontStyleRegular, col, x, y, align, w, maxW);
}

// a line of differently coloured pieces, ink from x, kept inside maxW: squeezed as one if it is too long
static void DrawRun(Graphics &gr, const std::vector<std::pair<std::string, Color>> &run, const FontFamily &fam, float size, int style,
                    float x, float y, float maxW) {
	Font font(&fam, size, style, UnitPixel);
	std::vector<std::wstring> ws;
	std::vector<float> adv;                 // each piece's letters, then its trailing spaces
	float total = 0;
	for (auto &p : run) {
		ws.emplace_back(p.first.begin(), p.first.end());
		size_t end = ws.back().find_last_not_of(L' ');
		float spaces = end == std::wstring::npos ? InkWidth(gr, ws.back(), font)
		             : end + 1 < ws.back().size() ? InkWidth(gr, ws.back().substr(end + 1), font) : 0;
		adv.push_back(InkReach(ws.back(), font) + spaces);
	}
	for (size_t i = 0; i + 1 < adv.size(); i++) total += adv[i];
	if (!ws.empty()) total += InkReach(ws.back(), font);
	float room = maxW;
	float k = total > room && total > 0 ? std::max(0.f, room) / total : 1, sx = k, sy = std::min(1.f, k / 0.72f);
	for (size_t i = 0; i < run.size(); i++) {
		SolidBrush br(run[i].second);
		DrawSqueezed(gr, ws[i], font, br, x, y, size, sx, sy);
		x += adv[i] * k;
	}
}

static const Color BG = C(215, 14, 16, 22), ACCENT = C(255, 255, 196, 0), WHITE = C(255, 255, 255, 255),
                   GREY = C(255, 170, 176, 190), RED = C(255, 235, 70, 60), GREEN = C(255, 60, 200, 110);

static std::string Ordinal(int n);
static float TextWidth(Graphics &gr, const std::string &s, float size, bool bold = true) {
	std::wstring ws(s.begin(), s.end());
	FontFamily fam(L"Segoe UI");
	Font font(&fam, size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
	RectF r;
	gr.MeasureString(ws.c_str(), -1, &font, PointF(0, 0), &r);
	return r.Width;
}

// one fact about the car's race: a label and pieces of coloured text
struct Fact { std::string label; std::vector<std::pair<std::string, Color>> parts; };
static void AddDelta(Fact &f, int from, int to) {
	int d = from - to;      // places gained
	if (d) f.parts.push_back({ (d > 0 ? "  +" : "  -") + std::to_string(abs(d)), Color(d > 0 ? 0xFF3CC86E : 0xFFEB463C) });
	else f.parts.push_back({ "  EVEN", Color(0xFFAAB0BE) });
}
static std::vector<Fact> StoryFacts(int i) {
	std::vector<Fact> out;
	const Color W(0xFFFFFFFF), G(0xFFAAB0BE);
	if (i < 0) {           // sample for edit mode
		Fact a{ "RACE", { { "STARTED 14TH   RUNNING 3RD", W } } }; AddDelta(a, 14, 3); out.push_back(a);
		return out;
	}
	const Car &c = g_car[i];
	if (c.startPos > 0) {
		Fact f{ "RACE", { { "STARTED " + Ordinal(c.startPos) + "   RUNNING " + Ordinal(c.spos), W } } };
		AddDelta(f, c.startPos, c.spos); out.push_back(f);
	}
	if (c.restartPos > 0 && g_greens >= 2) {
		Fact f{ "SINCE RESTART", { { "RESTARTED " + Ordinal(c.restartPos) + "   NOW " + Ordinal(c.spos), W } } };
		AddDelta(f, c.restartPos, c.spos); out.push_back(f);
	}
	Fact st{ "STAGES", {} };
	for (size_t s = 0; s < S.stageLaps.size() && s < 8; s++)
		if (c.stagePos[s]) {
			st.parts.push_back({ (st.parts.empty() ? "" : "     ") + std::string("S") + std::to_string(s + 1) + " ", G });
			st.parts.push_back({ Ordinal(c.stagePos[s]), W });
		}
	if (!st.parts.empty()) out.push_back(st);
	if (c.pits > 0 && c.pitPhase == 0) {
		Fact f{ "LAST PIT", { { "LAP " + std::to_string(c.pitLap), W } } };
		if (c.pitStall > 1) { char b[32]; snprintf(b, sizeof b, "   %.1fs STOP", c.pitStall); f.parts.push_back({ b, W }); }
		if (c.pitInPos && c.pitOutPos) {
			f.parts.push_back({ "   IN " + Ordinal(c.pitInPos) + "  OUT " + Ordinal(c.pitOutPos), G });
		}
		f.parts.push_back({ "   " + std::to_string(c.pits) + (c.pits == 1 ? " STOP" : " STOPS"), G });
		out.push_back(f);
	}
	if (c.lapsLed > 0) out.push_back({ "LAPS LED", { { std::to_string(c.lapsLed) + (c.lapsLed == 1 ? " LAP" : " LAPS"), W } } });
	return out;
}

static void DrawStory(Graphics &gr, bool sample) {
	if (!S.tagStory) return;
	auto facts = StoryFacts(sample ? -1 : OvCar());
	if (facts.empty()) return;
	// starts on the first fact at every cut, then steps through them
	size_t n = (size_t)(std::max(0.0, Now() - g_shot.since) / S.tagRotate) % facts.size();
	const Fact &f = facts[n];
	SolidBrush bg(C(225, 24, 27, 36)), acc(ACCENT);
	FillRound(gr, bg, 12, 98, 536, 26, 6);
	gr.FillRectangle(&acc, 12.f, 98.f, 4.f, 26.f);
	float lab = std::max(100.f, std::min(200.f, TextWidth(gr, f.label, 13) + 14));
	Text(gr, f.label, 24, 102, 13, ACCENT, true, StringAlignmentNear, 0, lab - 6);
	FontFamily fam(L"Segoe UI");
	DrawRun(gr, f.parts, fam, 13, FontStyleBold, 24 + lab + 13 / 6.f, 102, 548 - 10 - (24 + lab));
}

static void DrawTag(Graphics &gr, bool sample) {
	int i = sample ? -1 : OvCar();
	int number = sample ? 24 : g_car[i].number;
	std::string pos = sample ? "P3" : "P" + std::to_string(g_car[i].spos);
	std::string name = sample ? "JEFF GORDON" : g_car[i].first + " " + g_car[i].last;
	for (auto &ch : name) ch = (char)toupper((unsigned char)ch);
	std::string sub = sample ? "0.31s closing on Johnson" : OvReplay() ? "REPLAY"
	                         : (g_shot.kind == kBattle || g_shot.kind == kPass || g_shot.kind == kLeader) ? g_shot.why : "";
	auto col = g_colors.find(NumKey(number));
	SolidBrush bg(BG), acc(ACCENT), stripe(col != g_colors.end() ? col->second : ACCENT);
	FillRound(gr, bg, 0, 0, 560, 96, 10);
	FillRound(gr, acc, 0, 0, 96, 96, 10);
	gr.FillRectangle(&stripe, 96.f, 90.f, 464.f, 6.f);              // driver colour
	Text(gr, pos, 0, 18, 44, C(255, 20, 20, 20), true, StringAlignmentCenter, 96);
	float x = 104;
	Image *head = Headshot(number);
	if (head) x += DrawFit(gr, head, x, 2, 84, 88) + 8;
	Image *card = NumberArt(number);
	if (card) DrawFit(gr, card, x, 6, 80, 30);
	else Text(gr, "#" + NumLabel(number), x, 6, 22, ACCENT, true, StringAlignmentNear, 0, 110);
	Text(gr, name, x - 2, 36, 28, WHITE, true, StringAlignmentNear, 0, 556 - (x - 2));
	Text(gr, sub, x, 68, 14, GREY, false, StringAlignmentNear, 0, 552 - x);
	DrawStory(gr, sample);
}

// the battle box gap: median of the last ~1 s of readings for the pair on screen, so a
// single odd reading (a lap distance mid-update) cannot flash up
static double Steady(std::vector<double> &h, double v) {
	h.push_back(v);
	if (h.size() > 5) h.erase(h.begin());
	std::vector<double> s = h;
	std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
	return s[s.size() / 2];
}
static void PairGap(int top, int bottom, double &gap, double &metres) {
	static int ka = -2, kb = -2;
	static std::vector<double> hg, hm;
	if (std::min(top, bottom) != ka || std::max(top, bottom) != kb) { ka = std::min(top, bottom); kb = std::max(top, bottom); hg.clear(); hm.clear(); }
	gap = Steady(hg, fabs(Gap(top, bottom)));
	metres = Steady(hm, fabs(Ahead(top, bottom)));
}

// Who's on top in the battle box. Each driver keeps his row; the order changes only when the other car is
// clearly ahead on track (SWAP_M metres for SWAP_S seconds) - a nose-to-tail pair no longer flickers - and
// then the two rows slide past each other over SLIDE_S seconds.
static const double SWAP_M = 2.0, SWAP_S = 0.6, SLIDE_S = 0.45;
static bool g_boxSliding = false;
static void BattleOrder(int a, int b, int &top, int &bottom, double &slide) {
	static int ka = -2, kb = -2, shown = -1;
	static double since = -1, swapAt = -1;
	double now = Now();
	if (std::min(a, b) != ka || std::max(a, b) != kb) {         // a new pair: the better placed car on top
		ka = std::min(a, b); kb = std::max(a, b);
		shown = g_car[a].spos <= g_car[b].spos ? a : b;
		since = swapAt = -1;
	}
	int other = shown == a ? b : a;
	if (Ahead(other, shown) > SWAP_M) {
		if (since < 0) since = now;
		else if (now - since >= SWAP_S) { shown = other; swapAt = now; since = -1; }
	} else since = -1;
	top = shown; bottom = shown == a ? b : a;
	slide = swapAt < 0 ? 1.0 : std::min(1.0, (now - swapAt) / SLIDE_S);
	g_boxSliding = slide < 1.0;
}
// row heights during the slide: the car that just got by moves up from the lower row (eased)
static void SlideRows(double slide, float yUp, float yDown, float &yTop, float &yBottom) {
	double e = slide * slide * (3 - 2 * slide);
	yTop = (float)(yDown + (yUp - yDown) * e);
	yBottom = (float)(yUp + (yDown - yUp) * e);
}

static void DrawBattle(Graphics &gr, bool sample) {
	int a = sample ? -1 : g_shot.other, b = sample ? -1 : g_shot.car;
	SolidBrush bg(BG);
	FillRound(gr, bg, 0, 0, 420, 110, 10);
	// live: who is ahead on track (the tower changes only at the line), with a hold-off against flicker
	int top = a, bottom = b;
	double slide = 1.0;
	if (!sample) BattleOrder(a, b, top, bottom, slide);
	float yTop = 30, yBottom = 64;
	SlideRows(slide, 30, 64, yTop, yBottom);
	// official neighbours: the one ahead on track right now holds the better position
	int hi = sample ? 2 : std::min(g_car[a].spos, g_car[b].spos);
	bool neighbours = !sample && abs(g_car[a].spos - g_car[b].spos) == 1;
	auto livePos = [&](int slot) { return neighbours ? (slot == top ? hi : hi + 1) : g_car[slot].spos; };
	std::string title = (g_shot.kind == kPass && !sample ? "PASS FOR P" : "BATTLE FOR P") + std::to_string(hi);
	Text(gr, title, 14, 6, 14, ACCENT, true, StringAlignmentNear, 0, 392);
	auto row = [&](float y, int slot, const char *spos, const char *snum, const char *sname) {
		std::string p = slot >= 0 ? "P" + std::to_string(livePos(slot)) : spos;
		std::string n = slot >= 0 ? "#" + NumLabel(g_car[slot].number) : snum;
		std::string nm = slot >= 0 ? Short(slot) : sname;
		Text(gr, p, 14, y, 22, WHITE, true, StringAlignmentNear, 0, 58);
		Image *card = slot >= 0 ? NumberArt(g_car[slot].number) : nullptr;
		if (card) DrawFit(gr, card, 70, y + 2, 56, 28);
		else Text(gr, n, 70, y, 22, ACCENT, true, StringAlignmentNear, 0, 68);
		Text(gr, nm, 140, y, 22, WHITE, false, StringAlignmentNear, 0, 170);
	};
	row(yBottom, bottom, "P3", "#24", "Gordon");
	row(yTop, top, "P2", "#48", "Johnson");                 // drawn last: the car getting by goes over
	double gap = 0.31, metres = 24;
	if (!sample) PairGap(top, bottom, gap, metres);
	char g[32]; snprintf(g, sizeof g, "%.2fs", gap);
	Text(gr, g, 300, 34, 26, WHITE, true, StringAlignmentFar, 110);
	snprintf(g, sizeof g, "%.0f m", metres);
	Text(gr, g, 300, 68, 13, GREY, false, StringAlignmentFar, 110);
}

static void DrawOnboard(Graphics &gr, bool sample) {
	int i = sample ? -1 : OvCar();
	// speed from the car's velocity, like the dash (the car record's speed value reads about 1.3 times too high)
	float mph = sample ? 182 : OvReplay() ? OvRec(i, REC_MPH) / 1.30f : g_car[i].speed * 2.23694f;
	float rpm = sample ? 8650 : OvRec(i, REC_RPM);
	float thr = sample ? 0.9f : OvRec(i, REC_THROTTLE);
	SolidBrush bg(BG), acc(ACCENT), dark(C(255, 50, 54, 64)), grn(GREEN);
	FillRound(gr, bg, 0, 0, 300, 120, 10);
	char s[32];
	snprintf(s, sizeof s, "%.0f", mph);
	Text(gr, s, 10, 2, 54, WHITE, true, StringAlignmentFar, 150);
	Text(gr, "MPH", 164, 40, 16, GREY);
	snprintf(s, sizeof s, "%.0f RPM", rpm);
	Text(gr, s, 164, 14, 16, WHITE);
	float r = std::min(1.0f, std::max(0.0f, rpm / 10000.0f)), t = std::min(1.0f, std::max(0.0f, thr));
	gr.FillRectangle(&dark, 14, 80, 272, 10); gr.FillRectangle(&acc, 14.f, 80.f, 272 * r, 10.f);
	gr.FillRectangle(&dark, 14, 98, 272, 10); gr.FillRectangle(&grn, 14.f, 98.f, 272 * t, 10.f);
	Image *card = sample ? nullptr : NumberArt(g_car[i].number);
	if (card) DrawFit(gr, card, 214, 48, 72, 28, true);
	else Text(gr, "#" + (sample ? std::string("24") : NumLabel(g_car[i].number)), 230, 54, 16, ACCENT);
}

// ---- TV in-car dash (selectable with overlay_style = dash)
static std::wstring g_condensed;          // a condensed broadcast-style font, if installed

static void DashText(Graphics &gr, const std::string &s, float x, float y, float size, Color col,
                     bool bold = true, bool italic = true, StringAlignment align = StringAlignmentNear, float w = 0,
                     float maxW = 0) {
	FontFamily fam(g_condensed.c_str());
	FitText(gr, s, fam, size, (bold ? FontStyleBold : FontStyleRegular) | (italic ? FontStyleItalic : 0), col, x, y, align, w, maxW);
}

static std::string Ordinal(int n) {
	const char *suf = (n % 100 >= 11 && n % 100 <= 13) ? "TH" : n % 10 == 1 ? "ST" : n % 10 == 2 ? "ND" : n % 10 == 3 ? "RD" : "TH";
	return std::to_string(n) + suf;
}

static bool DashLook() { return _stricmp(S.overlayStyle.c_str(), "dash") == 0; }
// the car the dash panel being drawn is about (its colour from the art when dash_art_colors is on)
static const int NO_CAR_COLOR = -100000;
static int g_accentNum = NO_CAR_COLOR;
struct AccentFor {                               // while this lives, the dash colours are this car number's
	int was;
	explicit AccentFor(int num) : was(g_accentNum) { g_accentNum = num; }
	~AccentFor() { g_accentNum = was; }
};
static bool ArtColor(Color &c) {
	if (!S.dashArtColors || g_accentNum == NO_CAR_COLOR) return false;
	auto it = g_colors.find(NumKey(g_accentNum));
	if (it == g_colors.end()) return false;
	c = it->second;
	return true;
}
static Color MixWhite(Color c, float minLum) {   // dark colours lifted toward white so they show on the dark panel
	float l = (0.299f * c.GetR() + 0.587f * c.GetG() + 0.114f * c.GetB()) / 255;
	if (l >= minLum) return c;
	float t = (minLum - l) / std::max(0.01f, 1 - l);
	auto m = [&](BYTE v) { return (int)(v + (255 - v) * t + 0.5f); };
	return C(255, m(c.GetR()), m(c.GetG()), m(c.GetB()));
}
static Color Darker(Color c, float k) { return C(255, (int)(c.GetR() * k), (int)(c.GetG() * k), (int)(c.GetB() * k)); }
static Color DashAccent() {
	Color c;
	if (ArtColor(c)) return MixWhite(c, 0.42f);
	return C(255, (S.dashAccent >> 16) & 255, (S.dashAccent >> 8) & 255, S.dashAccent & 255);
}

// dark panel with the accent border
static void DashPanel(Graphics &gr, float w, float h) {
	SolidBrush bg(C(235, 10, 12, 16));
	Pen border(DashAccent(), 3);
	gr.FillRectangle(&bg, 0.f, 0.f, w, h);
	gr.DrawRectangle(&border, 1.5f, 1.5f, w - 3, h - 3);
}
// slanted accent block from x to x + w (wider at the top)
static void DashPlate(Graphics &gr, float x, float y, float w, float h, float slant) {
	GraphicsPath p;
	p.AddLine(x, y, x + w, y); p.AddLine(x + w, y, x + w - slant, y + h); p.AddLine(x + w - slant, y + h, x, y + h);
	p.CloseFigure();
	Color to = C(255, (S.dashAccent2 >> 16) & 255, (S.dashAccent2 >> 8) & 255, S.dashAccent2 & 255), from = DashAccent(), art;
	if (ArtColor(art)) { from = MixWhite(art, 0.12f); to = Darker(from, 0.35f); }   // the car's colour, fading darker
	LinearGradientBrush grad(PointF(x, y), PointF(x + w, y + h), from, to);
	gr.FillPath(&grad, &p);
}
// "12TH" with a small suffix, then an optional word under it
static void DashOrdinal(Graphics &gr, int n, float x, float y, float size, const char *under) {
	std::string ord = Ordinal(n);
	size_t cut = ord.size() - 2;
	DashText(gr, ord.substr(0, cut), x, y, size, WHITE);
	std::wstring digits(ord.begin(), ord.begin() + cut);
	FontFamily fam(g_condensed.c_str());
	Font font(&fam, size, FontStyleBold | FontStyleItalic, UnitPixel);
	RectF box;
	StringFormat tight(StringFormat::GenericTypographic());
	gr.MeasureString(digits.c_str(), -1, &font, PointF(0, 0), &tight, &box);
	DashText(gr, ord.substr(cut), x + box.Width + size * 0.24f, y + size * 0.12f, size * 0.44f, WHITE);
	if (under) DashText(gr, under, x + 2, y + size * 1.06f, size * 0.41f, WHITE);
}

static float DashWidth(Graphics &gr, const std::string &s, float size) {
	std::wstring ws(s.begin(), s.end());
	FontFamily fam(g_condensed.c_str());
	Font font(&fam, size, FontStyleBold | FontStyleItalic, UnitPixel);
	RectF r;
	gr.MeasureString(ws.c_str(), -1, &font, PointF(0, 0), &r);
	return r.Width;
}
// the race-story strip under a dash panel w wide
static void DrawStoryDash(Graphics &gr, bool sample, float w) {
	if (!S.tagStory) return;
	auto facts = StoryFacts(sample ? -1 : OvCar());
	if (facts.empty()) return;
	size_t n = (size_t)(std::max(0.0, Now() - g_shot.since) / S.tagRotate) % facts.size();
	const Fact &f = facts[n];
	SolidBrush bg(C(235, 10, 12, 16));
	gr.FillRectangle(&bg, 0.f, 112.f, w, 28.f);
	float plate = std::max(90.f, std::min(w * 0.4f, DashWidth(gr, f.label, 17) + 26));
	DashPlate(gr, 0, 112, plate, 28, 8);
	DashText(gr, f.label, 8, 114, 17, WHITE, true, true, StringAlignmentNear, 0, plate - 16);
	auto run = f.parts;
	for (auto &p : run) for (auto &ch : p.first) ch = (char)toupper((unsigned char)ch);
	FontFamily fam(g_condensed.c_str());
	float x = plate + 6 + 17 / 6.f;
	DrawRun(gr, run, fam, 17, FontStyleBold | FontStyleItalic, x, 114, w - 10 - x);
}

// driver tag, dash look: place block with the headshot, names, number card, why we are here
static void DrawTagDash(Graphics &gr, bool sample) {
	int i = sample ? -1 : OvCar();
	int number = sample ? 24 : g_car[i].number, place = sample ? 3 : g_car[i].spos;
	std::string first = sample ? "Jeff" : g_car[i].first, last = sample ? "Gordon" : g_car[i].last;
	for (auto &ch : first) ch = (char)toupper((unsigned char)ch);
	for (auto &ch : last) ch = (char)toupper((unsigned char)ch);
	std::string sub = sample ? "0.31s closing on Johnson"
	                         : (g_shot.kind == kBattle || g_shot.kind == kPass || g_shot.kind == kLeader) ? g_shot.why : "";
	for (auto &ch : sub) ch = (char)toupper((unsigned char)ch);
	AccentFor colour(number);
	DashPanel(gr, 520, 110);
	DashPlate(gr, 0, 0, 150, 110, 22);
	DashOrdinal(gr, place, 8, 0, 34, "PLACE");
	Image *head = Headshot(number);
	if (head) DrawFit(gr, head, 52, 14, 100, 96);
	DashText(gr, first, 160, 6, 18, WHITE, true, true, StringAlignmentNear, 0, 352);
	DashText(gr, last, 156, 24, 36, WHITE, true, true, StringAlignmentNear, 0, 356);
	Image *card = NumberArt(number);
	float x = 162;
	if (card) x += DrawFit(gr, card, x, 72, 56, 30) + 12;
	else { DashText(gr, NumLabel(number), x, 72, 22, DashAccent(), true, true, StringAlignmentNear, 0, 48); x += 50; }
	DashText(gr, sub, x, 78, 16, GREY, true, true, StringAlignmentNear, 0, 512 - x);
	auto col = g_colors.find(NumKey(number));
	if (col != g_colors.end()) { SolidBrush st(col->second); gr.FillRectangle(&st, 146.f, 104.f, 371.f, 3.f); }
	DrawStoryDash(gr, sample, 520);
}

// battle box, dash look: header plate, the two cars as they run on track, the gap
static void DrawBattleDash(Graphics &gr, bool sample) {
	int a = sample ? -1 : g_shot.other, b = sample ? -1 : g_shot.car;
	int top = a, bottom = b;
	double slide = 1.0;
	if (!sample) BattleOrder(a, b, top, bottom, slide);
	int hi = sample ? 2 : std::min(g_car[a].spos, g_car[b].spos);
	bool neighbours = !sample && abs(g_car[a].spos - g_car[b].spos) == 1;
	auto livePos = [&](int slot) { return neighbours ? (slot == top ? hi : hi + 1) : g_car[slot].spos; };
	float yTop = 38, yBottom = 80;
	SlideRows(slide, 38, 80, yTop, yBottom);
	AccentFor colour(sample ? 24 : g_car[b].number);       // the panel: the car on screen; each row's place: its own car
	DashPanel(gr, 460, 130);
	DashPlate(gr, 0, 0, 300, 30, 10);
	DashText(gr, std::string(g_shot.kind == kPass && !sample ? "PASS FOR " : "BATTLE FOR ") + Ordinal(hi), 12, 1, 22, WHITE,
	         true, true, StringAlignmentNear, 0, 274);
	auto row = [&](float y, int slot, int spos, int snum, const char *sname) {
		int p = slot >= 0 ? livePos(slot) : spos, num = slot >= 0 ? g_car[slot].number : snum;
		std::string nm = slot >= 0 ? Short(slot) : sname;
		for (auto &ch : nm) ch = (char)toupper((unsigned char)ch);
		{ AccentFor rowColour(num); DashText(gr, std::to_string(p), 10, y, 28, DashAccent(), true, true, StringAlignmentFar, 34); }
		Image *card = NumberArt(num);
		if (card) DrawFit(gr, card, 52, y + 3, 56, 30);
		else DashText(gr, NumLabel(num), 52, y, 24, WHITE, true, true, StringAlignmentNear, 0, 60);
		DashText(gr, nm, 116, y, 28, WHITE, true, true, StringAlignmentNear, 0, 200);
		auto col = g_colors.find(NumKey(num));
		if (col != g_colors.end()) { SolidBrush st(col->second); gr.FillRectangle(&st, 46.f, y + 6, 3.f, 28.f); }
	};
	row(yBottom, bottom, 3, 24, "Gordon");
	row(yTop, top, 2, 48, "Johnson");                       // drawn last: the car getting by goes over
	double gap = 0.31, metres = 24;
	if (!sample) PairGap(top, bottom, gap, metres);
	char v[32];
	snprintf(v, sizeof v, "%.2f", gap);
	DashText(gr, v, 320, 46, 36, WHITE, true, true, StringAlignmentCenter, 130);
	snprintf(v, sizeof v, "SEC  |  %.0f M", metres);
	DashText(gr, v, 320, 90, 14, GREY, true, true, StringAlignmentCenter, 130);
}

static void DrawDash(Graphics &gr, bool sample) {
	int i = sample ? -1 : OvCar();
	int number = sample ? 60 : g_car[i].number, place = sample ? 12 : g_car[i].spos;
	std::string first = sample ? "Ryan" : g_car[i].first, last = sample ? "Preece" : g_car[i].last;
	for (auto &ch : first) ch = (char)toupper((unsigned char)ch);
	for (auto &ch : last) ch = (char)toupper((unsigned char)ch);
	// speed from the car's velocity (the car record's speed value reads about 1.3 times too high)
	float mph = sample ? 190 : OvReplay() ? OvRec(i, REC_MPH) / 1.30f : g_car[i].speed * 2.23694f, rpm = sample ? 8275 : OvRec(i, REC_RPM);
	int gear = sample ? 4 : OvReplay() ? -9 : g_car[i].gear;
	if (mph < 0 || mph > 400) mph = 0;
	static std::map<int, float> topRpm;                  // highest RPM seen per car = the bar's end
	float &top = topRpm[sample ? -1 : i];
	top = std::max(top, std::max(rpm, 8000.0f));
	AccentFor colour(number);
	Color acc = C(255, (S.dashAccent >> 16) & 255, (S.dashAccent >> 8) & 255, S.dashAccent & 255);

	(void)acc;
	DashPanel(gr, 660, 110);
	DashPlate(gr, 0, 0, 150, 110, 22);          // place block, with the headshot standing in it
	DashOrdinal(gr, place, 8, 0, 34, "PLACE");
	Image *head = Headshot(number);
	if (head) DrawFit(gr, head, 52, 14, 100, 96);
	// driver: kept clear of the speed arc (its outer edge is at x = 366)
	DashText(gr, first, 160, 6, 18, WHITE, true, true, StringAlignmentNear, 0, 200);
	DashText(gr, last, 156, 24, 36, WHITE, true, true, StringAlignmentNear, 0, 204);
	Image *card = NumberArt(number);
	if (card) DrawFit(gr, card, 162, 72, 56, 30);
	else DashText(gr, NumLabel(number), 162, 72, 22, ACCENT, true, true, StringAlignmentNear, 0, 80);
	// speed arc: green to yellow, filled to the current speed
	float cx = 410, cy = 62, r = 38, from = 150, sweep = 240;
	Pen track(C(255, 52, 56, 64), 12);
	gr.DrawArc(&track, cx - r, cy - r, r * 2, r * 2, from, sweep);
	float frac = std::min(1.0f, std::max(0.0f, mph / (float)S.dashSpeedMax));
	const int segs = 24;
	for (int k = 0; k < segs && k < (int)(frac * segs + 0.5f); k++) {
		float t = k / (float)(segs - 1);
		Pen seg(C(255, (int)(90 + 165 * t), (int)(200 + 30 * t), 40), 12);
		gr.DrawArc(&seg, cx - r, cy - r, r * 2, r * 2, from + sweep * k / segs, sweep / segs + 0.6f);
	}
	char v[32];
	snprintf(v, sizeof v, "%.0f", mph);
	DashText(gr, v, cx - 50, cy - 14, 28, WHITE, true, true, StringAlignmentCenter, 100);
	DashText(gr, "MPH", cx - 50, 88, 14, WHITE, true, true, StringAlignmentCenter, 100);
	// RPM: segmented bar
	const int bars = 12;
	int lit = (int)(rpm / top * bars + 0.5f);
	for (int k = 0; k < bars; k++) {
		SolidBrush b(k < lit ? (k >= bars - 2 ? C(255, 235, 60, 40) : C(255, 255, 140, 30)) : C(255, 52, 56, 64));
		gr.FillRectangle(&b, 482.f + k * 9, 16.f, 6.f, 26.f);
	}
	snprintf(v, sizeof v, "%.0f", rpm);
	DashText(gr, v, 470, 46, 28, WHITE, true, true, StringAlignmentCenter, 125);
	DashText(gr, "RPM", 470, 88, 14, WHITE, true, true, StringAlignmentCenter, 125);
	// gear
	{
		SolidBrush sep(C(255, 52, 56, 64));
		gr.FillRectangle(&sep, 600.f, 14.f, 2.f, 82.f);
		std::string gt = gear == -9 ? "-" : gear < 0 ? "R" : gear == 0 ? "N" : std::to_string(gear);
		DashText(gr, gt, 602, 8, 56, gear > 0 ? WHITE : DashAccent(), true, true, StringAlignmentCenter, 56);
		DashText(gr, "GEAR", 602, 88, 14, WHITE, true, true, StringAlignmentCenter, 56);
	}
	DrawStoryDash(gr, sample, 660);
}

static void Render(Panel &p, bool show) {
	if (!show) { if (p.shown) { ShowWindow(p.hwnd, SW_HIDE); p.shown = false; } return; }
	int w = (int)(p.baseW * p.scale) + 4, h = (int)(p.baseH * p.scale) + 4;
	HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
	BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
	bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
	void *bits = nullptr;
	HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	HGDIOBJ old = SelectObject(mem, bmp);
	{
		Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, (BYTE *)bits);
		Graphics gr(&canvas);
		gr.SetSmoothingMode(SmoothingModeAntiAlias);
		gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
		gr.Clear(C(0, 0, 0, 0));
		gr.ScaleTransform(p.scale, p.scale);
		bool sample = g_edit && (g_shot.car < 0 || !g_car[g_shot.car].valid);
		if (&p == &g_tag) { if (DashLook() && !OvReplay()) DrawTagDash(gr, sample); else DrawTag(gr, sample); }
		else if (&p == &g_battle) { if (DashLook()) DrawBattleDash(gr, sample || g_shot.other < 0); else DrawBattle(gr, sample || g_shot.other < 0); }
		else if (&p == &g_dash) DrawDash(gr, sample);
		else if (&p == &g_qualBox) DrawQual(gr, sample);
		else if (&p == &g_alert) DrawAlert(gr, g_edit);
		else DrawOnboard(gr, sample);
		if (g_edit) { Pen pen(ACCENT, 2 / p.scale); pen.SetDashStyle(DashStyleDash); gr.DrawRectangle(&pen, 1.f, 1.f, p.baseW - 2.f, p.baseH - 2.f); }
	}
	POINT src = { 0, 0 }, dst = { p.x, p.y };
	SIZE sz = { w, h };
	BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
	UpdateLayeredWindow(p.hwnd, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
	SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, screen);
	if (!p.shown) { ShowWindow(p.hwnd, SW_SHOWNOACTIVATE); p.shown = true; }
	SetWindowPos(p.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static Panel *PanelOf(HWND h) { for (Panel *p : g_panels) if (p->hwnd == h) return p; return nullptr; }

static void SavePanel(Panel &p) {
	char v[64];
	snprintf(v, sizeof v, "%d, %d, %.2f", p.x, p.y, p.scale);
	SaveKey(std::string("overlay_") + p.key, v);
}

static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
	Panel *p = PanelOf(h);
	switch (m) {
	case WM_LBUTTONDOWN:
		if (g_edit) { ReleaseCapture(); SendMessage(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
		return 0;
	case WM_MOUSEWHEEL:
		if (g_edit && p) {
			EnterCriticalSection(&g_lock);
			p->scale = std::min(3.0f, std::max(0.4f, p->scale + (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 0.05f : -0.05f)));
			Render(*p, true); SavePanel(*p);
			LeaveCriticalSection(&g_lock);
		}
		return 0;
	case WM_MOVE:
	case WM_EXITSIZEMOVE:
		if (p && g_edit) {
			RECT r; GetWindowRect(h, &r); p->x = r.left; p->y = r.top;
			if (m == WM_EXITSIZEMOVE) { EnterCriticalSection(&g_lock); SavePanel(*p); LeaveCriticalSection(&g_lock); }
		}
		break;
	}
	return DefWindowProc(h, m, w, l);
}

static void SetEdit(bool on) {
	g_edit = on;
	for (Panel *p : g_panels) {
		LONG ex = GetWindowLong(p->hwnd, GWL_EXSTYLE);
		ex = on ? (ex & ~WS_EX_TRANSPARENT) : (ex | WS_EX_TRANSPARENT);
		SetWindowLong(p->hwnd, GWL_EXSTYLE, ex);
		if (!on && p->shown) SavePanel(*p);
	}
	Log(on ? "-- overlay edit mode: drag panels to move, mouse wheel to resize, Ctrl+F10 when done --\n"
	          : "-- overlay positions saved --\n");
}

// the dash look's panels are a different size
static void PanelSizes() {
	bool dash = _stricmp(S.overlayStyle.c_str(), "dash") == 0;
	g_tag.baseW = dash ? 520 : 560; g_tag.baseH = dash ? 140 : 126;
	g_battle.baseW = dash ? 460 : 420; g_battle.baseH = dash ? 130 : 110;
}

static void CreatePanels(HINSTANCE inst) {
	WNDCLASSA wc = {}; wc.lpfnWndProc = PanelProc; wc.hInstance = inst; wc.lpszClassName = "RaceVisionPanel";
	wc.hCursor = LoadCursor(NULL, IDC_SIZEALL);
	RegisterClassA(&wc);
	int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
	PanelSizes();
	g_tag.x = 40; g_tag.y = sh - 180;
	g_battle.x = 40; g_battle.y = sh - 310;
	g_onboard.x = sw - 340; g_onboard.y = sh - 180;
	g_dash.x = 40; g_dash.y = 30;
	g_alert.x = sw / 2 - 230; g_alert.y = 30;
	for (Panel *p : g_panels) {
		auto it = S.raw.find(std::string("overlay_") + p->key);
		if (it != S.raw.end()) {
			int x, y; float s;
			if (sscanf(it->second.c_str(), "%d , %d , %f", &x, &y, &s) == 3) { p->x = x; p->y = y; p->scale = s; }
		}
		p->hwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		                          "RaceVisionPanel", p->key, WS_POPUP, p->x, p->y, p->baseW, p->baseH, NULL, NULL, inst, NULL);
	}
}

static bool IsOnboard(int cam) {   // (camera 22 counts as the in-car camera it runs on)
	return std::find(S.onboard.begin(), S.onboard.end(), cam) != S.onboard.end() ||
	       (cam > 21 && std::find(S.onboard.begin(), S.onboard.end(), GameCam(cam)) != S.onboard.end());
}

// Overlays are rare: the companion apps already show a running order tower and their own pit / race control
// graphics, and most shots are better clean. The in-car dash shows on every on-board shot (in-car / roof
// cameras). The driver tag / battle box: green flag, a few laps into the run, not on board, and at most once
// every overlay_gap seconds. They only appear or disappear when the car or the camera changes (or the flag
// does), never in the middle of a shot.
// ---- qualifying delta (single-car / knockout qualifying in practice) ----
// pole's time at lap distance d: from pole's recorded best lap if we saw it, else spread evenly
static double PoleAt(int pole, int poleMs, float d) {
	const auto &tr = g_car[pole].qBestTrace;
	bool same = pole >= 0 && g_car[pole].qBestMs && abs(g_car[pole].qBestMs - poleMs) <= 5 && tr.size() > 10;
	if (!same) return g_trackLen > 0 ? poleMs * (double)d / g_trackLen : -1;
	if (d <= tr.front().first) return tr.front().second * (double)d / std::max(tr.front().first, 1.f);
	for (size_t i = 1; i < tr.size(); i++)
		if (tr[i].first >= d) {
			double f = (d - tr[i - 1].first) / std::max(tr[i].first - tr[i - 1].first, 0.01f);
			return tr[i - 1].second + f * (tr[i].second - tr[i - 1].second);
		}
	return tr.back().second + (poleMs - tr.back().second) * (double)(d - tr.back().first) /
	       std::max(g_trackLen - tr.back().first, 1.f);
}

// live delta of `car` against pole (ms, + = slower); false when it isn't on a timed lap
static bool QualDelta(int car, int &delta) {
	if (car < 0 || g_ko.board.empty() || !g_game || g_car[car].qLapStart < 0) return false;
	const Car &c = g_car[car];
	if (g_trackLen > 0 && c.dist < g_trackLen * 0.04f) return false;          // just crossed: too early
	int pole = g_ko.board[0].first, poleMs = g_ko.board[0].second;
	int nowMs = (int)(GameSec(*g_game) * 1000);
	double at = PoleAt(pole, poleMs, c.dist);
	if (at < 0) return false;
	delta = (int)((nowMs - c.qLapStart) - at);
	return true;
}

static void DrawQual(Graphics &gr, bool sample) {
	SolidBrush bg(BG);
	FillRound(gr, bg, 0, 0, 420, 96, 10);
	std::string head = "QUALIFYING";
	if (sample) head = "ROUND 2  -  4:12";
	else if (g_ko.mode == "knockout") {
		char b[64];
		if (g_ko.phase == "checkered") snprintf(b, sizeof b, "ROUND %d  -  CHECKERED FLAG", g_ko.round);
		else snprintf(b, sizeof b, "ROUND %d  -  %d:%02d", g_ko.round, (int)g_ko.timeLeft / 60, (int)g_ko.timeLeft % 60);
		head = b;
	} else head = "SINGLE-CAR QUALIFYING";
	Text(gr, head, 14, 4, 13, ACCENT, true, StringAlignmentNear, 0, 392);
	auto who = [&](float y, int slot, const char *pos, const char *num, const char *name) {
		Text(gr, pos, 14, y, 20, WHITE, true, StringAlignmentNear, 0, 46);
		Image *card = slot >= 0 ? NumberArt(g_car[slot].number) : nullptr;
		if (card) DrawFit(gr, card, 58, y + 2, 50, 26);
		else Text(gr, slot >= 0 ? "#" + NumLabel(g_car[slot].number) : num, 58, y, 20, ACCENT, true, StringAlignmentNear, 0, 60);
		Text(gr, slot >= 0 ? Short(slot) : name, 118, y, 20, WHITE, false, StringAlignmentNear, 0, 186);
	};
	char t[32];
	// row 1: pole
	if (sample || !g_ko.board.empty()) {
		int pole = sample ? -1 : g_ko.board[0].first;
		who(26, pole, "1.", "#54", "Gibbs");
		snprintf(t, sizeof t, "%.3f", sample ? 26.414 : g_ko.board[0].second / 1000.0);
		Text(gr, t, 300, 26, 20, WHITE, true, StringAlignmentFar, 110);
	} else Text(gr, "NO TIME YET", 14, 26, 18, GREY);
	// row 2: the car on screen - live delta, its lap just done, or out / in lap
	int car = sample ? -1 : g_shot.car;
	if (!sample && (car < 0 || !g_car[car].valid)) return;
	int pos = 0;
	if (!sample) for (size_t i = 0; i < g_ko.board.size(); i++) if (g_ko.board[i].first == car) pos = (int)i + 1;
	bool justDone = !sample && Now() - g_car[car].qLastLapAt < 6;
	int delta = 0;
	bool live = sample || QualDelta(car, delta);
	if (sample) delta = -41;
	std::string p = justDone && pos ? std::to_string(pos) + "." : "--";
	who(60, car, p.c_str(), "#77", "Hocevar");
	if (justDone) {
		snprintf(t, sizeof t, "%.3f", g_car[car].qLastLapMs / 1000.0);
		Text(gr, t, 300, 60, 20, WHITE, true, StringAlignmentFar, 110);
	} else if (live) {
		snprintf(t, sizeof t, "%+.3f", delta / 1000.0);
		SolidBrush box(delta <= 0 ? GREEN : RED);
		gr.FillRectangle(&box, 318.f, 60.f, 92.f, 28.f);
		Text(gr, t, 318, 60, 20, WHITE, true, StringAlignmentFar, 88);
	} else {
		unsigned s = g_car[car].aiState;
		Text(gr, s == AI_INLAP_S || s == AI_INLAP_F ? "IN LAP" : "OUT LAP", 300, 62, 16, GREY, true, StringAlignmentFar, 110);
	}
}

// The crash alert, top of the screen: a caution sign when a crash worth a replay has just happened (and whether its
// replay is coming, waits for the caution, or is saved for later - while you drive, say), counting down to an
// automatic replay; during the replay the REPLAY bug with the car, the angle and the time until it's back to live.
struct AlertInfo { int kind = 0; std::string title, line, right; };
static AlertInfo AlertNow() {
	AlertInfo a;
	double now = Now();
	char b[64];
	if (g_rp.phase) {
		Crash *c = CrashById(g_rp.crash);
		a.kind = 2; a.title = g_rp.reel ? "HIGHLIGHTS" : "REPLAY";
		a.line = g_rp.car >= 0 ? "#" + NumLabel(g_car[g_rp.car].number) + " " + Short(g_rp.car) : (c ? CrashWho(*c, 3) : "");
		if (g_rp.takes.size() > 1) a.line += "  -  angle " + std::to_string(g_rp.take + 1) + " of " + std::to_string(g_rp.takes.size());
		double left = g_rp.at + g_rp.len - now;
		for (size_t k = g_rp.take + 1; k < g_rp.takes.size(); k++) left += g_rp.takes[k].len;
		snprintf(b, sizeof b, g_reel.empty() ? "LIVE IN %d" : "NEXT IN %d", std::max(0, (int)ceil(left)));
		a.right = b;
		if (g_tpManual) { a.right = g_rp.speedTxt; a.line = g_rp.car >= 0 ? "#" + NumLabel(g_car[g_rp.car].number) + " " + Short(g_rp.car) : ""; }
		else if (!g_rp.speedTxt.empty() && g_rp.speedTxt != "PLAY" && g_rp.speedTxt != "LIVE") a.right = g_rp.speedTxt;
		return a;
	}
	if (g_plan.crash >= 0) {
		if (Crash *c = CrashById(g_plan.crash)) {
			a.kind = 1; a.title = "CRASH"; a.line = CrashWho(*c, 3);
			snprintf(b, sizeof b, "REPLAY IN %d", std::max(0, (int)ceil(g_plan.at - now)));
			a.right = b;
			return a;
		}
	}
	if (now < g_remindUntil && g_youDrive) {
		if (Crash *c = CrashById(g_remindCrash)) {
			if (!c->replayed) { a.kind = 1; a.title = "REPLAY READY"; a.line = CrashWho(*c, 3); a.right = "CTRL+F7 TO WATCH - THE RACE PAUSES"; return a; }
		}
	}
	for (int k = (int)g_crashes.size() - 1; k >= 0; k--) {
		const Crash &c = g_crashes[k];
		if (c.worthyAt < 0 || now - c.worthyAt > 8) continue;
		a.kind = 1; a.title = "CRASH"; a.line = CrashWho(c, 3);
		a.right = c.replayed ? "" : g_youDrive && S.replayDrive ? "CTRL+F7 TO WATCH NOW" : g_youDrive || !S.replayAuto ? "REPLAY SAVED - REPLAYS LIST" :
		          S.replayCautionOnly && g_flag == FLAG_GREEN ? "REPLAY UNDER THE CAUTION" : "REPLAY COMING";
		return a;
	}
	return a;
}
static void DrawAlert(Graphics &gr, bool sample) {
	AlertInfo a = AlertNow();
	if (sample && !a.kind) { a.kind = 1; a.title = "CRASH"; a.line = "#24 Byron, #10 Dillon, #43 Jones +4"; a.right = "REPLAY IN 4"; }
	if (!a.kind) return;
	SolidBrush bg(BG);
	FillRound(gr, bg, 0, 0, 460, 100, 10);
	if (a.kind == 1) {                               // caution sign
		SolidBrush y(C(255, 255, 204, 0)), k(C(255, 20, 20, 20));
		PointF tri[3] = { PointF(50, 12), PointF(90, 86), PointF(10, 86) };
		gr.FillPolygon(&y, tri, 3);
		Text(gr, "!", 10, 30, 40, C(255, 20, 20, 20), true, StringAlignmentCenter, 80);
	} else {                                         // REPLAY bug: a play mark on red
		SolidBrush r(RED), w(WHITE);
		FillRound(gr, r, 10, 12, 80, 76, 8);
		PointF tri[3] = { PointF(38, 30), PointF(70, 50), PointF(38, 70) };
		gr.FillPolygon(&w, tri, 3);
	}
	Text(gr, a.title, 106, 6, 26, a.kind == 1 ? ACCENT : WHITE, true, StringAlignmentNear, 0, 340);
	Text(gr, a.line, 106, 42, 17, WHITE, false, StringAlignmentNear, 0, 340);
	if (!a.right.empty()) Text(gr, a.right, 106, 68, 15, a.kind == 1 ? ACCENT : GREY, true, StringAlignmentNear, 0, 340);
}

// RaceVision's overlays go and come back together with the companion apps' (hide_companion_overlays): all of them off the
// moment a wipe starts (TransitionStart), and the live ones back only when the other apps' are (or, with that setting
// off, half a second after the wipe back to live) - one change on screen, not two. The replay's own tag / dash / banner
// show while the tape rolls between wipes.
static bool WipeBusy();
static bool CompanionsAway();
static double g_ovBusyAt = -100;
static void OvHideAll() { for (Panel *p : g_panels) Render(*p, false); }
static bool OvHold() {
	if (g_edit) return false;
	if (WipeBusy()) { g_ovBusyAt = Now(); return true; }
	if (g_rp.phase) return false;
	return CompanionsAway() || Now() - g_ovBusyAt < 0.5;
}
static void UpdateOverlays() {
	static int seenCar = -2, seenCam = -2, seenFlag = -2;
	if (OvHold()) { OvHideAll(); return; }
	static bool on = false;
	static bool tag = false, box = false, gauges = false;
	static double lastTag = -1e9;
	if (OvReplay() && !g_edit) {                     // a replay: the classic tag on its car; in-car angles get the dash
		bool on = g_overlaysOn && S.replayOverlays && g_car[g_rp.car].valid;
		bool inCar = on && IsOnboard(g_rp.cam);
		bool dashStyle = _stricmp(S.overlayStyle.c_str(), "dash") == 0;
		Render(g_tag, on && !inCar);
		Render(g_battle, false);
		Render(g_onboard, inCar && !dashStyle);
		Render(g_dash, inCar && dashStyle);
		Render(g_alert, S.replayBanner);              // it's a replay, not live (user, 2026-10-28)
		return;
	}
	bool live = g_overlaysOn && g_shot.car >= 0 && g_car[g_shot.car].valid && g_session == SESSION_RACE && !g_rp.phase && !g_youDrive;
	bool race = live && g_flag == FLAG_GREEN;
	if (g_shot.car != seenCar || g_lastCam != seenCam || g_flag != seenFlag || on != g_overlaysOn || !live) {
		seenCar = g_shot.car; seenCam = g_lastCam; seenFlag = g_flag; on = g_overlaysOn;
		bool battleShot = (g_shot.kind == kBattle || g_shot.kind == kPass) && g_shot.other >= 0 && g_car[g_shot.other].valid;
		bool onboard = IsOnboard(g_lastCam);
		bool deepRun = g_leaderLaps - g_greenLap >= S.overlayMinRunLaps;
		gauges = live && onboard;                             // every on-board shot
		bool want = race && !onboard && deepRun && g_shot.kind != kManual;
		bool due = Now() - lastTag >= S.overlayGap;
		tag = want && due;
		box = tag && battleShot;
		if (tag) lastTag = Now();
	}
	bool otherOk = g_shot.other >= 0 && g_car[g_shot.other].valid;
	bool dashStyle = _stricmp(S.overlayStyle.c_str(), "dash") == 0;
	bool dash = dashStyle && gauges;                 // on board: the dash replaces the tag and the gauges
	Render(g_tag, g_edit || (tag && !dash));
	Render(g_battle, g_edit || (box && otherOk));
	Render(g_onboard, (g_edit && !dashStyle) || (gauges && !dashStyle));
	Render(g_dash, (g_edit && dashStyle) || dash);
	Render(g_alert, g_edit || (S.replayBanner && g_rp.phase) || (g_overlaysOn && g_session == SESSION_RACE && AlertNow().kind != 0));
	// qualifying in practice: pole + the on-screen car's live delta, redrawn every update
	ReadKo();
	bool qual = g_overlaysOn && g_session == SESSION_PRACTICE && g_ko.on && g_shot.car >= 0 &&
	            g_car[g_shot.car].valid && (g_ko.phase == "running" || g_ko.phase == "checkered");
	(void)qual;                                     // the qualifying timing box belongs to Race Control (user, 2026-10-20)
}

// ---------------------------------------------------------------- crowd
// Sound files are loaded once into memory and played through XAudio2 (part of Windows),
// each reaction on its own voice so they can overlap; the game's sound is not touched.
struct CrowdClip { WAVEFORMATEX fmt = {}; std::vector<BYTE> data; std::string name; };
static IXAudio2 *g_xa = nullptr;
static IXAudio2MasteringVoice *g_master = nullptr;
static std::map<std::string, std::vector<CrowdClip>> g_clips;      // kind -> its sound files
static std::vector<IXAudio2SourceVoice *> g_voices;
struct CrowdLater { double at; std::string kind; float gain; };
static std::vector<CrowdLater> g_later;

// a PCM (16 / 24 / 32 bit) or float .wav
static bool LoadWav(const std::string &p, CrowdClip &c) {
	std::string f = ReadAll(p);
	if (f.size() < 44 || f.compare(0, 4, "RIFF") || f.compare(8, 4, "WAVE")) return false;
	bool fmt = false;
	for (size_t i = 12; i + 8 <= f.size();) {
		DWORD len; memcpy(&len, &f[i + 4], 4);
		if (i + 8 + len > f.size()) len = (DWORD)(f.size() - i - 8);
		if (!f.compare(i, 4, "fmt ") && len >= 16) {
			memcpy(&c.fmt, &f[i + 8], 16);
			if (c.fmt.wFormatTag == 0xFFFE && len >= 40) {          // WAVE_FORMAT_EXTENSIBLE: take the sub format
				WORD sub; memcpy(&sub, &f[i + 8 + 24], 2); c.fmt.wFormatTag = sub;
			}
			c.fmt.cbSize = 0;
			fmt = c.fmt.wFormatTag == WAVE_FORMAT_PCM || c.fmt.wFormatTag == 3;
		} else if (!f.compare(i, 4, "data")) c.data.assign(f.begin() + i + 8, f.begin() + i + 8 + len);
		i += 8 + len + (len & 1);
	}
	return fmt && !c.data.empty();
}

static std::string ExeDir() {
	std::string d = g_ini;
	return d.substr(0, d.find_last_of('\\'));
}

static void CrowdInit() {
	if (!S.crowd) return;
	std::string dir = S.crowdFolder;
	if (dir.size() < 2 || (dir[1] != ':' && dir[0] != '\\')) dir = ExeDir() + "\\" + dir;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "\\*.wav").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			std::string name = fd.cFileName, kind;
			for (char ch : name) { if (!isalpha((unsigned char)ch)) break; kind += (char)tolower((unsigned char)ch); }
			CrowdClip c; c.name = name;
			if (!kind.empty() && LoadWav(dir + "\\" + name, c)) g_clips[kind].push_back(std::move(c));
			else Log("Crowd: could not read %s\n", name.c_str());
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	if (g_clips.empty()) { Log("Crowd: no sounds in %s\n", dir.c_str()); return; }
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(g_xa->CreateMasteringVoice(&g_master))) {
		Log("Crowd: no sound device\n");
		if (g_xa) g_xa->Release();
		g_xa = nullptr; return;
	}
	std::string list;
	for (auto &kv : g_clips) list += " " + kv.first + " x" + std::to_string(kv.second.size());
	Log("Crowd: on, volume %.2f -%s\n", S.crowdVolume, list.c_str());
}

static void CrowdPlay(const std::string &kind, float gain) {
	if (!g_xa) return;
	auto it = g_clips.find(kind);
	if (it == g_clips.end() || it->second.empty()) return;
	static std::map<std::string, size_t> last;
	auto &v = it->second;
	size_t n = rand() % v.size();
	if (v.size() > 1 && last.count(kind) && n == last[kind]) n = (n + 1) % v.size();   // not the same one twice
	last[kind] = n;
	const CrowdClip &c = v[n];
	IXAudio2SourceVoice *sv = nullptr;
	if (FAILED(g_xa->CreateSourceVoice(&sv, &c.fmt))) return;
	XAUDIO2_BUFFER b = {};
	b.AudioBytes = (UINT32)c.data.size(); b.pAudioData = c.data.data(); b.Flags = XAUDIO2_END_OF_STREAM;
	sv->SubmitSourceBuffer(&b);
	sv->SetVolume((float)(gain * S.crowdVolume));
	sv->Start();
	g_voices.push_back(sv);
	while (g_voices.size() > 6) { g_voices.front()->DestroyVoice(); g_voices.erase(g_voices.begin()); }
	SYSTEMTIME st; GetLocalTime(&st);
	Log("[%02d:%02d:%02d] crowd    %s (%.2f)\n", st.wHour, st.wMinute, st.wSecond, c.name.c_str(), gain);
}

static bool CrowdOn(const char *ev) {
	std::string l = "," + S.crowdEvents + ",";
	for (auto &ch : l) if (ch == ' ') ch = ',';
	return l.find(std::string(",") + ev + ",") != std::string::npos;
}
static bool Favourite(int slot) {
	return slot >= 0 && std::find(S.crowdFavourites.begin(), S.crowdFavourites.end(), g_car[slot].number) != S.crowdFavourites.end();
}

// what the crowd reacts to: the green flag, passes up front (the lead most), crashes, the
// white flag and the win; favourites get the biggest cheers and the groans
static void Crowd() {
	double now = Now();
	// finished voices go; delayed reactions start
	for (size_t i = 0; i < g_voices.size();) {
		XAUDIO2_VOICE_STATE st; g_voices[i]->GetState(&st);
		if (st.BuffersQueued == 0) { g_voices[i]->DestroyVoice(); g_voices.erase(g_voices.begin() + i); } else i++;
	}
	for (size_t i = 0; i < g_later.size();) {
		if (now >= g_later[i].at) { CrowdPlay(g_later[i].kind, g_later[i].gain); g_later.erase(g_later.begin() + i); } else i++;
	}
	if (!g_xa || g_session != SESSION_RACE || g_score.empty()) return;

	static int race = -1;
	static double seenPass = -1, lastPop = -100, lastOoh = -100;
	static bool white = false, win = false;
	static double seenIncident[NUM_SLOTS];
	if (race != g_raceNo) {        // a new race: nothing from before counts
		race = g_raceNo; seenPass = g_passAt; white = win = false;
		for (int i = 0; i < NUM_SLOTS; i++) seenIncident[i] = g_car[i].incidentAt;
		g_later.clear();
	}
	// green flag
	if (g_greenNow) {
		if (g_greens <= 1 && CrowdOn("start")) CrowdPlay("roar", 1.0f);
		else if (g_greens > 1 && CrowdOn("restart")) CrowdPlay("roar", 0.8f);
	}
	// passes: g_passCar got by g_passVictim for the victim's spot
	if (g_passAt != seenPass) {
		seenPass = g_passAt;
		int by = g_passCar, victim = g_passVictim, pos = victim >= 0 ? g_car[victim].spos : 0;
		if (by >= 0 && pos == 1 && CrowdOn("lead")) {
			CrowdPlay("roar", Favourite(by) ? 1.0f : 0.85f);
			g_later.push_back({ now + 0.8, "cheer", Favourite(by) ? 0.9f : 0.6f });
			lastPop = now;
		} else if (by >= 0 && pos > 1 && pos <= S.crowdPassDepth && CrowdOn("pass")) {
			if (Favourite(by)) { CrowdPlay("cheer", 0.8f); lastPop = now; }
			else if (Favourite(victim)) { CrowdPlay("ahh", 0.5f); lastPop = now; }
			else if (now - lastPop > S.crowdCooldown) {
				double up = 1 - (pos - 2) / (double)std::max(1, S.crowdPassDepth - 2);   // 2nd loudest
				CrowdPlay("pop", (float)(0.25 + 0.45 * up));
				lastPop = now;
			}
		}
	}
	// crashes: an ooh, louder the further up and the more cars; a groan after for the
	// leader, the top 3 or a favourite
	int crashed = 0, best = 99;
	bool fav = false;
	for (int i : g_order) {
		Car &c = g_car[i];
		if (c.incidentAt == seenIncident[i]) continue;
		seenIncident[i] = c.incidentAt;
		if (c.incidentAt < 0 || strcmp(c.incidentWhy, "CRASH") != 0) continue;
		crashed++; fav |= Favourite(i);
		if (c.spos > 0 && c.spos < best) best = c.spos;
	}
	if (crashed && CrowdOn("crash") && now - lastOoh > 3) {
		lastOoh = now;
		float gain = (float)std::min(1.0, 0.5 + 0.35 * PosWeight(best) + 0.1 * (crashed - 1) + (fav ? 0.2 : 0));
		CrowdPlay("ooh", gain);
		if (best <= 3 || fav) g_later.push_back({ now + 1.6, "ahh", gain * 0.8f });
	}
	// white flag and the win
	if (g_lapsTotal > 0) {
		if (!white && g_leaderLaps == g_lapsTotal - 1 && g_flag == FLAG_GREEN) {
			white = true;
			if (CrowdOn("white")) CrowdPlay("roar", 0.7f);
		}
		if (!win && g_leaderLaps >= g_lapsTotal) {
			win = white = true;
			if (CrowdOn("win")) {
				CrowdPlay("roar", 1.0f);
				g_later.push_back({ now + 1.0, "cheer", Favourite(g_score[0]) ? 1.0f : 0.85f });
			}
		}
	}
}

// Ctrl+F8: the next kind of sound, for setting the volume
static void CrowdTest() {
	if (!g_xa) { Log("-- crowd is off or has no sounds --\n"); return; }
	static size_t n = 0;
	std::vector<std::string> kinds;
	for (auto &kv : g_clips) kinds.push_back(kv.first);
	CrowdPlay(kinds[n++ % kinds.size()], 1.0f);
}

#include "racevision_cams.cpp"
#include "racevision_trackcams.cpp"
#include "racevision_ui.cpp"
#include "racevision_timeline.cpp"
#include "racevision_camsets.cpp"
#include "racevision_rig.cpp"
#include "racevision_dyncams.cpp"
#include "racevision_tape.cpp"
#include "racevision_highlights.cpp"
#include "racevision_remote.cpp"

// ---------------------------------------------------------------- main
// The game's frame rate, from the camera hook's frame counter (test, 2026-10-26: some cameras sometimes drop NR2003 to
// a few fps with the picture pulsing darker - on the TV cameras while cycling them, and in replays). A drop under
// 15 fps is logged with the camera, the car it follows and where that car is on the lap (the TV camera in use is
// the one whose stretch covers that spot), and again when it is back.
static void FpsWatch(Game &g) {
	static double t0 = -1, lowSince = -1;
	static DWORD f0 = 0;
	static int lowest = 0;
	if (!g.hooked && !(g.camOk && g.InstallHook())) return;
	double now = Now();
	DWORD f = g.Get<DWORD>(g.mbox + 12, 0);
	if (t0 < 0 || now - t0 > 5) { t0 = now; f0 = f; return; }
	if (now - t0 < 1.0) return;
	int fps = (int)((f - f0) / (now - t0) + 0.5);
	t0 = now; f0 = f;
	if (fps == 0) { lowSince = -1; return; }                // menus, paused, minimised: no frames at all
	auto where = [&]() {
		DWORD rec = g.Get<DWORD>(FOCUS_ADDR, 0);
		int idx = -1; bool rp = false;
		if (rec >= REC0 && (rec - REC0) % REC_SIZE == 0 && (rec - REC0) / REC_SIZE < NUM_SLOTS) idx = (int)((rec - REC0) / REC_SIZE);
		else if (rec >= REPLAY_REC0 && (rec - REPLAY_REC0) % REC_SIZE == 0 && (rec - REPLAY_REC0) / REC_SIZE < NUM_SLOTS) { idx = (int)((rec - REPLAY_REC0) / REC_SIZE); rp = true; }
		int cam = g.Camera();
		char b[200];
		snprintf(b, sizeof b, "camera %d (%s), %s%s at %.0f m of the lap%s", cam, cam >= 0 ? CamName(cam) : "?",
		         idx >= 0 ? Name(idx).c_str() : "?", rp ? " (replay)" : "", idx >= 0 ? g_car[idx].dist : -1.0f,
		         g.Get<int>(RP_MODE, 0) == 1 ? ", the replay is playing" : "");
		return std::string(b);
	};
	SYSTEMTIME st; GetLocalTime(&st);
	if (fps < 15 && lowSince < 0) {
		lowSince = now; lowest = fps;
		Log("[%02d:%02d:%02d] LOW FPS  %d fps - %s\n", st.wHour, st.wMinute, st.wSecond, fps, where().c_str());
	} else if (lowSince >= 0) {
		lowest = std::min(lowest, fps);
		if (fps >= 25) {
			Log("[%02d:%02d:%02d] FPS OK   %d fps again after %.0fs (lowest %d) - %s\n", st.wHour, st.wMinute, st.wSecond, fps,
			    now - lowSince, lowest, where().c_str());
			lowSince = -1;
		}
	}
}

int main() {
	setvbuf(stdout, NULL, _IONBF, 0);      // log lines show at once, also when redirected
	InitializeCriticalSection(&g_lock);
	InitializeCriticalSection(&g_logLock);
	SetProcessDPIAware();
	SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
	GetModuleFileNameA(NULL, g_ini, MAX_PATH);
	char *slash = strrchr(g_ini, '\\');
	strcpy(slash ? slash + 1 : g_ini, "racevision.ini");
	// first start as RaceVision: keep the overlay positions placed under the old name, but
	// not its timing (the old defaults cut far too often)
	std::vector<std::pair<std::string, std::string>> carry;
	char old[MAX_PATH]; strcpy(old, g_ini);
	strcpy(strrchr(old, '\\') + 1, "tv_director.ini");
	if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES) {
		FILE *f = fopen(old, "r");
		char line[512];
		while (f && fgets(line, sizeof line, f)) {
			std::string l = Trim(line);
			size_t eq = l.find('=');
			if (l.rfind("overlay_", 0) == 0 && eq != std::string::npos)
				carry.push_back({ Trim(l.substr(0, eq)), Trim(l.substr(eq + 1)) });
		}
		if (f) fclose(f);
	}
	LoadSettings();
	for (auto &kv : carry) SaveKey(kv.first, kv.second);
	// settings added after the ini was written: add them with their defaults so they can be edited
	char num[32];
	std::vector<std::pair<std::string, std::string>> added = {
		{ "short_track_m", (snprintf(num, sizeof num, "%.0f", S.shortTrackM), num) },
		{ "battle_short_m", (snprintf(num, sizeof num, "%.0f", S.battleShortM), num) },
		{ "onboard_hold", (snprintf(num, sizeof num, "%.0f", S.onboardHold), num) },
		{ "hide_companion_overlays", "0" }, { "companion_apps", "" }, { "companion_keep", "" },
		{ "replay_wipe_color", (snprintf(num, sizeof num, "#%06X", S.dashAccent & 0xFFFFFF), num) },   // (starts as the colour the wipe already had)
		{ "lead_battle_depth", "3" }, { "lead_battle_hold", "120" }, { "lead_battle_pack", "6" },
		{ "art_folders", S.artFolders }, { "companion_folder", S.companionFolder }, { "art_series", S.artSeries },
		{ "camera_call", "1" }, { "overlay_style", S.overlayStyle }, { "dash_accent", "#3CC8C8" }, { "dash_accent2", "#145A60" }, { "dash_art_colors", "0" }, { "dash_speed_max", "220" },
		{ "tag_story", "1" }, { "tag_rotate", "6" }, { "stage_laps", "" },
		{ "crowd", "0" }, { "crowd_volume", "0.8" }, { "crowd_folder", "sounds\\crowd" }, { "crowd_favourites", "" },
		{ "crowd_pass_depth", "10" }, { "crowd_cooldown", "6" }, { "crowd_events", "start, restart, lead, pass, crash, white, win" },
		{ "overlay_gap", "150" }, { "qual_player_cam", "1" },
		{ "cam_replay", "19, 20" }, { "replay_auto", "0" }, { "replay_caution_only", "1" }, { "replay_preroll", "4" },
		{ "replay_after", "8" }, { "replay_max", "20" }, { "replay_within", "120" }, { "live_after_climb_out", "1" },
		{ "crash_damage", "0.5" }, { "contact_join", "0.05" }, { "crash_span", "20" }, { "custom_cams", "1" }, { "chase2_view", "4.5, -2.2, 1.0, 26, 4" },
		{ "far_chase2_view", "15, 0, 7.5, 0, 20" }, { "incar3_eye", "-0.25, -0.5, 0.05" },
		{ "drone_height", "30" }, { "drone_quiet", "1" }, { "drone_distance", "100" }, { "drone_top_mph", "175" }, { "drone_hover_m", "25" }, { "rear_pan", "1" }, { "rear_pan_span", "110" }, { "rear_pan_fov", "105" },
		{ "rear_mount", "1" }, { "rear_mount_m", "45" }, { "rear_hold_m", "70" }, { "rear_pass_m", "5" }, { "rear_block_s", "1.5" }, { "rear_pan_smooth", "0.45" }, { "replay_overlays", "1" }, { "replay_banner", "1" }, { "replay_smart", "1" }, { "replay_slow_max", "5" }, { "replay_transition", "1" }, { "replay_transition_ms", "1500" }, { "rear_alone", "11" }, { "replay_worthy", "1.5" }, { "replay_angles", "3" },
		{ "replay_take", "8" }, { "replay_countdown", "5" }, { "reel_angles", "1" }, { "cam_replay_angles", "16, 5, 21" },
		{ "replay_drive", "1" }, { "highlights", "1" }, { "multi_pass_cars", "2" }, { "highlight_gap", "180" },
		{ "bump_run_window", "3" }, { "bump_run_depth", "10" }, { "superspeedways", "daytona; talladega" }, { "replay_slow_impact", "1.0" }, { "highlight_bump", "1" }, { "highlight_preroll", "6" }, { "replay_finish", "1" },
		{ "replay_finish_after", "6" }, { "reel_highlights", "1" }, { "tv_cut_gap", "1.5" }, { "tv_cut_wait", "4" },
		{ "dyn_cams", "0" }, { "dyn_pit", "1" }, { "dyn_crash", "1" }, { "dyn_restart", "1" }, { "dyn_battle", "1" }, { "dyn_finish", "1" },
		{ "dyn_learn", "1" }, { "dyn_chance", "60" }, { "dyn_half_width", "0" }, { "dyn_yaw_fix", "0" }, { "dyn_pitch_sign", "1" },
		{ "dyn_rig", "1" }, { "dyn_autozoom", "1" }, { "dyn_smooth", "1" } };
	for (auto &kv : added) if (!S.raw.count(kv.first)) SaveKey(kv.first, kv.second);
	if (!S.cams.count("replay")) S.cams["replay"] = IntList(S.raw["cam_replay"]);
	if (!S.cams.count("replay_angles")) S.cams["replay_angles"] = IntList(S.raw["cam_replay_angles"]);
	if (!carry.empty()) { LoadSettings(); Log("Overlay positions carried over from tv_director.ini.\n"); }
	g_overlaysOn = S.overlays;

	GdiplusStartupInput gin; ULONG_PTR gtok;
	GdiplusStartup(&gtok, &gin, NULL);
	CreatePanels(GetModuleHandle(NULL));
	ArtRoots();
	for (const wchar_t *f : { L"Bahnschrift SemiBold Condensed", L"Bahnschrift Condensed", L"Bahnschrift", L"Segoe UI" }) {
		FontFamily fam(f);
		if (fam.GetLastStatus() == Ok) { g_condensed = f; break; }
	}
	if (__argc > 2 && strcmp(__argv[1], "--selftest") == 0) {    // format checks, no game needed: results to the file
		FILE *o = fopen(__argv[2], "w");
		if (o) { CsSelfTest(o); std::string tr = __argc > 3 ? __argv[3] : "bristol_night_RR"; DynSelfTest(o, tr); RigSelfTest(o); HlSelfTest(o); if (__argc > 4) CsDumpSamples(__argv[4], tr); fclose(o); }
		GdiplusShutdown(gtok);
		return 0;
	}
	if (__argc > 2 && strcmp(__argv[1], "--wipe") == 0) {         // a contact sheet of the replay wipe, no game needed
		WipeTest(__argv[2], __argc > 3 ? atoi(__argv[3]) : 0);
		GdiplusShutdown(gtok);
		return 0;
	}
	if (__argc > 1 && strcmp(__argv[1], "--cammap") == 0) {     // read only: this track's TV camera map to a PNG, then quit
		Game g; std::string err;
		g_logPath = g_ini; g_logPath.replace(g_logPath.size() - 3, 3, "log");
		g_logFile = fopen(g_logPath.c_str(), "w");
		if (g.Attach()) { char tn[33] = {}; g.Read(TRACK_NAME, tn, 32); g_trackName = tn; TvLoad(g); }
		std::string out = __argc > 2 ? __argv[2] : "cammap.png";
		if (__argc > 3) g_mapFile = __argv[3];
		if (CamMapPng(g, out, err)) Log("wrote %s\n", out.c_str()); else Log("Camera map: %s\n", err.c_str());
		GdiplusShutdown(gtok);
		return 0;
	}
	if (__argc > 1 && strcmp(__argv[1], "--preview") == 0) {    // sample panels to PNG files, then quit
		PickSeries();
		CLSID png; UINT n = 0, size = 0;
		GetImageEncodersSize(&n, &size);
		std::vector<BYTE> buf(size);
		ImageCodecInfo *enc = (ImageCodecInfo *)buf.data();
		GetImageEncoders(n, size, enc);
		for (UINT k = 0; k < n; k++) if (wcscmp(enc[k].MimeType, L"image/png") == 0) png = enc[k].Clsid;
		for (Panel *p : g_panels) {
			Bitmap bmp(p->baseW + 40, p->baseH + 40, PixelFormat32bppARGB);
			Graphics gr(&bmp);
			gr.SetSmoothingMode(SmoothingModeAntiAlias);
			gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
			gr.Clear(C(255, 70, 90, 110));                      // stand-in for the game picture
			gr.TranslateTransform(20, 20);
			if (p == &g_tag) { if (DashLook()) DrawTagDash(gr, true); else DrawTag(gr, true); }
			else if (p == &g_battle) { if (DashLook()) DrawBattleDash(gr, true); else DrawBattle(gr, true); }
			else if (p == &g_dash) DrawDash(gr, true); else if (p == &g_qualBox) DrawQual(gr, true); else DrawOnboard(gr, true);
			std::string out = std::string("preview_") + p->key + ".png";
			std::wstring w(out.begin(), out.end());
			bmp.Save(w.c_str(), &png, NULL);
			Log("wrote %s\n", out.c_str());
		}
		GdiplusShutdown(gtok);
		return 0;
	}

	g_logPath = g_ini;
	g_logPath.replace(g_logPath.size() - 3, 3, "log");
	g_logFile = fopen(g_logPath.c_str(), "w");
	Log("RaceVision - NR2003 TV director\n");
	Log("Settings: %s\n", g_ini);
	Log("Ctrl+F8 crowd test sound, Ctrl+F9 overlays on/off, Ctrl+F10 overlay edit mode, Ctrl+F11 pause / resume, Ctrl+F12 quit.\n");
	Log("Can be started any time (no telemetry used). Waiting for NR2003...\n\n");

	Game g;
	g_game = &g;
	g.useCall = S.cameraCall;
	CrowdInit();
	HANDLE ui = StartUi();
	StartRemote();
	double lastAttach = -10, lastPoll = 0, lastDecide = 0, lastDraw = 0, lastStatus = 0;
	for (;;) {
		MSG msg;
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
		if (g_quit) break;                                   // the window was closed
		EnterCriticalSection(&g_lock);
		bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
		if (ctrl && (GetAsyncKeyState(VK_F12) & 1)) { LeaveCriticalSection(&g_lock); break; }
		if (ctrl && (GetAsyncKeyState(VK_F11) & 1)) { g_paused = !g_paused; Log(g_paused ? "-- paused --\n" : "-- directing --\n"); }
		if ((ctrl && (GetAsyncKeyState(VK_F10) & 1)) || g_reqEdit) { g_reqEdit = false; SetEdit(!g_edit); }
		if (g_reqFocus >= 0) {                               // a car picked in the window: like pressing V to it
			int f = g_reqFocus;
			g_reqFocus = -1;
			if (g_car[f].valid && g.SetFocus(f)) {
				if (g_shot.car >= 0) g_car[g_shot.car].lastShown = Now();
				g_shot.car = f; g_shot.kind = kManual; g_shot.other = -1; g_shot.since = Now(); g_shot.why = "";
				Log("           you picked %s - director waits %.0fs\n", Name(f).c_str(), S.manualHold);
			}
		}
		if (g_reqCam >= 0) {                                 // a camera picked on the phone: the director waits too
			int c = g_reqCam;
			g_reqCam = -1;
			if (g.camOk && SetShotCam(g, c, g_shot.car >= 0 && g_car[g_shot.car].valid ? g_shot.car : -1)) {
				g_lastCam = c;
				if (g_shot.car >= 0) { g_shot.kind = kManual; g_shot.other = -1; g_shot.since = Now(); g_shot.why = ""; }
				Log("           you picked camera %d (%s) - director waits %.0fs\n", c, CamName(c), S.manualHold);
			}
		}
		if (g_reqAuto) {                                     // "back to auto": end the wait now
			g_reqAuto = false;
			if (g_shot.kind == kManual) { g_shot.since = Now() - S.manualHold - S.minHold - 1; Log("-- back to the director --\n"); }
		}
		if (g_reqReplay) {                                   // "Replay crash" (window / phone): the best one lately
			g_reqReplay = false;
			if (g_rp.phase) { g_reel.clear(); ReplayEnd(g, "by hand"); }
			else if (g.h) {
				const Crash *best = nullptr;
				for (auto &c : g_crashes)
					if (g_simT - c.first <= S.replayWithin && (!best || Worth(c) > Worth(*best))) best = &c;
				if (!best && !g_crashes.empty()) best = &g_crashes.back();
				ReplayStart(g, best ? best->id : -1, true, -1, true);
			}
		}
		if (g_reqReplayId >= 0) { int id = g_reqReplayId; g_reqReplayId = -1; g_reel.clear(); if (g_rp.phase) ReplayEnd(g, "by hand"); if (g.h) ReplayStart(g, id, true, -1, true); }
		if (g_reqReel) { g_reqReel = false; if (g_rp.phase) ReplayEnd(g, "by hand"); if (g.h) ReelStart(); }
		if (g_reqLive) { g_reqLive = false; g_reel.clear(); g_plan.crash = -1; if (g_rp.phase) ReplayEnd(g, "by hand"); }
		if (g.h) { ReelTick(g); TpService(g); }
		CompanionTick();
		{ static double lastWorth = 0; if (Now() - lastWorth > 0.5) { lastWorth = Now(); CrashWorthWatch(); } }
		if (ctrl && (GetAsyncKeyState(VK_F8) & 1)) CrowdTest();
		if (ctrl && (GetAsyncKeyState(VK_F7) & 1)) g_reqReplay = true;   // watch the best crash now (in your car too)
		if (ctrl && (GetAsyncKeyState(VK_F9) & 1)) {
			g_overlaysOn = !g_overlaysOn; SaveKey("overlays", g_overlaysOn ? "1" : "0");
			Log(g_overlaysOn ? "-- overlays on --\n" : "-- overlays off --\n");
		}

		double now = Now();
		if (now - lastAttach > 2.0) {
			lastAttach = now;
			bool had = g.h != NULL;
			if (g.Attach()) {
				if (!had) Log("NR2003 found.\n");
				if (!g.camOk && g.CheckCamera()) Log("Camera control ready (camera %d).\n", g.Camera());
				if (!g.camOk && g.Get<int>(RP_MODE, 1) == 0) LiveCameraCheck(g);
			} else if (had) { Log("NR2003 closed.\n"); ResetRace(); g_order.clear(); CamGone(); DynGone(); }
		}
		if (g_rp.phase && !g.h) g_rp.phase = 0;              // NR2003 closed mid-replay
		// the tape rolls: race data stands still, only the replay ticks (live 2026-10-26: with polling at 20/s the
		// replay tick never got its turn - the replay never changed angle nor went back to live)
		if (g_rp.phase) { if (now - lastPoll > 0.1) { lastPoll = now; ReplayTick(g); } }
		else if (g.h && now - lastPoll > 0.05) {
			lastPoll = now;
			int session = g_session;
			Poll(g);
			if (session != SESSION_RACE && g_session == SESSION_RACE) { ResetRace(); Log("Race session.\n"); }
			TapeSample(g);
			DynLearnTick(g);
			{   // the same race session started over (restart race): the leader's laps or the game clock go back
				static int lastLaps = -1; static double lastSim = -1;
				if (g_session == SESSION_RACE && lastLaps >= 0 &&
				    ((lastLaps >= 3 && g_leaderLaps + 2 < lastLaps) || (lastSim > 30 && g_simT + 30 < lastSim))) {
					ResetRace(); Log("Race started over: crash log cleared.\n");
				}
				lastLaps = g_session == SESSION_RACE ? g_leaderLaps : -1; lastSim = g_simT;
			}
			if (g_planReset) { g_planReset = false; g_plan.crash = -1; g_reel.clear(); }
			Crowd();
		}
		if (g.camOk && !g_rp.phase) ClimbOutWatch(g);
		if (g.h) FpsWatch(g);
		if (g.h) DynTick(g);                              // dynamic cameras: aim the one on air (racevision_dyncams.cpp)
		if (g.h) CamWork(g);
		{
			static double lastTv = -100;
			if (g.h && !g_rp.phase && now - lastTv > 5) { lastTv = now; TvLoad(g); }
			CamMapRequest(g);
		}
		if (g.camOk) {
			int cam = g.Camera();
			if (g_virtCam && GameCam(g_virtCam) == cam) cam = g_virtCam;   // RaceVision's own camera, not the one it runs on
			if (cam != g_lastCam) { if (g_lastCam >= 0) Log("           camera now %d\n", cam); g_lastCam = cam; }
			if (!g_paused && !g_rp.phase && now - lastDecide > 0.25) {
				lastDecide = now; Direct(g);
				static double lastAuto = 0;
				if (now - lastAuto > 0.5) { lastAuto = now; ReplayAuto(g); if (!g_rp.phase) FinishAuto(g); }
			} else if (g_paused && !g_rp.phase && now - lastDecide > 0.25) {
				lastDecide = now; FollowPicked(g, true);         // directing paused: the overlays still follow the car you pick
			}
			if (!g_rp.phase) RearKeep(g);
		}
		TlTick(g);                                    // the director view's recorder (racevision_timeline.cpp)
		static double lastArt = -100;
		if (!g_order.empty() && now - lastArt > 15) { lastArt = now; ArtRoots(); PickSeries(); }   // AIMOD may have started since
		if (now - lastDraw > (g_boxSliding ? 0.03 : 0.2)) { lastDraw = now; UpdateOverlays(); }   // ~30 fps while rows slide
		if (g.h && now - lastStatus > 60) {
			lastStatus = now;
			Log("  status: %d cars, track %.0f m, field %.0f mph, %s, leader lap %d/%d, on screen: %s\n",
			       (int)g_order.size(), g_trackLen, g_fieldSpeed * 2.237, g_flag == FLAG_GREEN ? "green" : "not green",
			       g_leaderLaps, g_lapsTotal, g_shot.car >= 0 ? Name(g_shot.car).c_str() : "-");
		}
		LeaveCriticalSection(&g_lock);
		Sleep(15);
	}
	TlClose();
	DynSave();
	CamRestore(g);                                    // the cameras as the game had them
	g.RemoveHook();                                   // leave NR2003's code as we found it
	if (g_ui) PostMessage(g_ui, WM_CLOSE, 0, 0);
	WaitForSingleObject(ui, 3000);
	for (auto *v : g_voices) v->DestroyVoice();
	if (g_xa) g_xa->Release();
	CompanionShow();
	for (Panel *p : g_panels) DestroyWindow(p->hwnd);
	for (auto &kv : g_imgs) delete kv.second;
	g_imgs.clear();
	GdiplusShutdown(gtok);
	return 0;
}
