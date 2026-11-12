// ---------------------------------------------------------------- tape transport + replay transition (2026-11-06)
// Included from tv_director.cpp. NR2003's own replay controls, made available to the app and the phone:
//
//   [0xB8E5A2] current tape frame        [0xB8E586] frames on the tape      [0xB8E59A] 0 live / 1 playing the tape
//   [0xB8E5AA] STEP (int)   [0xB8E5AE] SLOW flag (byte)   [0xB8E5AF] slow counter (int)
//     flag 0: the tape moves STEP frames a tick (1 = normal, 0 = paused, 2..16 fast forward, -1..-16 rewind)
//     flag 1: SLOW MOTION - one frame every |STEP|+1 ticks (STEP 1 = 1/2 ... 16 = 1/17; negative = backwards)
//   Found in the replay tick at 0x503B0D and the replay keys (0x503C00) - the key functions below are the game's own:
//     0x502C20 to the beginning   0x502870 / 0x502900 one frame forward / back   0x502720(fraction 0..1) seek (leaves it paused)
//     0x502B50 stop = back to live (FN_REPLAY_STOP)    0x5024A0(seconds back, paused, 1) play (FN_REPLAY_PLAY)
//
// The speed is set by writing STEP + flag + counter (9 bytes) - exactly what the slow-motion key does one step at a time -
// so any speed can be asked for at once. Everything else runs on the game's main thread through the camera hook.

static const DWORD FN_TP_START = 0x502C20, FN_TP_BACK1 = 0x502900, FN_TP_FWD1 = 0x502870, FN_TP_SEEK = 0x502720;

// ---- speed
static void TpSetSpeed(Game &g, int step, bool slow) {
	if (!g.h) return;
	unsigned char b[9];
	int zero = 0;
	memcpy(b, &step, 4); b[4] = slow ? 1 : 0; memcpy(b + 5, &zero, 4);
	SIZE_T w = 0;
	WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)TP_STEP, b, 9, &w);
}
static std::string TpSpeedText(Game &g) {
	if (g.Get<int>(RP_MODE, 0) != 1) return "LIVE";
	int step = g.Get<int>(TP_STEP, 1);
	bool slow = g.Get<unsigned char>(TP_SLOW, 0) != 0;
	char b[48];
	if (step == 0) return "PAUSED";
	if (slow) { snprintf(b, sizeof b, step < 0 ? "SLOW REVERSE 1/%d" : "SLOW 1/%d", abs(step) + 1); return b; }
	if (step == 1) return "PLAY";
	if (step == -1) return "REWIND";
	snprintf(b, sizeof b, step > 0 ? "FAST x%d" : "REWIND x%d", abs(step));
	return b;
}

// ---- what is on the tape: laps, flags (crashes come from the crash list)
struct TpMark { int frame; char type; std::string label; };
static std::vector<TpMark> g_tpMarks;
static int g_tpLap = -1, g_tpFlag = -1;
static void TpReset() { g_tpMarks.clear(); g_tpLap = -1; g_tpFlag = -1; }
static void TpMarkTick(Game &g, int frames) {
	(void)g;
	if (g_tpLap < 0 || g_leaderLaps > g_tpLap) g_tpMarks.push_back({ frames, 'L', "LAP " + std::to_string(g_leaderLaps + 1) });
	g_tpLap = g_leaderLaps;
	if (g_tpFlag >= 0 && g_flag != g_tpFlag && g_flag != 0) g_tpMarks.push_back({ frames, 'F', g_flag == FLAG_GREEN ? "GREEN" : "CAUTION" });
	if (g_flag != 0) g_tpFlag = g_flag;
	if (g_tpMarks.size() > 4000) g_tpMarks.erase(g_tpMarks.begin(), g_tpMarks.begin() + 500);
}

// ---- requests from the phone / the app (queued, done on the director's thread)
struct TpReq { std::string op; int n = 0; double v = 0; };
static std::vector<TpReq> g_tpQueue;                      // under g_lock
static std::string g_tpMsg; static double g_tpMsgAt = 0;  // what the last request said (shown on the phone)

static void TpQueue(const std::string &op, int n, double v) {
	EnterCriticalSection(&g_lock);
	if (g_tpQueue.size() < 16) g_tpQueue.push_back({ op, n, v });
	LeaveCriticalSection(&g_lock);
}
static void TpSay(const std::string &m) { g_tpMsg = m; g_tpMsgAt = Now(); Log("Playback: %s\n", m.c_str()); }

// the tape under the transport's hand: a replay already rolling is taken over, from live one starts paused a few seconds back
static bool TpEnter(Game &g, std::string &why) {
	if (!g.h) { why = "NR2003 isn't running"; return false; }
	int mode = g.Get<int>(RP_MODE, -1);
	if (mode == 1) {
		if (!g_rp.phase) {                                  // a replay the game (or the user) started: adopt it
			g_rp = ReplayRun(); g_rp.phase = 1; g_rp.at = g_rp.began = Now(); g_rp.len = 1e9; g_rp.ticks0 = g.Get<int>(GAME_TICKS, 0);
			g_rp.frames0 = g.Get<int>(RP_FRAMES, 0);
			g_rp.car = g.Focus() >= 0 ? g.Focus() : -1;
		}
		g_tpManual = true; g_reel.clear(); g_plan.crash = -1;
		return true;
	}
	if (!ReplayReady(g, why)) return false;
	int car = g_shot.car >= 0 && g_car[g_shot.car].valid ? g_shot.car : (g_order.empty() ? -1 : g_order[0]);
	int cam = NextCam("replay");
	{
		TrGuard tg;
		TrBegin(tg, 0, "PLAYBACK");
		float back = 4.f; DWORD a0; memcpy(&a0, &back, 4);
		g.MainThreadCall(FN_REPLAY_PLAY, a0, 1, 1);              // (seconds back, PAUSED, 1 = like the key)
		for (int j = 0; j < 40 && g.Get<int>(RP_MODE, -1) != 1; j++) Sleep(25);
		if (g.Get<int>(RP_MODE, -1) != 1) { g_replayFails++; why = "the replay didn't start"; return false; }
		g_rp = ReplayRun(); g_rp.phase = 1; g_rp.at = g_rp.began = Now(); g_rp.len = 1e9; g_rp.ticks0 = g.Get<int>(GAME_TICKS, 0);
		g_rp.frames0 = g.Get<int>(RP_FRAMES, 0); g_rp.car = car; g_rp.cam = cam;
		g_rp.reel = false; g_rp.crash = -1;
		if (car >= 0 && cam >= 0 && g.Set(cam, car, true)) g_lastCam = cam;
		TpSetSpeed(g, 0, false);
		g_tpManual = true; g_reel.clear(); g_plan.crash = -1;
	}
	return true;
}
static void TpSeekFrame(Game &g, double frame) {
	int frames = g.Get<int>(RP_FRAMES, 0);
	if (frames < 10) return;
	double frac = std::max(0.0, std::min(1.0, frame / (double)(frames - 1)));
	int step = g.Get<int>(TP_STEP, 0); bool slow = g.Get<unsigned char>(TP_SLOW, 0) != 0;
	float f = (float)frac; DWORD a0; memcpy(&a0, &f, 4);
	g.MainThreadCall(FN_TP_SEEK, a0, 0, 0);                      // (it leaves the tape paused)
	if (step != 0) TpSetSpeed(g, step, slow);                    // ...so a tape that was rolling rolls on at its speed
}
static double TpCrashFrame(int id, int rate) {             // (a highlight too: from where its replay starts)
	Crash *c = CrashById(id);
	if (!c) return -1;
	double f = TapeFrameAt(c->impactAt >= 0 ? c->impactAt : c->first, rate);
	double pre = !c->kind ? S.replayPreroll : c->kind == HL_FINISH ? std::max(S.hlPreroll, 8.0) : S.hlPreroll;
	return f >= 0 ? f - pre * rate : -1;
}
static void TpDo(Game &g, const TpReq &r) {
	std::string why;
	if (r.op == "live") {
		g_reel.clear(); g_plan.crash = -1;
		if (g.Get<int>(RP_MODE, 0) == 1) { if (!g_rp.phase) { g_rp.phase = 1; } ReplayEnd(g, "back to live"); }
		return;
	}
	if (!TpEnter(g, why)) { TpSay("not now: " + why); return; }
	int rate = TpRate(g), idx = g.Get<int>(TP_IDX, 0), frames = g.Get<int>(RP_FRAMES, 0);
	int st = g.Get<int>(TP_STEP, 0); bool isSlow = g.Get<unsigned char>(TP_SLOW, 0) != 0;
	if (r.op == "pause") TpSetSpeed(g, 0, false);
	else if (r.op == "toggle") TpSetSpeed(g, st != 0 ? 0 : 1, false);
	else if (r.op == "slower") TpSetSpeed(g, std::min(16, isSlow ? abs(st) + 1 : 1), true);
	else if (r.op == "faster") {
		if (isSlow && abs(st) > 1) TpSetSpeed(g, abs(st) - 1, true);
		else if (isSlow || st < 1) TpSetSpeed(g, 1, false);
		else TpSetSpeed(g, st < 2 ? 2 : std::min(16, st * 2), false);
	}
	else if (r.op == "play") TpSetSpeed(g, 1, false);
	else if (r.op == "slow") { int n = std::max(1, std::min(16, r.n)); TpSetSpeed(g, r.v < 0 ? -n : n, true); }
	else if (r.op == "fast") { int n = std::max(2, std::min(16, r.n)); TpSetSpeed(g, r.v < 0 ? -n : n, false); }
	else if (r.op == "step") g.MainThreadCall(r.n < 0 ? FN_TP_BACK1 : FN_TP_FWD1, 1, 0, 0);
	else if (r.op == "start") g.MainThreadCall(FN_TP_START, 1, 0, 0);
	else if (r.op == "seek") TpSeekFrame(g, r.v * (frames - 1));
	else if (r.op == "lap") {
		std::string want = "LAP " + std::to_string(r.n);
		int best = -1;
		for (auto &m : g_tpMarks) if (m.type == 'L' && m.label == want) { best = m.frame; break; }
		if (best < 0) TpSay("lap " + std::to_string(r.n) + " isn't on the tape");
		else TpSeekFrame(g, best - 1.0 * rate);
	} else if (r.op == "crash") {
		double f = TpCrashFrame(r.n, rate);
		if (f < 0) TpSay("that crash isn't on the tape"); else TpSeekFrame(g, f);
	} else if (r.op == "jump") {                              // next / previous mark of a kind: n = +-1, v = 'L' / 'C' / 'H' / 'F'
		char kind = (char)r.v;
		std::vector<std::pair<double, int>> at;               // frame, crash id
		if (kind == 'C') { for (auto &c : g_crashes) { double f = TpCrashFrame(c.id, rate); if (f >= 0) at.push_back({ f, c.id }); } }
		else if (kind == 'H') { for (auto &c : g_highs) { double f = TpCrashFrame(c.id, rate); if (f >= 0) at.push_back({ f, c.id }); } }
		else for (auto &m : g_tpMarks) if (m.type == kind) at.push_back({ (double)m.frame - (kind == 'L' ? rate : 0), 0 });
		std::sort(at.begin(), at.end());
		double to = -1;
		if (r.n > 0) { for (auto &a : at) if (a.first > idx + rate * 1.5) { to = a.first; break; } }
		else { for (size_t i = at.size(); i-- > 0;) if (at[i].first < idx - rate * 2.0) { to = at[i].first; break; } }
		if (to < 0) TpSay(r.n > 0 ? "nothing later on the tape" : "nothing earlier on the tape");
		else TpSeekFrame(g, to);
	}
}
static void TpService(Game &g) {
	std::vector<TpReq> q;
	EnterCriticalSection(&g_lock);
	q.swap(g_tpQueue);
	LeaveCriticalSection(&g_lock);
	for (auto &r : q) { if (g.h) TpDo(g, r); }
}

// ---- the state for the phone: GET /api/tape
static std::string TpJson(Game *g) {
	std::string o = "{";
	char b[256];
	EnterCriticalSection(&g_lock);
	bool ok = g && g->h && g->camOk;
	int mode = ok ? g->Get<int>(RP_MODE, 0) : 0, frames = ok ? g->Get<int>(RP_FRAMES, 0) : 0, idx = ok ? g->Get<int>(TP_IDX, 0) : 0, rate = ok ? TpRate(*g) : 36;
	std::string speed = ok ? TpSpeedText(*g) : "LIVE", why;
	int step = ok ? g->Get<int>(TP_STEP, 1) : 1; bool slow = ok && g->Get<unsigned char>(TP_SLOW, 0) != 0;
	bool can = ok && ReplaySigOk(*g);
	if (!ok) why = "NR2003 isn't running"; else if (!can) why = "unknown NR2003.exe";
	else if (mode == 0 && g->Get<int>(DRIVER_STATE, -1) == 3) { can = false; why = "you're in your car"; }
	else if (mode == 0 && g_replayFails >= 2) { can = false; why = "replays are off until NR2003 restarts"; }
	snprintf(b, sizeof b, "\"mode\":%d,\"frames\":%d,\"idx\":%d,\"rate\":%d,\"step\":%d,\"slow\":%s,\"manual\":%s,\"can\":%s,\"speed\":", mode, frames, idx, rate, step,
	         slow ? "true" : "false", g_tpManual ? "true" : "false", can ? "true" : "false");
	o += b; o += J(speed); o += ",\"why\":" + J(why);
	o += ",\"msg\":" + J(Now() - g_tpMsgAt < 8 ? g_tpMsg : "");
	o += ",\"lap\":" + std::to_string(g_leaderLaps + 1) + ",\"marks\":[";
	bool first = true;
	auto add = [&](double f, char t, const std::string &label, int id) {
		if (f < 0 || (frames > 0 && f > frames)) return;
		snprintf(b, sizeof b, "%s{\"f\":%d,\"t\":\"%c\",\"id\":%d,\"l\":", first ? "" : ",", (int)f, t, id);
		o += b; o += J(label); o += "}"; first = false;
	};
	for (auto &m : g_tpMarks) add(m.frame, m.type, m.label, 0);
	for (auto &c : g_crashes) {
		double f = TapeFrameAt(c.impactAt >= 0 ? c.impactAt : c.first, rate);
		add(f, 'C', "CRASH  " + CrashWho(c, 2), c.id);
	}
	for (auto &c : g_highs) {                                 // the other highlights: passes, bump and runs, the finish
		double f = TapeFrameAt(c.impactAt >= 0 ? c.impactAt : c.first, rate);
		add(f, 'H', (c.what.empty() ? std::string(HlTitle(c.kind)) : c.what) + "  " + CrashWho(c, 2), c.id);
	}
	o += "]}";
	LeaveCriticalSection(&g_lock);
	return o;
}

// ---------------------------------------------------------------- companion apps' overlays out of the way (hide_companion_overlays)
// A ticker or track map from another app would sit on top of a replay. While a replay or a wipe is on, the visible overlay windows of the
// programs named in companion_apps (a part of the exe's path; blank = every other program) are hidden, and shown again once back to live. Only borderless
// windows (not an app's own main window) and only the ones seen visible at the moment they were hidden; RaceVision's own panels stay.
static bool WipeBusy();
// What counts as an overlay window: visible, no owner, no title bar (an app's own window has one), and - when no program is named -
// topmost (that is what an overlay is). Windows' own shell (taskbar...), the game and RaceVision itself are never touched.
struct CoWin { HWND h; RECT r; };                                         // the window and where it was
static std::vector<CoWin> g_coHidden;
static volatile bool g_coOn = false;
static double g_coOffSince = 0, g_coScanAt = 0;
static CRITICAL_SECTION g_coCs;
static bool g_coInit = false;
static std::string CoLower(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }
static bool CoListHit(const std::string &list, const std::string &path) {
	std::string l = CoLower(list) + ";"; size_t a = 0, b;
	while ((b = l.find(';', a)) != std::string::npos) {
		std::string tok = Trim(l.substr(a, b - a)); a = b + 1;
		if (!tok.empty() && path.find(tok) != std::string::npos) return true;
	}
	return false;
}
static bool CoWantProc(DWORD pid, std::map<DWORD, bool> &cache) {
	auto it = cache.find(pid);
	if (it != cache.end()) return it->second;
	bool want = false;
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (h) {
		char path[MAX_PATH] = {}; DWORD n = MAX_PATH;
		if (QueryFullProcessImageNameA(h, 0, path, &n)) {
			std::string p = CoLower(path), base = p.substr(p.find_last_of("\\/") + 1);
			char win[MAX_PATH] = {}; GetWindowsDirectoryA(win, MAX_PATH);
			std::string w = CoLower(win);
			bool system = (!w.empty() && p.compare(0, w.size(), w) == 0) || p.find("\\windowsapps\\") != std::string::npos || base == "nr2003.exe";
			want = !system && !CoListHit(S.companionKeep, p) && (S.companionApps.empty() || CoListHit(S.companionApps, p));
		}
		CloseHandle(h);
	}
	return cache[pid] = want;
}
struct CoEnumCtx { std::map<DWORD, bool> cache; std::vector<HWND> found; };
static BOOL CALLBACK CoEnum(HWND h, LPARAM l) {
	CoEnumCtx *c = (CoEnumCtx *)l;
	if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
	LONG st = GetWindowLong(h, GWL_STYLE), ex = GetWindowLong(h, GWL_EXSTYLE);
	if (st & WS_CAPTION) return TRUE;                                        // the app's own window, not an overlay
	if (S.companionApps.empty() && !(ex & WS_EX_TOPMOST)) return TRUE;
	DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
	if (pid == GetCurrentProcessId() || !CoWantProc(pid, c->cache)) return TRUE;
	RECT r; GetWindowRect(h, &r);
	if (r.right - r.left < 32 || r.bottom - r.top < 32) return TRUE;           // helper windows
	c->found.push_back(h);
	return TRUE;
}
// hidden AND moved far off screen: an app that shows its window again (they do) then shows it where nobody can see it
static void CoHideWin(HWND h) { SetWindowPos(h, NULL, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW | SWP_ASYNCWINDOWPOS); }
static bool CoListed(HWND h) { for (auto &w : g_coHidden) if (w.h == h) return true; return false; }
// hide what is visible now that isn't yet hidden (a window the app made new or showed again where it was)
static int CoScan() {
	CoEnumCtx ctx;
	EnumWindows(CoEnum, (LPARAM)&ctx);
	int added = 0;
	EnterCriticalSection(&g_coCs);
	for (HWND h : ctx.found) {
		if (!CoListed(h)) {
			CoWin w; w.h = h; GetWindowRect(h, &w.r);
			if (w.r.left < -20000) { w.r.left = 0; w.r.top = 0; }
			g_coHidden.push_back(w); added++;
		}
		CoHideWin(h);
	}
	LeaveCriticalSection(&g_coCs);
	return added;
}
// the guard: every few ms, anything hidden that is visible again or back on screen goes away again (no flash between the main loop's looks)
static DWORD WINAPI CoGuardThread(LPVOID) {
	for (;;) {
		Sleep(g_coOn ? 5 : 50);
		if (!g_coOn) continue;
		EnterCriticalSection(&g_coCs);
		for (auto &w : g_coHidden) {
			if (!IsWindow(w.h)) continue;
			RECT r; GetWindowRect(w.h, &r);
			if (IsWindowVisible(w.h) || r.left > -20000) CoHideWin(w.h);
		}
		LeaveCriticalSection(&g_coCs);
	}
	return 0;
}
static void CompanionHide() {
	if (!S.hideCompanions || g_coOn) return;
	if (!g_coInit) { InitializeCriticalSection(&g_coCs); g_coInit = true; CreateThread(NULL, 0, CoGuardThread, NULL, 0, NULL); }
	EnterCriticalSection(&g_coCs); g_coHidden.clear(); LeaveCriticalSection(&g_coCs);
	CoScan();
	g_coOn = true; g_coOffSince = 0; g_coScanAt = Now();
	Log("           other programs' overlays hidden (%d window%s)\n", (int)g_coHidden.size(), g_coHidden.size() == 1 ? "" : "s");
}
static bool CompanionsAway() { return g_coOn; }
static void CompanionShow() {
	if (!g_coOn) return;
	g_coOn = false;                                          // (the guard stops first)
	EnterCriticalSection(&g_coCs);
	for (auto &w : g_coHidden)
		if (IsWindow(w.h)) SetWindowPos(w.h, NULL, w.r.left, w.r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
	g_coHidden.clear();
	LeaveCriticalSection(&g_coCs);
	Log("           other programs' overlays shown again\n");
}
// every loop: they stay hidden for the whole replay and both wipes (the guard thread keeps them so, this looks for new windows),
// and come back only once the replay is over and the wipe back to live has been gone a moment
static void CompanionTick() {
	bool want = S.hideCompanions && (g_rp.phase || WipeBusy());
	if (want) {
		g_coOffSince = 0;
		if (!g_coOn) { CompanionHide(); return; }
		if (Now() - g_coScanAt > 0.4) { g_coScanAt = Now(); CoScan(); }
	} else if (g_coOn) {
		if (g_coOffSince == 0) g_coOffSince = Now();
		if (!S.hideCompanions || Now() - g_coOffSince > 0.5) CompanionShow();
	}
}

// ---------------------------------------------------------------- the replay transition: a full-screen TV wipe
// Three slanted bars sweep across (a white edge, the accent colour, then the dark one), the screen is covered, the word
// REPLAY / LIVE slams in, and the bars leave the other way. The cut happens while it is covered: the director calls
// TransitionStart, TransitionCover (returns once the screen is covered), makes the cut, TransitionRelease (the wipe leaves).
// It runs on a thread of its own, so the director's loop is never held up drawing it.
struct WipeState {
	HANDLE th = NULL, evGo = NULL, evCover = NULL;
	volatile LONG busy = 0, release = 0;
	int kind = 0; char sub[96] = {};
};
static WipeState g_wp;

static LRESULT CALLBACK WipeProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProc(h, m, w, l); }
static double WipeEase(double x) { x = std::max(0.0, std::min(1.0, x)); return x < 0.5 ? 4 * x * x * x : 1 - pow(-2 * x + 2, 3) / 2; }

static void WipeDraw(Graphics &gr, int W, int H, int phase, double p, double holdT, int kind, const char *sub) {
	gr.Clear(Color(0, 0, 0, 0));
	float sk = H * 0.30f, span = W + 3 * sk;
	Color dark(255, 12, 14, 20), white(255, 255, 255, 255);
	unsigned wc = S.replayWipeColor != 0xFFFFFFFF ? S.replayWipeColor : S.dashAccent;     // its own colour: the dash boxes keep the driver colours
	Color acc(255, (wc >> 16) & 255, (wc >> 8) & 255, wc & 255);
	if (acc.GetR() + acc.GetG() + acc.GetB() < 60) acc = Color(255, 255, 196, 0);
	SolidBrush bDark(dark), bAcc(acc), bWhite(white);
	auto cover = [&](SolidBrush &br, double x) {            // everything left of the slanted edge at x
		PointF pt[4] = { PointF(-2 * sk, 0), PointF((float)x + sk, 0), PointF((float)x, (float)H), PointF(-2 * sk, (float)H) };
		gr.FillPolygon(&br, pt, 4);
	};
	auto rest = [&](SolidBrush &br, double x) {             // everything right of the slanted edge at x
		PointF pt[4] = { PointF((float)x + sk, 0), PointF(W + 2 * sk, 0), PointF(W + 2 * sk, (float)H), PointF((float)x, (float)H) };
		gr.FillPolygon(&br, pt, 4);
	};
	auto stripe = [&](SolidBrush &br, double x, float wdt) {
		PointF pt[4] = { PointF((float)x + sk - wdt, 0), PointF((float)x + sk, 0), PointF((float)x, (float)H), PointF((float)x - wdt, (float)H) };
		gr.FillPolygon(&br, pt, 4);
	};
	if (phase == 0) {                                        // in: white edge leads, then the accent, then the dark bar
		double xw = -2 * sk + span * WipeEase(p / 0.72), xa = -2 * sk + span * WipeEase((p - 0.14) / 0.72), xd = -2 * sk + span * WipeEase((p - 0.28) / 0.72);
		cover(bAcc, xa);
		cover(bDark, xd);
		stripe(bWhite, xw, (float)H * 0.05f);
		stripe(bAcc, xw - H * 0.07f, (float)H * 0.025f);
	} else if (phase == 1) {
		gr.FillRectangle(&bDark, 0, 0, W, H);
	} else {                                                 // out: the dark bar leaves first, the white edge last
		double xd = -2 * sk + span * WipeEase(p / 0.72), xa = -2 * sk + span * WipeEase((p - 0.14) / 0.72), xw = -2 * sk + span * WipeEase((p - 0.28) / 0.72);
		rest(bWhite, xw);
		rest(bAcc, xa + 0.0);
		rest(bDark, xd);
		// the white edge is a thin band, not a cover
		gr.SetCompositingMode(CompositingModeSourceOver);
	}
	// the word, on the dark bar
	double ta = phase == 1 ? std::min(1.0, holdT / 0.14) : phase == 2 ? 1.0 - std::min(1.0, p * 3.0) : 0.0;
	if (ta > 0.01 && kind != 2) {
		FontFamily fam(L"Segoe UI");
		float big = H * 0.20f, sm = H * 0.034f;
		double slide = (1.0 - WipeEase(phase == 1 ? holdT / 0.32 : 1.0)) * W * 0.06;
		for (int y = 0; y < H && phase == 1; y += 4) { SolidBrush sl(Color(14, 255, 255, 255)); gr.FillRectangle(&sl, 0, y, W, 1); }
		Font f(&fam, big, FontStyleBold | FontStyleItalic, UnitPixel);
		StringFormat fmt; fmt.SetAlignment(StringAlignmentCenter); fmt.SetLineAlignment(StringAlignmentCenter);
		const wchar_t *word = kind == 1 ? L"LIVE" : L"REPLAY";
		RectF box((float)(-slide), H * 0.5f - big * 0.62f, (float)W, big * 1.2f);
		SolidBrush sh(Color((BYTE)(110 * ta), 0, 0, 0)), tw(Color((BYTE)(255 * ta), 255, 255, 255)), ta2(Color((BYTE)(255 * ta), acc.GetR(), acc.GetG(), acc.GetB()));
		RectF b2 = box; b2.X += H * 0.008f; b2.Y += H * 0.008f;
		gr.DrawString(word, -1, &f, b2, &fmt, &sh);
		gr.DrawString(word, -1, &f, box, &fmt, &tw);
		// the accent rule under it grows out from the middle
		double grow = phase == 1 ? WipeEase(holdT / 0.30) : 1.0;
		float rw = (float)(W * 0.36 * grow);
		gr.FillRectangle(&ta2, W / 2.f - rw / 2, H * 0.5f + big * 0.62f, rw, H * 0.008f);
		if (kind == 1) {                                     // the red LIVE dot
			SolidBrush red(Color((BYTE)(255 * ta), 235, 60, 50));
			float r = H * 0.022f, cx = W / 2.f - big * 1.45f, cy = H * 0.5f;
			gr.FillEllipse(&red, cx - r, cy - r, 2 * r, 2 * r);
		}
		if (sub && sub[0]) {
			Font fs(&fam, sm, FontStyleBold, UnitPixel);
			StringFormat f2; f2.SetAlignment(StringAlignmentCenter);
			std::wstring ws; for (const char *c = sub; *c; c++) ws += (wchar_t)(unsigned char)*c;
			SolidBrush tg(Color((BYTE)(235 * ta), 200, 205, 215));
			gr.DrawString(ws.c_str(), -1, &fs, RectF(0.f, H * 0.5f + big * 0.62f + H * 0.025f, (float)W, sm * 1.6f), &f2, &tg);
		}
	} else if (ta > 0.01 && kind == 2 && sub && sub[0]) {   // a new angle: just the small label
		FontFamily fam(L"Segoe UI");
		Font fs(&fam, H * 0.05f, FontStyleBold | FontStyleItalic, UnitPixel);
		StringFormat f2; f2.SetAlignment(StringAlignmentCenter); f2.SetLineAlignment(StringAlignmentCenter);
		std::wstring ws; for (const char *c = sub; *c; c++) ws += (wchar_t)(unsigned char)*c;
		SolidBrush tg(Color((BYTE)(255 * ta), acc.GetR(), acc.GetG(), acc.GetB()));
		gr.DrawString(ws.c_str(), -1, &fs, RectF(0.f, 0.f, (float)W, (float)H), &f2, &tg);
	}
	if (phase == 1 && holdT < 0.10) {                        // a flash as the bars meet
		SolidBrush fl(Color((BYTE)(120 * (1.0 - holdT / 0.10)), 255, 255, 255));
		gr.FillRectangle(&fl, 0, 0, W, H);
	}
}

static DWORD WINAPI WipeThread(LPVOID) {
	WNDCLASSA wc = {}; wc.lpfnWndProc = WipeProc; wc.hInstance = GetModuleHandle(NULL); wc.lpszClassName = "RaceVisionWipe";
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "RaceVisionWipe", "wipe", WS_POPUP,
	                            0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
	LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
	auto nowS = [&]() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)fq.QuadPart; };
	for (;;) {
		MSG msg;
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
		if (WaitForSingleObject(g_wp.evGo, 40) != WAIT_OBJECT_0) continue;
		int kind = g_wp.kind; char sub[96]; memcpy(sub, g_wp.sub, sizeof sub); sub[95] = 0;
		int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN);
		double total = S.replayTransitionMs / 1000.0 * (kind == 2 ? 0.55 : 1.0);
		double tIn = total * 0.36, holdMin = kind == 2 ? 0.07 : total * 0.30, tOut = total * 0.34, holdMax = 2.2;
		HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
		BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
		bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
		void *bits = nullptr;
		HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
		if (bmp && bits) {
			HGDIOBJ old = SelectObject(mem, bmp);
			SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, W, H, SWP_NOACTIVATE | SWP_NOREDRAW);
			bool shown = false;
			double t0 = nowS(), tHold = 0, tOutAt = 0;
			int phase = 0;
			for (;;) {
				double el = nowS() - t0, p = 0, holdT = 0;
				if (phase == 0) { p = el / tIn; if (p >= 1.0) { p = 1.0; phase = 1; tHold = nowS(); SetEvent(g_wp.evCover); } }
				if (phase == 1) {
					holdT = nowS() - tHold;
					if ((holdT >= holdMin && g_wp.release) || holdT >= holdMax) { phase = 2; tOutAt = nowS(); }
				}
				if (phase == 2) { p = (nowS() - tOutAt) / tOut; if (p >= 1.0) break; }
				{
					Bitmap canvas(W, H, W * 4, PixelFormat32bppPARGB, (BYTE *)bits);
					Graphics gr(&canvas);
					gr.SetSmoothingMode(SmoothingModeAntiAlias);
					gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
					WipeDraw(gr, W, H, phase, p, holdT, kind, sub);
				}
				POINT src = { 0, 0 }, dst = { 0, 0 };
				SIZE sz = { W, H };
				BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
				UpdateLayeredWindow(hwnd, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
				if (!shown) { ShowWindow(hwnd, SW_SHOWNOACTIVATE); shown = true; }
				Sleep(4);
			}
			ShowWindow(hwnd, SW_HIDE);
			SelectObject(mem, old); DeleteObject(bmp);
		}
		DeleteDC(mem); ReleaseDC(NULL, screen);
		SetEvent(g_wp.evCover);                              // (never leave the director waiting)
		InterlockedExchange(&g_wp.busy, 0);
	}
	return 0;
}
static void TransitionStart(int kind, const std::string &sub) {
	if (!S.replayTransition || g_wp.busy) return;
	if (kind != 1) CompanionHide();                           // (the screen is about to be a replay: out of the way before the wipe)
	OvHideAll(); g_ovBusyAt = Now();                          // RaceVision's own overlays in the same moment (UpdateOverlays keeps them off)
	if (!g_wp.th) {
		g_wp.evGo = CreateEvent(NULL, FALSE, FALSE, NULL);
		g_wp.evCover = CreateEvent(NULL, TRUE, FALSE, NULL);
		g_wp.th = CreateThread(NULL, 0, WipeThread, NULL, 0, NULL);
		if (!g_wp.th) return;
	}
	g_wp.kind = kind;
	memset(g_wp.sub, 0, sizeof g_wp.sub); strncpy(g_wp.sub, sub.c_str(), sizeof g_wp.sub - 1);
	ResetEvent(g_wp.evCover);
	InterlockedExchange(&g_wp.release, 0);
	InterlockedExchange(&g_wp.busy, 1);
	SetEvent(g_wp.evGo);
}
static void TransitionCover() {
	if (g_wp.th && g_wp.busy) WaitForSingleObject(g_wp.evCover, 1200);
}
static void TransitionRelease() {
	InterlockedExchange(&g_wp.release, 1);
}

// ---- RaceVision.exe --wipe out.png [kind]: a contact sheet of the wipe at its key moments over a stand-in picture (no game needed)
static bool WipePng(Bitmap &bm, const wchar_t *path) {
	UINT n = 0, sz = 0; GetImageEncodersSize(&n, &sz);
	if (!sz) return false;
	std::vector<char> buf(sz);
	ImageCodecInfo *ic = (ImageCodecInfo *)buf.data();
	GetImageEncoders(n, sz, ic);
	for (UINT i = 0; i < n; i++)
		if (wcscmp(ic[i].MimeType, L"image/png") == 0) return bm.Save(path, &ic[i].Clsid, NULL) == Ok;
	return false;
}
static void WipeTest(const char *out, int kind) {
	const int W = 1280, H = 720, cols = 3, tw = 640, th = 360;
	struct Fr { int phase; double p, hold; const char *label; };
	Fr frs[] = { { 0, 0.20, 0, "in 20%" }, { 0, 0.45, 0, "in 45%" }, { 0, 0.75, 0, "in 75%" },
	             { 1, 1, 0.03, "covered, flash" }, { 1, 1, 0.20, "covered, word in" }, { 1, 1, 0.50, "covered, held" },
	             { 2, 0.20, 0, "out 20%" }, { 2, 0.45, 0, "out 45%" }, { 2, 0.75, 0, "out 75%" } };
	int rows = 3;
	Bitmap sheet(cols * tw, rows * th, PixelFormat32bppARGB);
	Graphics sg(&sheet);
	sg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
	sg.Clear(Color(255, 30, 30, 30));
	for (int i = 0; i < 9; i++) {
		Bitmap fr(W, H, PixelFormat32bppPARGB);
		{
			Graphics g(&fr);
			g.SetSmoothingMode(SmoothingModeAntiAlias);
			g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
			LinearGradientBrush bg(Point(0, 0), Point(W, H), Color(255, 70, 110, 160), Color(255, 40, 130, 70));   // a stand-in for the game picture
			g.FillRectangle(&bg, 0, 0, W, H);
			SolidBrush car(Color(255, 220, 40, 40)); g.FillRectangle(&car, W * 0.35f, H * 0.55f, W * 0.3f, H * 0.12f);
			Bitmap layer(W, H, PixelFormat32bppPARGB);
			{
				Graphics lg(&layer);
				lg.SetSmoothingMode(SmoothingModeAntiAlias);
				lg.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
				WipeDraw(lg, W, H, frs[i].phase, frs[i].p, frs[i].hold, kind, kind == 0 ? "CRASH  -  #24 BYRON, #10 DILLON" : kind == 2 ? "ANGLE 2" : "BACK TO THE RACE");
			}
			g.DrawImage(&layer, 0, 0, W, H);
		}
		sg.DrawImage(&fr, (i % cols) * tw, (i / cols) * th, tw, th);
		FontFamily fam(L"Segoe UI"); Font f(&fam, 16, FontStyleBold, UnitPixel); SolidBrush wb(Color(255, 255, 255, 255));
		std::wstring ws; for (const char *c = frs[i].label; *c; c++) ws += (wchar_t)*c;
		sg.DrawString(ws.c_str(), -1, &f, PointF((float)((i % cols) * tw + 8), (float)((i / cols) * th + 6)), &wb);
	}
	std::wstring wp; for (const char *c = out; *c; c++) wp += (wchar_t)*c;
	WipePng(sheet, wp.c_str());
}
static bool WipeBusy() { return g_wp.busy != 0; }
