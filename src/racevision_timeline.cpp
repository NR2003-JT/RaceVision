// ---------------------------------------------------------------- the director's timeline + live camera map (2026-11-01)
// Build notes: "Dynamic Camera Map Recorder with Timeline" - a timeline of how RaceVision follows a race (when and
// where it cut, to what camera, on what driver, and why) and a live track map with the cars as dots and the TV camera
// bands, the one on the broadcast glowing.
// The recorder runs in the main loop (TlTick, under g_lock). It doesn't touch the director: it compares what is on
// screen with the last tick and writes an event when the car, the camera, or the TV camera showing it changes. The
// director leaves notes for it (g_tlBest / g_tlCur scores at a cut, g_tlCamWhy for a camera change, g_tlNote from
// TvHold). Every 0.5 game s it keeps all cars' world x, y (for scrubbing the map back in time).
// Each session is saved as JSON lines in %PROGRAMDATA%\NR2003 Companions\racevision\timelines\ (h = header, m = map
// and TV cameras, c = cars, e = event, p = positions); the director page (racevision_director.html, /director) reads
// the live state from /api/director/* or opens a saved file.
// The map: RaceControl's map.json when there is one; else RaceVision learns one from the cars (lap distance bins).
static std::string J(const std::string &ansi);       // racevision_remote.cpp
static std::string DynLiveJson();                    // racevision_dyncams.cpp
static bool DynPoseFor(int cam, float *pose, std::string &label);

enum TlSrc { tlCut, tlCamera, tlPicked, tlQual, tlReplay, tlDrive, tlTv, tlSetting, tlStart };
static const char *TL_SRC[] = { "cut", "camera", "picked", "qualifying", "replay", "drive", "tv", "setting", "start" };
struct TlEvent {
	double t = 0; std::string wall; int lap = 0, flag = 0, car = -1, num = 0, cam = -1, base = -1, tg = -1, tn = 0, kind = 0, src = 0, other = -1;
	float x = 0, y = 0, d = -1; double best = -1, cur = -1; std::string why, note;
	bool dyn = false; float dpose[5] = {}; std::string dlabel;   // a dynamic camera: where it stood (x, y, z, yaw, lens) and what it was
};
struct TlSample { double t; int flag, car, cam; std::vector<unsigned char> slot; std::vector<float> xy; };
static std::vector<TlEvent> g_tlEv;
static std::vector<TlSample> g_tlPos;
static std::vector<std::pair<double, int>> g_tlFlags, g_tlLaps;   // (game s, flag) changes / (game s, leader lap) when it changes
static const size_t TL_MAX_POS = 2 * 3600 * 5;       // 5 hours at 2 a second
static int g_tlSid = 0;                               // session number (the page starts over when it changes)
static std::string g_tlKey, g_tlFile, g_tlMapJson = "null", g_tlCarsJson = "[]", g_tlMapSig;
static FILE *g_tlOut = nullptr;
static double g_tlLastFlush = 0, g_tlLastPos = -1, g_tlLastLive = 0, g_tlLastMapTry = -100;
// the director's notes (g_tlBest, g_tlCur, g_tlCamWhy, g_tlNote) are declared in tv_director.cpp, by g_shot
// the live picture: every car's x, y (main loop, ~20 a second), read by the page
struct TlCar { int slot; float x, y; bool pit; };
static std::vector<TlCar> g_tlLive;
// RaceVision's own map, learnt from the cars when RaceControl hasn't drawn one
static const int TL_BINS = 500;
static std::vector<double> g_tlBx, g_tlBy; static std::vector<int> g_tlBn;
static double g_tlBinLen = 0;
static TrackMapPts g_tlMap; static bool g_tlMapOwn = false;

static std::string TlNum(double v, int dec = 1) { char b[32]; snprintf(b, sizeof b, "%.*f", dec, v); return b; }
static std::string TlColor(int number) {
	auto it = g_colors.find(NumKey(number));
	if (it == g_colors.end()) return "";
	char b[16]; snprintf(b, sizeof b, "#%02X%02X%02X", it->second.GetR(), it->second.GetG(), it->second.GetB());
	return b;
}
static const char *TlSession() { return g_session == SESSION_RACE ? "race" : g_session == SESSION_PRACTICE ? (g_qual ? "qualifying" : "practice") : "session"; }

static std::string TlEventJson(const TlEvent &e) {
	char b[512];
	snprintf(b, sizeof b, "{\"t\":%.2f,\"w\":\"%s\",\"lap\":%d,\"flag\":%d,\"car\":%d,\"num\":\"%s\",\"cam\":%d,\"base\":%d,\"tg\":%d,\"tn\":%d,"
	         "\"kind\":\"%s\",\"src\":\"%s\",\"other\":%d,\"x\":%.1f,\"y\":%.1f,\"d\":%.1f,\"best\":%.1f,\"cur\":%.1f,",
	         e.t, e.wall.c_str(), e.lap, e.flag, e.car, e.car >= 0 ? NumLabel(e.num).c_str() : "", e.cam, e.base, e.tg, e.tn,
	         KindName[e.kind], TL_SRC[e.src], e.other, e.x, e.y, e.d, e.best, e.cur);
	std::string dynj;
	if (e.dyn) { char d[200]; snprintf(d, sizeof d, ",\"dyn\":{\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"yaw\":%.4f,\"fov\":%.1f}", e.dpose[0], e.dpose[1], e.dpose[2], e.dpose[3], e.dpose[4]); dynj = d; }
	return b + std::string("\"name\":") + J(e.car >= 0 ? Name(e.car) : "") + ",\"camName\":" + J(e.dyn ? e.dlabel : e.cam >= 0 ? CamName(e.cam) : "") +
	       ",\"why\":" + J(e.why) + ",\"note\":" + J(e.note) + dynj + "}";
}
static std::string TlSampleJson(const TlSample &s) {
	std::string o = "{\"t\":" + TlNum(s.t, 2) + ",\"flag\":" + std::to_string(s.flag) + ",\"car\":" + std::to_string(s.car) +
	                ",\"cam\":" + std::to_string(s.cam) + ",\"p\":[";
	char b[48];
	for (size_t i = 0; i < s.slot.size(); i++) {
		snprintf(b, sizeof b, "%s%d,%.1f,%.1f", i ? "," : "", s.slot[i], s.xy[2 * i], s.xy[2 * i + 1]);
		o += b;
	}
	return o + "]}";
}
static void TlWrite(const char *tag, const std::string &json) {
	if (!g_tlOut) return;
	fprintf(g_tlOut, "{\"%s\":%s}\n", tag, json.c_str());
}

static std::string TlDir() {
	char pd[MAX_PATH];
	if (!GetEnvironmentVariableA("PROGRAMDATA", pd, MAX_PATH)) return "";
	std::string d = std::string(pd) + "\\NR2003 Companions";
	CreateDirectoryA(d.c_str(), NULL); d += "\\racevision"; CreateDirectoryA(d.c_str(), NULL);
	d += "\\timelines"; CreateDirectoryA(d.c_str(), NULL);
	return d;
}
struct TlFileInfo { std::string name; ULONGLONG size; FILETIME at; };
static std::vector<TlFileInfo> TlFiles() {
	std::vector<TlFileInfo> v;
	std::string dir = TlDir();
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "\\*.jsonl").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return v;
	do v.push_back({ fd.cFileName, ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, fd.ftLastWriteTime });
	while (FindNextFileA(h, &fd));
	FindClose(h);
	std::sort(v.begin(), v.end(), [](const TlFileInfo &a, const TlFileInfo &b) { return CompareFileTime(&a.at, &b.at) > 0; });
	return v;
}

// ---- the TV cameras of a set as the page draws them: [[TV1 cameras], [TV2], [spectator]], each camera = where it stands
// (world x, y, height), the stretch of the lap it shows (from..to, the map's own distance), its lens (zoom, degrees) and
// whether it is fixed. lap = the game's lap length the camera file is measured in; the map's may differ a hair.
static std::string TlTvArrayJson(const TrackMapPts &m, double lap, const std::vector<TvCam> g[3]) {
	double scaleL = lap > 0 && m.len > 0 ? lap / m.len : 1;
	std::string o = "[";
	char b[256];
	for (int k = 0; k < 3; k++) {
		o += k ? ",[" : "[";
		const std::vector<TvCam> &v = g[k];
		for (size_t i = 0; i < v.size(); i++) {
			float x, y; MapAt(m, v[i].sf / scaleL, v[i].cl, x, y);
			double to = v.size() < 2 ? v[i].from + lap : v[(i + 1) % v.size()].from;
			snprintf(b, sizeof b, "%s{\"n\":%d,\"from\":%.1f,\"to\":%.1f,\"x\":%.1f,\"y\":%.1f,\"h\":%.1f,\"z\":%.1f,\"fixed\":%s}", i ? "," : "",
			         v[i].n, v[i].from / scaleL, TvMod(to, lap) / scaleL, x, y, v[i].h, v[i].zoom * 57.2958, v[i].type == 1 ? "true" : "false");
			o += b;
		}
		o += "]";
	}
	return o + "]";
}

// ---- the map the page draws: track points, each TV camera (where it stands, the stretch it shows), pit cameras
static void TlBuildMapJson(Game &g) {
	TrackMapPts m; bool own = false;
	if (!g_tvTrack.empty() && LoadTrackMap(g_tvTrack, m)) own = false;
	else if (g_tlMapOwn && g_tlMap.x.size() >= 100) { m = g_tlMap; own = true; }
	std::string sig = g_tvTrack + "|" + std::to_string(m.x.size()) + "|" + TlNum(m.len, 0) + "|" + (own ? "own" : "rc");
	for (int k = 0; k < 3; k++) {                    // (every camera's place and lens: a set can differ only in its lenses)
		sig += "|" + std::to_string(g_tv[k].size());
		for (auto &c : g_tv[k]) sig += TlNum(c.from, 1) + "," + TlNum(c.zoom * 1000, 0) + "," + TlNum(c.cl, 1) + ";";
	}
	if (sig == g_tlMapSig) return;
	g_tlMapSig = sig;
	if (m.x.size() < 100) { g_tlMapJson = "null"; return; }
	std::string o = "{\"track\":" + J(g_tvTrack) + ",\"len\":" + TlNum(m.len, 2) + ",\"lap\":" + TlNum(g_tvLen > 0 ? g_tvLen : m.len, 2) +
	                ",\"from\":" + J(own ? "cars" : "racecontrol") + ",\"pts\":[";
	char b[160];
	for (size_t i = 0; i < m.x.size(); i++) { snprintf(b, sizeof b, "%s%.1f,%.1f", i ? "," : "", m.x[i], m.y[i]); o += b; }
	o += "],\"tv\":" + TlTvArrayJson(m, g_tvLen > 0 ? g_tvLen : m.len, g_tv);
	o += ",\"pit\":[";
	bool first = true;
	for (int p = 0; p < 2 && g.h; p++) {
		float px, py;
		if (!g.Read(PIT1_BLOCK + p * 0x24, &px, 4) || !g.Read(PIT1_BLOCK + p * 0x24 + 4, &py, 4) || !(px == px) || fabs(px) > 1e5) continue;
		snprintf(b, sizeof b, "%s{\"cam\":%d,\"x\":%.1f,\"y\":%.1f}", first ? "" : ",", 17 + p, px, py); o += b; first = false;
	}
	g_tlMapJson = o + "]}";
	TlWrite("m", g_tlMapJson);
}

// learning the map from the cars: lap distance (the camera file's) in 500 bins, the average x, y of cars on track
static void TlLearn(Game &g) {
	if (g_tvLen <= 0) return;
	if (fabs(g_tlBinLen - g_tvLen) > 0.5) { g_tlBx.assign(TL_BINS, 0); g_tlBy.assign(TL_BINS, 0); g_tlBn.assign(TL_BINS, 0); g_tlBinLen = g_tvLen; g_tlMapOwn = false; }
	for (int i = 0; i < NUM_SLOTS; i++) {
		const Car &c = g_car[i];
		if (!c.valid || c.pit || c.speed < 15) continue;
		float d = CarDlong(g, i), x, y;
		if (d < 0 || !CarXY(g, i, x, y)) continue;
		int b = std::min(TL_BINS - 1, (int)(d / g_tvLen * TL_BINS));
		if (g_tlBn[b] >= 40) continue;
		g_tlBx[b] += x; g_tlBy[b] += y; g_tlBn[b]++;
	}
	if (g_tlMapOwn) return;
	int have = 0; for (int n : g_tlBn) if (n >= 3) have++;
	if (have < TL_BINS * 97 / 100) return;
	TrackMapPts m; m.len = g_tvLen;
	for (int b = 0; b < TL_BINS; b++) {                    // gaps: between the nearest filled bins either side
		if (g_tlBn[b] >= 3) { m.x.push_back((float)(g_tlBx[b] / g_tlBn[b])); m.y.push_back((float)(g_tlBy[b] / g_tlBn[b])); continue; }
		int a = b, c = b, s = 0;
		while (s++ < TL_BINS && g_tlBn[(a + TL_BINS) % TL_BINS] < 3) a--;
		s = 0; while (s++ < TL_BINS && g_tlBn[c % TL_BINS] < 3) c++;
		int ia = (a + TL_BINS) % TL_BINS, ic = c % TL_BINS; double t = (double)(b - a) / std::max(1, c - a);
		m.x.push_back((float)(g_tlBx[ia] / g_tlBn[ia] + (g_tlBx[ic] / g_tlBn[ic] - g_tlBx[ia] / g_tlBn[ia]) * t));
		m.y.push_back((float)(g_tlBy[ia] / g_tlBn[ia] + (g_tlBy[ic] / g_tlBn[ic] - g_tlBy[ia] / g_tlBn[ia]) * t));
	}
	int n = TL_BINS, h = std::max(2, std::min(40, (int)floor(12.0 / (m.len / n) + 0.5)));
	double norm = (2.0 * h + 1) * (4.0 * h * h + 4 * h - 3);
	std::vector<float> x0 = m.x, y0 = m.y;
	for (int i = 0; i < n; i++) {
		double sx = 0, sy = 0;
		for (int j = -h; j <= h; j++) { double w = (3 * (3.0 * h * h + 3 * h - 1) - 15.0 * j * j) / norm; int k = ((i + j) % n + n) % n; sx += w * x0[k]; sy += w * y0[k]; }
		m.x[i] = (float)sx; m.y[i] = (float)sy;
	}
	g_tlMap = m; g_tlMapOwn = true;
	Log("Director view: track map learnt from the cars (%s)\n", g_tvTrack.c_str());
}

static void TlCars() {
	std::string o = "[";
	bool first = true;
	for (int i = 0; i < NUM_SLOTS; i++) {
		if (!g_car[i].valid) continue;
		o += std::string(first ? "" : ",") + "[" + std::to_string(i) + "," + J(NumLabel(g_car[i].number)) + "," +
		     J(g_car[i].first + " " + g_car[i].last) + "," + J(TlColor(g_car[i].number)) + "]";
		first = false;
	}
	o += "]";
	if (o == g_tlCarsJson) return;
	g_tlCarsJson = o;
	TlWrite("c", o);
}

static void TlOpen(Game &g) {
	if (g_tlOut) { fclose(g_tlOut); g_tlOut = nullptr; }
	g_tlEv.clear(); g_tlPos.clear(); g_tlFlags.clear(); g_tlLaps.clear(); g_tlSid++; g_tlMapSig.clear(); g_tlCarsJson = "[]"; g_tlLastPos = -1;
	std::string dir = TlDir();
	if (dir.empty()) return;
	std::vector<TlFileInfo> old = TlFiles();             // keep the newest 30 (about 5 MB an hour each)
	for (size_t i = 29; i < old.size(); i++) DeleteFileA((dir + "\\" + old[i].name).c_str());
	SYSTEMTIME st; GetLocalTime(&st);
	char name[160];
	snprintf(name, sizeof name, "%04d-%02d-%02d_%02d%02d%02d_%s_%s.jsonl", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
	         g_trackName.empty() ? "track" : g_trackName.c_str(), TlSession());
	for (char *p = name; *p; p++) if (strchr("\\/:*?\"<>| ", *p)) *p = '_';
	g_tlFile = name;
	g_tlOut = fopen((dir + "\\" + name).c_str(), "wb");
	char b[256];
	snprintf(b, sizeof b, "{\"v\":1,\"track\":%s,\"session\":\"%s\",\"started\":\"%04d-%02d-%02d %02d:%02d:%02d\",\"laps\":%d}",
	         J(g_trackName).c_str(), TlSession(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, g_lapsTotal);
	TlWrite("h", b);
	Log("Director view: recording to %s\n", name);
	TlBuildMapJson(g);
}

static void TlAdd(Game &g, TlEvent e) {
	SYSTEMTIME st; GetLocalTime(&st);
	char w[16]; snprintf(w, sizeof w, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
	e.wall = w; e.t = g_simT; e.lap = g_leaderLaps; e.flag = g_flag;
	if (e.car >= 0 && e.car < NUM_SLOTS) {
		e.num = g_car[e.car].number;
		float x, y; if (CarXY(g, e.car, x, y)) { e.x = x; e.y = y; }
		e.d = CarDlong(g, e.car);
	}
	if (e.cam >= 0) e.base = GameCam(e.cam);
	if (e.cam >= 0) { std::string lb; if (DynPoseFor(e.cam, e.dpose, lb)) { e.dyn = true; e.dlabel = lb; } }
	if (Now() - g_tlNoteAt < 1.5 && e.note.empty()) { e.note = g_tlNote; g_tlNote.clear(); g_tlNoteAt = -100; }
	g_tlEv.push_back(e);
	TlWrite("e", TlEventJson(e));
}

// the settings worth marking on the timeline: when one changes the page shows where
static const char *TL_KEYS[] = { "min_hold", "max_hold", "switch_margin", "revisit_after", "onboard_hold", "manual_hold", "follow_after",
                                 "camera_every", "tv_cut_gap", "tv_cut_wait", "lead_battle_depth", "lead_battle_hold", "battle_gap" };

static void TlTick(Game &g) {
	static int lastCar = -2, lastCam = -2, lastKind = -1, lastTg = -1, lastTk = -1, lastSrc = -1, lastOther = -2;
	static std::map<std::string, std::string> lastKeys;
	double now = Now();
	if (!g.h || g_order.empty()) return;
	std::string key = g_trackName + "|" + std::to_string(g_session) + "|" + std::to_string(g_raceNo);   // (not g_qual: qualifying runs inside practice)
	if (key != g_tlKey) { g_tlKey = key; TlOpen(g); lastCar = lastCam = -2; lastKind = lastTg = lastTk = lastSrc = -1; lastOther = -2; lastKeys.clear(); }
	if (now - g_tlLastMapTry > 10) { g_tlLastMapTry = now; TlBuildMapJson(g); TlCars(); }

	// flag and lap marks (the timeline's flag strip and lap ticks)
	if (!g_rp.phase && (g_tlFlags.empty() || g_tlFlags.back().second != g_flag)) {
		g_tlFlags.push_back({ g_simT, g_flag }); TlWrite("f", "[" + TlNum(g_simT, 2) + "," + std::to_string(g_flag) + "]");
	}
	if (!g_rp.phase && (g_tlLaps.empty() || g_tlLaps.back().second != g_leaderLaps)) {
		g_tlLaps.push_back({ g_simT, g_leaderLaps }); TlWrite("l", "[" + TlNum(g_simT, 2) + "," + std::to_string(g_leaderLaps) + "]");
	}

	// the live dots
	if (now - g_tlLastLive > 0.05) {
		g_tlLastLive = now;
		g_tlLive.clear();
		for (int i = 0; i < NUM_SLOTS; i++) {
			float x, y;
			if (g_car[i].valid && CarXY(g, i, x, y) && x == x && y == y) g_tlLive.push_back({ i, x, y, g_car[i].pit });
		}
		if (!g_rp.phase) TlLearn(g);
	}

	// what's on screen
	bool replay = g_rp.phase != 0;
	int car = replay ? g_rp.car : g_shot.car, cam = replay ? g_rp.cam : g_lastCam;
	int src = replay ? tlReplay : (g_youDrive || g_driveCar >= 0) ? tlDrive : g_shot.kind == kManual ? tlPicked :
	          (g_qual && g_session == SESSION_PRACTICE) ? tlQual : -1;
	int tg = replay ? -1 : TvGroupOf(cam), tk = -1;
	if (tg >= 0 && car >= 0 && car < NUM_SLOTS && g_car[car].valid) {
		float d = CarDlong(g, car); double since, toNext;
		if (d >= 0) tk = TvZone(tg, d, since, toNext);
	}
	if (car != lastCar || cam != lastCam || (src >= 0 ? src : tlCut) != (lastSrc >= 0 ? lastSrc : tlCut) || (!replay && g_shot.kind != lastKind) ||
	    (!replay && g_shot.other != lastOther && car == lastCar)) {
		TlEvent e; e.car = car; e.cam = cam; e.tg = tg; e.tn = tg >= 0 && tk >= 0 ? g_tv[tg][tk].n : 0;
		e.kind = replay ? kIncident : g_shot.kind; e.other = replay ? -1 : g_shot.other;
		if (replay) { e.src = tlReplay; e.why = "crash replay"; if (cam != lastCam && car == lastCar) e.note = "next angle"; }
		else if (src >= 0) { e.src = src; e.why = g_shot.why.empty() ? (src == tlPicked ? "you picked the car" : "") : g_shot.why; }
		else if (car != lastCar) { e.src = tlCut; e.why = g_shot.why; e.best = g_tlBest; e.cur = g_tlCur; }
		else if (cam != lastCam) { e.src = tlCamera; e.why = g_tlCamWhy.empty() ? "camera change" : g_tlCamWhy; }
		else { e.src = tlCut; e.why = g_shot.why; e.note = g_shot.other != lastOther ? "battle partner changed" : "same car, new reason"; }
		if (lastCar == -2) e.src = src >= 0 ? src : tlStart;
		g_tlBest = g_tlCur = -1; g_tlCamWhy.clear();
		TlAdd(g, e);
		lastCar = car; lastCam = cam; lastKind = g_shot.kind; lastSrc = src; lastOther = g_shot.other; lastTg = tg; lastTk = tk;
	} else if (tg >= 0 && tk >= 0 && (tg != lastTg || tk != lastTk)) {
		// the game's own cut: the next TV camera of the group took over the car
		TlEvent e; e.car = car; e.cam = cam; e.tg = tg; e.tn = g_tv[tg][tk].n; e.kind = g_shot.kind; e.other = g_shot.other; e.src = tlTv;
		char b[80]; snprintf(b, sizeof b, "%s camera %d takes over", TV_GROUP_NAME[tg], g_tv[tg][tk].n); e.why = b;
		if (lastTk >= 0 && lastTg == tg) { snprintf(b, sizeof b, "from camera %d", g_tv[tg][lastTk].n); e.note = b; }
		TlAdd(g, e);
		lastTg = tg; lastTk = tk;
	}

	// settings changed (desktop, phone or the director page): a mark on the timeline
	for (const char *k : TL_KEYS) {
		std::string v = Raw(k);
		auto it = lastKeys.find(k);
		if (it != lastKeys.end() && it->second != v) {
			TlEvent e; e.car = car; e.cam = cam; e.kind = g_shot.kind; e.src = tlSetting; e.why = std::string(k) + " " + it->second + " -> " + v;
			e.note = k;
			TlAdd(g, e);
		}
		lastKeys[k] = v;
	}

	// positions for scrubbing (not while the tape plays: the cars are the replay's then)
	if (!replay && g_simT > g_tlLastPos + 0.5 - 1e-6 && g_tlPos.size() < TL_MAX_POS) {
		g_tlLastPos = g_simT;
		TlSample s; s.t = g_simT; s.flag = g_flag; s.car = car; s.cam = cam;
		for (auto &c : g_tlLive) { s.slot.push_back((unsigned char)c.slot); s.xy.push_back(c.x); s.xy.push_back(c.y); }
		g_tlPos.push_back(s);
		TlWrite("p", TlSampleJson(s));
	}
	if (g_tlOut && now - g_tlLastFlush > 5) { g_tlLastFlush = now; fflush(g_tlOut); }
}
static void TlClose() { if (g_tlOut) { fclose(g_tlOut); g_tlOut = nullptr; } }

// ---- the page's calls (racevision_remote.cpp); each takes g_lock
static std::string TlLiveJson() {
	EnterCriticalSection(&g_lock);
	bool att = g_game && g_game->h;
	bool replay = g_rp.phase != 0;
	int car = replay ? g_rp.car : g_shot.car, cam = replay ? g_rp.cam : g_lastCam;
	int tg = replay ? -1 : TvGroupOf(cam), tk = -1;
	double toNext = -1, since = -1;
	if (att && tg >= 0 && car >= 0 && car < NUM_SLOTS && g_car[car].valid) {
		float d = CarDlong(*g_game, car);
		if (d >= 0) { tk = TvZone(tg, d, since, toNext); double v = std::max(5.0f, g_car[car].speed); toNext /= v; since /= v; }
	}
	char b[512];
	snprintf(b, sizeof b, "{\"attached\":%s,\"sid\":%d,\"t\":%.2f,\"lap\":%d,\"laps\":%d,\"flag\":%d,\"session\":\"%s\",\"paused\":%s,\"replay\":%s,"
	         "\"car\":%d,\"other\":%d,\"kind\":\"%s\",\"cam\":%d,\"base\":%d,\"tg\":%d,\"tk\":%d,\"tn\":%d,\"tvSince\":%.1f,\"tvNext\":%.1f,"
	         "\"age\":%.1f,\"waiting\":%s,\"drive\":%s,\"events\":%d,\"samples\":%d,",
	         att ? "true" : "false", g_tlSid, g_simT, g_leaderLaps, g_lapsTotal, g_flag, TlSession(), g_paused ? "true" : "false", replay ? "true" : "false",
	         car, replay ? -1 : g_shot.other, replay ? "REPLAY" : KindName[g_shot.kind], cam, cam >= 0 ? GameCam(cam) : -1, tg, tk,
	         tg >= 0 && tk >= 0 ? g_tv[tg][tk].n : 0, since, toNext > 1e6 ? -1.0 : toNext, Now() - g_shot.since,
	         g_tvWaitFrom >= 0 ? "true" : "false", (g_youDrive || g_driveCar >= 0) ? "true" : "false", (int)g_tlEv.size(), (int)g_tlPos.size());
	std::string o = b;
	bool art = !g_artSeriesDir.empty() && g_artSeries != "?";     // the selected art folder (number cards, headshots, logo)
	o += std::string("\"series\":") + J(art ? g_artSeries : "") + ",\"logo\":" + (art && IsFile(g_artSeriesDir + "\\serieslogo.png") ? "true" : "false") + ",";
	o += DynLiveJson();
	o += "\"track\":" + J(g_trackName) + ",\"file\":" + J(g_tlFile) + ",\"why\":" + J(replay ? "crash replay" : g_shot.why) +
	     ",\"camName\":" + J(cam >= 0 ? CamName(cam) : "") + ",\"cars\":[";
	for (size_t i = 0; i < g_tlLive.size(); i++) {
		const TlCar &c = g_tlLive[i];
		snprintf(b, sizeof b, "%s[%d,%.1f,%.1f,%d,%d]", i ? "," : "", c.slot, c.x, c.y, c.pit ? 1 : 0, g_car[c.slot].spos);
		o += b;
	}
	o += "]}";
	LeaveCriticalSection(&g_lock);
	return o;
}
static std::string TlMapJsonGet() { EnterCriticalSection(&g_lock); std::string o = "{\"sid\":" + std::to_string(g_tlSid) + ",\"map\":" + g_tlMapJson + ",\"cars\":" + g_tlCarsJson + "}"; LeaveCriticalSection(&g_lock); return o; }
static std::string TlEventsJson(int from) {
	EnterCriticalSection(&g_lock);
	std::string o = "{\"sid\":" + std::to_string(g_tlSid) + ",\"from\":" + std::to_string(std::max(0, from)) + ",\"events\":[";
	for (size_t i = std::max(0, from); i < g_tlEv.size(); i++) o += (i > (size_t)std::max(0, from) ? "," : "") + TlEventJson(g_tlEv[i]);
	o += "],\"flags\":[";
	for (size_t i = 0; i < g_tlFlags.size(); i++) o += (i ? ",[" : "[") + TlNum(g_tlFlags[i].first, 2) + "," + std::to_string(g_tlFlags[i].second) + "]";
	o += "],\"laps\":[";
	for (size_t i = 0; i < g_tlLaps.size(); i++) o += (i ? ",[" : "[") + TlNum(g_tlLaps[i].first, 2) + "," + std::to_string(g_tlLaps[i].second) + "]";
	o += "]";
	if (!g_tlPos.empty()) o += ",\"t0\":" + TlNum(g_tlPos.front().t, 2);
	o += "}";
	LeaveCriticalSection(&g_lock);
	return o;
}
static std::string TlAtJson(double t) {
	EnterCriticalSection(&g_lock);
	std::string o = "null";
	if (!g_tlPos.empty()) {
		auto it = std::lower_bound(g_tlPos.begin(), g_tlPos.end(), t, [](const TlSample &s, double v) { return s.t < v; });
		if (it == g_tlPos.end()) it--;
		if (it != g_tlPos.begin() && fabs((it - 1)->t - t) < fabs(it->t - t)) it--;
		o = TlSampleJson(*it);
	}
	LeaveCriticalSection(&g_lock);
	return o;
}
static std::string TlFilesJson() {
	std::string o = "[";
	std::vector<TlFileInfo> v = TlFiles();
	for (size_t i = 0; i < v.size() && i < 30; i++) {
		SYSTEMTIME u, st; FileTimeToSystemTime(&v[i].at, &u); SystemTimeToTzSpecificLocalTime(NULL, &u, &st);
		char b[96]; snprintf(b, sizeof b, ",\"size\":%llu,\"at\":\"%04d-%02d-%02d %02d:%02d\"}", v[i].size, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
		o += std::string(i ? "," : "") + "{\"name\":" + J(v[i].name) + b;
	}
	return o + "]";
}
static bool TlFileRead(const std::string &name, std::string &body) {
	if (name.empty() || name.find_first_of("\\/:") != std::string::npos || name.find("..") != std::string::npos ||
	    name.size() < 7 || name.substr(name.size() - 6) != ".jsonl") return false;
	EnterCriticalSection(&g_lock);
	if (g_tlOut && name == g_tlFile) fflush(g_tlOut);
	LeaveCriticalSection(&g_lock);
	body = ReadAll(TlDir() + "\\" + name);
	return !body.empty();
}
static std::string DirectorHtml() {
	HRSRC r = FindResourceA(NULL, MAKEINTRESOURCEA(103), (LPCSTR)RT_RCDATA);
	if (!r) return "<h1>RaceVision</h1><p>The director page is missing from this build.</p>";
	HGLOBAL g = LoadResource(NULL, r);
	return std::string((const char *)LockResource(g), SizeofResource(NULL, r));
}
