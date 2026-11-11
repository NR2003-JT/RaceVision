// ---------------------------------------------------------------- dynamic cameras (2026-11-03)
// RaceVision places its OWN static cameras where the story is, instead of only choosing between the game's.
//
// How a camera can be put anywhere (read from NR2003.exe 2026-11-03, aim convention checked against the stock pit cameras of
// seven tracks): the pit lane cameras (17 / 18) are fixed cameras - 0x56A54E.. reads their position (block floats 0-2) and their
// yaw / pitch / roll / lens (floats 3-6) from the working blocks at 0xC57378 / 0xC5739C every frame and does NOT aim at the car.
// Writing those blocks moves the camera. Yaw = atan2(dy, dx) in the world x, y frame (the track maps' frame), pitch + = down.
// racevision_cams.cpp already writes a block from g_liveTune[camera id] while that camera is on screen; two virtual cameras ride
// the two blocks - DYN_A (41, on 17) and DYN_B (42, on 18) - and the director alternates them, so the next angle is placed in the
// block that is NOT on screen and the cut is clean (never a half-written view). While one is on screen DynTick re-aims it at the
// car every loop (a pan, eased), so a fixed camera can follow a car through its shot.
//
// What it picks (roles) and the templates it can use are in DYN_T. Where is the camera? Relative to the track (the track map
// gives the centre line at any lap distance: DynAt) - beside the track and ahead of the cars for battles and restarts, at pit
// entry / exit for pit road, around the wreck for a crash, at the line for the finish, next to the car for a pit stall.
//
// It learns, per track, in %PROGRAMDATA%\NR2003 Companions\racevision\learned\<track>.txt: where pit road starts and ends, which
// side of the track it is on, where cars stop in their stalls, where crashes happen, how wide the track is (from where the cars
// drive), and how each template (and side) worked: every shot is scored on what the lens saw (the car at a good size, a pan
// that is not a whip, no car in the way) and your thumbs up / down on the director page; the choice favours what worked, keeps
// trying others, and never repeats itself back to back - the same track gets different angles race after race.
static const int DYN_A = 41, DYN_B = 42;
struct DynR { float lo, hi; };
enum DynAnchor { daTarget, daPitIn, daPitOut, daLine, daPose, daWreck, daCone };
struct DynT {
	const char *id, *label, *role;
	DynAnchor anchor;
	DynR ahead;      // daTarget: seconds of travel ahead of the car; pit / line anchors: metres past the anchor; daPose / daWreck: metres away
	DynR lat;        // metres beyond the edge of the track (not for daPose / daWreck)
	DynR height;     // metres above the road (above the car for daPose)
	DynR fov;        // lens, degrees
	float end;       // daTarget: seconds after the car passes it (- = before); anchors: metres past it (- = before); 0 = hold time only
	float holdMax;   // never longer than this (s)
};
static const DynT DYN_T[] = {
	{ "side_tele",    "Trackside telephoto",   "battle",    daTarget, { 3.6f, 5.5f }, { 20, 45 },  { 4, 12 },       { 7, 13 },   -0.8f, 9 },
	{ "side_mid",     "Trackside, mid range",  "battle",    daTarget, { 2.4f, 3.4f }, { 12, 22 },  { 2, 5 },        { 20, 30 },  0.3f,  7 },
	{ "side_low",     "Low and wide",          "battle",    daTarget, { 1.6f, 2.4f }, { 5, 9 },    { 0.8f, 1.5f },  { 34, 50 },  0.6f,  6 },
	{ "high_corner",  "High corner",           "battle",    daTarget, { 1.0f, 1.8f }, { 40, 70 },  { 25, 45 },      { 38, 52 },  1.2f,  8 },
	{ "restart_low",  "Restart, trackside",    "restart",   daTarget, { 2.5f, 4.0f }, { 8, 16 },   { 1, 3 },        { 16, 28 },  0.5f,  9 },
	{ "restart_high", "Restart, from above",   "restart",   daTarget, { 1.0f, 2.0f }, { 40, 60 },  { 25, 40 },      { 40, 55 },  1.5f,  9 },
	{ "pit_commit",   "Pit entry",             "pit_in",    daPitIn,  { 10, 30 },     { 6, 14 },   { 3, 7 },        { 28, 44 },  35,    14 },
	{ "pit_commit_back", "Pit entry, from up the track", "pit_in", daPitIn, { -140, -70 }, { 10, 22 }, { 2.5f, 6 },  { 16, 26 },  70,    12 },
	{ "pit_commit_high", "Pit entry, from above", "pit_in",  daPitIn,  { -20, 20 },    { 8, 16 },   { 16, 28 },      { 38, 54 },  45,    12 },
	{ "pit_stall",    "Pit stall",             "pit_stall", daPose,   { 5.5f, 9 },    { 0, 0 },    { 1.1f, 2.0f },  { 40, 56 },  0,     9 },
	{ "pit_stall_low", "Pit stall, wheel level", "pit_stall", daPose, { 3.5f, 5.5f },  { 0, 0 },    { 0.55f, 0.95f },{ 55, 70 },  0,     7 },
	{ "pit_stall_high", "Pit stall, from the box", "pit_stall", daPose, { 8, 12 },     { 0, 0 },    { 3.0f, 4.6f },  { 38, 50 },  0,     9 },
	{ "pit_out",      "Pit exit",              "pit_out",   daPitOut, { 25, 60 },     { 7, 13 },   { 2.5f, 5 },      { 28, 42 },  30,    12 },
	{ "pit_out_low",  "Pit exit, low",         "pit_out",   daPitOut, { 15, 40 },     { 5, 8 },    { 0.8f, 1.4f },  { 40, 54 },  25,    9 },
	{ "pit_out_high", "Pit exit, from above",  "pit_out",   daPitOut, { 40, 80 },     { 10, 18 },  { 15, 26 },      { 38, 52 },  40,    11 },
	{ "crash_wide",   "Crash, wide",           "crash",     daWreck,  { 40, 75 },     { 0, 0 },    { 8, 20 },       { 32, 50 },  0,     10 },
	{ "crash_close",  "Crash, close",          "crash",     daWreck,  { 16, 30 },     { 0, 0 },    { 1.5f, 3.5f },  { 45, 62 },  0,     8 },
	{ "crash_high",   "Crash, from above",     "crash",     daWreck,  { 30, 55 },     { 0, 0 },    { 35, 60 },      { 40, 58 },  0,     9 },
	{ "finish_line",  "At the line",           "finish",    daLine,   { 8, 25 },      { 6, 11 },   { 1.5f, 4 },     { 30, 45 },  40,    16 },
	{ "finish_high",  "The line from above",   "finish",    daLine,   { 10, 40 },     { 9, 16 },   { 14, 24 },      { 38, 52 },  40,    14 },
	{ "finish_head_on", "Head-on at the line", "finish",    daLine,   { 110, 170 },   { 5, 10 },   { 1.2f, 2.6f },  { 9, 15 },   25,    12 },
	// the choose cone restart (RaceControl's log says where the cone is): metres past the cone, the cars choosing their lane come to it
	{ "cone_side",    "Cone, trackside",       "cone",      daCone,   { -45, -20 },   { 7, 14 },   { 1.5f, 3 },     { 20, 30 },  0,     14 },
	{ "cone_high",    "Cone, from above",      "cone",      daCone,   { -30, 0 },     { 10, 20 },  { 14, 26 },      { 34, 46 },  0,     14 },
	{ "cone_past",    "Cone, looking back",    "cone",      daCone,   { 25, 55 },     { 6, 12 },   { 1.5f, 3 },     { 22, 32 },  0,     14 },
};
static const DynT DYN_TEST = { "test", "Test camera", "test", daPose, { 24, 24 }, { 0, 0 }, { 3, 3 }, { 40, 40 }, 0, 8 };

// ---- what has been learnt about this track
struct DynArm { int n = 0; double mean = 0; };
struct DynMem {
	std::string track; double lap = 0; bool dirty = false; double savedAt = 0, loadedAt = 0;
	std::vector<float> pitIn, pitOut;                 // lap distances (game metres) where cars turned into / left pit road
	std::vector<std::array<float, 2>> stalls;         // where cars stopped in the pits
	std::vector<std::array<float, 3>> crashes;        // x, y, how many crashes there
	int sideVotes[2] = {};                            // pit side seen from the stalls: [0] right of travel, [1] left
	int latHist[41] = {}; int latN = 0;               // |distance from the centre line| of cars racing, 1 m bins
	int cautions = 0, cautionCars = 0, pitters = 0;   // how many of the cars on track pit at a caution
	std::map<std::string, DynArm> arms;               // "<template>|<L or R>" -> how it scored
	std::map<std::string, float> reach;               // template id -> 0.3..1: how much of its distance range this track allows (thumbs down pulls it in)
};
static const float DYN_MAX_OUT = 30.f;                // never further than this beyond the edge of the track (the scenery starts there)
static DynMem g_dm;
static float DynReach(const char *id) { auto it = g_dm.reach.find(id); return it == g_dm.reach.end() ? 1.f : it->second; }
static std::mt19937 g_dynRng{ std::random_device{}() };
static float DynRnd(float lo, float hi) { return lo + (hi - lo) * std::uniform_real_distribution<float>(0.f, 1.f)(g_dynRng); }
static float DynRnd(DynR r) { return DynRnd(r.lo, r.hi); }
static float DynMedian(std::vector<float> v) { if (v.empty()) return -1; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
static double DynWrap(double d, double len) { d = fmod(d, len); if (d >= len / 2) d -= len; else if (d < -len / 2) d += len; return d; }   // -len/2 .. len/2

// ---- the track: centre line, lap length scale
struct DynGeo { TrackMapPts m; double scaleL = 1; bool ok = false; std::string track; double tried = -100; float pitCam[2][3] = {}; bool pitCamOk = false; };
static DynGeo g_dg;
static bool DynGeoOk(Game &g) {
	double now = Now();
	if (g_dg.ok && g_dg.track == g_trackName && (g_tvLen <= 0 || fabs(g_dg.m.len * g_dg.scaleL - g_tvLen) < 1)) return true;
	if (now - g_dg.tried < 5) return g_dg.ok && g_dg.track == g_trackName;
	g_dg.tried = now;
	if (g_trackName.empty() || g_tvLen <= 0) return false;
	TrackMapPts m;
	if (!LoadTrackMap(g_trackName, m)) {
		if (g_tlMapOwn && g_tlMap.x.size() >= 100 && g_tvTrack == g_trackName) m = g_tlMap; else { g_dg.ok = false; return false; }
	}
	g_dg.m = m; g_dg.scaleL = g_tvLen / m.len; g_dg.ok = true; g_dg.track = g_trackName; g_dg.pitCamOk = false;
	if (!g_dm.track.empty() && g_dm.track != g_trackName) { /* DynLearnTick saves and reloads */ }
	return true;
}
static void DynAt(double d, double lat, float &x, float &y) { MapAt(g_dg.m, d / g_dg.scaleL, lat, x, y); }
static void DynTangent(double d, double &tx, double &ty) {
	float ax, ay, bx, by; DynAt(d - 4, 0, ax, ay); DynAt(d + 4, 0, bx, by);
	tx = bx - ax; ty = by - ay; double l = hypot(tx, ty); if (l < 1e-6) { tx = 1; ty = 0; } else { tx /= l; ty /= l; }
}
static bool DynProject(double x, double y, double &d, double &lat) {     // lap distance (game m) and lateral offset (+ = left of travel)
	const TrackMapPts &m = g_dg.m; int n = (int)m.x.size();
	if (n < 50) return false;
	int best = 0; double bd = 1e18;
	for (int i = 0; i < n; i++) { double dx = m.x[i] - x, dy = m.y[i] - y, dd = dx * dx + dy * dy; if (dd < bd) { bd = dd; best = i; } }
	int a = (best + n - 2) % n, b = (best + 2) % n;
	double tx = m.x[b] - m.x[a], ty = m.y[b] - m.y[a], l = hypot(tx, ty); if (l < 1e-6) return false;
	tx /= l; ty /= l;
	lat = (x - m.x[best]) * (-ty) + (y - m.y[best]) * tx;
	d = best / (double)n * m.len * g_dg.scaleL;
	return true;
}
// (tests: cars and their lap distances from a table instead of the game's memory)
static bool g_dynSim = false; static float g_dynSimXYZ[NUM_SLOTS][3]; static float g_dynSimD[NUM_SLOTS];
static float DynCarD(Game &g, int i) { return g_dynSim ? g_dynSimD[i] : CarDlong(g, i); }
static float DynCarZ(Game &g, int i) {
	if (g_dynSim) return g_dynSimXYZ[i][2];
	float z = g.Get<float>(BLOCK0 + i * BLOCK_SIZE + 6630, 0.f);
	return (z == z && fabsf(z) < 3000) ? z : 0.f;
}
static bool DynCarPos(Game &g, int i, float &x, float &y, float &z) {
	if (g_dynSim && i >= 0 && i < NUM_SLOTS) { x = g_dynSimXYZ[i][0]; y = g_dynSimXYZ[i][1]; z = g_dynSimXYZ[i][2]; return true; }
	if (i < 0 || i >= NUM_SLOTS || !CarXY(g, i, x, y) || !(x == x) || !(y == y)) return false;
	z = DynCarZ(g, i); return true;
}

// ---- RaceControl's learnt pit entry (RaceControl.ini PitEntry.<track>, metres from the line) as a start before we have seen a stop
static std::string g_dynRcTrack; static double g_dynRcPitIn = -1;
static double DynRcPitIn() {
	if (g_dynRcTrack == g_trackName) return g_dynRcPitIn;
	g_dynRcTrack = g_trackName; g_dynRcPitIn = -1;
	std::string e = CsExeDir();
	std::string envIni; { char env[MAX_PATH]; if (GetEnvironmentVariableA("RV_RCINI", env, MAX_PATH)) envIni = env; }   // (tests)
	std::vector<std::string> c = { envIni, Raw("racecontrol_ini"), e + "\\..\\..\\RaceControl\\app\\RaceControl.ini", e + "\\..\\RaceControl\\app\\RaceControl.ini", e + "\\RaceControl.ini" };
	for (auto &p : c) {
		if (p.empty() || !IsFile(p)) continue;
		std::string t = ReadAll(p), key = "PitEntry." + g_trackName + "=";
		size_t at = t.find("\n" + key); if (at == std::string::npos) continue;
		g_dynRcPitIn = atof(t.c_str() + at + 1 + key.size());
		return g_dynRcPitIn;
	}
	return -1;
}
static void DynPitCams(Game &g) {               // the stock pit cameras (they stand at the two ends of pit road)
	if (g_dg.pitCamOk || !g.h) return;
	for (int p = 0; p < 2; p++) {
		std::vector<float> raw(9);
		DWORD blk = p ? PIT2_BLOCK : PIT1_BLOCK;
		auto st = g_stockRaw.find(blk);
		if (st != g_stockRaw.end()) raw = st->second; else if (!g.Read(blk, raw.data(), 36)) return;
		for (int k = 0; k < 3; k++) g_dg.pitCam[p][k] = raw[k];
	}
	g_dg.pitCamOk = true;
}
static bool DynPitEnds(double &inD, double &outD) {      // where pit road starts and ends along the lap (game m), best knowledge first
	double len = g_tvLen;
	if (len <= 0) return false;
	inD = g_dm.pitIn.empty() ? -1 : DynMedian(g_dm.pitIn);          // seen: cars turning in / rolling out (the pit flag)
	outD = g_dm.pitOut.empty() ? -1 : DynMedian(g_dm.pitOut);
	if (inD < 0) { double rc = DynRcPitIn(); if (rc >= 0 && rc < len) inD = rc; }   // RaceControl learnt where pit road starts
	double road = -1;                                              // how long pit road is, if the stalls tell
	if (inD >= 0 && g_dg.ok && !g_dm.stalls.empty()) {
		double farthest = 0;
		for (auto &st : g_dm.stalls) { double d, l; if (DynProject(st[0], st[1], d, l)) { double rel = fmod(d - inD + len, len); if (rel < len * 0.4) farthest = std::max(farthest, rel); } }
		if (farthest > 0) road = farthest + 45;
	}
	if ((inD < 0 || outD < 0) && g_dg.pitCamOk) {                   // last resort: the two pit cameras stand at the ends of pit road
		double d1, l1, d2, l2;
		if (DynProject(g_dg.pitCam[0][0], g_dg.pitCam[0][1], d1, l1) && DynProject(g_dg.pitCam[1][0], g_dg.pitCam[1][1], d2, l2)) {
			double span = DynWrap(d2 - d1, len);
			double a = span >= 0 ? d1 : d2;
			if (inD < 0) inD = fmod(a - 40 + len, len);
			if (outD < 0 && road < 0) road = fabs(span) + 90;
		}
	}
	if (inD < 0 && outD >= 0) inD = fmod(outD - 450 + len, len);
	if (inD < 0) return false;
	if (outD < 0) outD = fmod(inD + (road > 0 ? road : 450), len);
	return true;
}
static int DynPitSide() {                               // +1 pit road is left of travel, -1 right
	if (g_dm.sideVotes[0] + g_dm.sideVotes[1] >= 3) return g_dm.sideVotes[1] >= g_dm.sideVotes[0] ? 1 : -1;
	if (g_dg.pitCamOk) {
		double d, l1 = 0, l2 = 0;
		if (DynProject(g_dg.pitCam[0][0], g_dg.pitCam[0][1], d, l1) && DynProject(g_dg.pitCam[1][0], g_dg.pitCam[1][1], d, l2)) return l1 + l2 >= 0 ? 1 : -1;
	}
	return -1;
}
static double DynHalfWidth() {
	if (S.dynHalfWidth > 0) return S.dynHalfWidth;
	if (g_dm.latN >= 400) {                              // 97th percentile of where the cars drive, plus the verge
		int need = (int)(g_dm.latN * 0.97), acc = 0;
		for (int b = 0; b < 41; b++) { acc += g_dm.latHist[b]; if (acc >= need) return std::max(6.0, std::min(30.0, b + 2.5)); }
	}
	return 9.0;
}

// ---- remembering (one small text file per track)
static std::string DynFile(const std::string &track) {
	char pd[MAX_PATH];
	if (!GetEnvironmentVariableA("PROGRAMDATA", pd, MAX_PATH)) return "";
	std::string d = std::string(pd) + "\\NR2003 Companions"; CreateDirectoryA(d.c_str(), NULL);
	d += "\\racevision"; CreateDirectoryA(d.c_str(), NULL);
	d += "\\learned"; CreateDirectoryA(d.c_str(), NULL);
	std::string n = track; for (char &c : n) if (strchr("\\/:*?\"<>| ", c)) c = '_';
	return d + "\\" + n + ".txt";
}
static void DynSave() {
	if (g_dm.track.empty() || !g_dm.dirty) return;
	std::string f = DynFile(g_dm.track);
	if (f.empty()) return;
	std::string o = "RaceVision dynamic camera memory v1\ntrack " + g_dm.track + "\nlap " + TlNum(g_dm.lap, 1) + "\n";
	char b[96];
	o += "pitin"; for (float v : g_dm.pitIn) { snprintf(b, sizeof b, " %.1f", v); o += b; } o += "\n";
	o += "pitout"; for (float v : g_dm.pitOut) { snprintf(b, sizeof b, " %.1f", v); o += b; } o += "\n";
	snprintf(b, sizeof b, "side %d %d\n", g_dm.sideVotes[0], g_dm.sideVotes[1]); o += b;
	o += "stall"; for (auto &s : g_dm.stalls) { snprintf(b, sizeof b, " %.1f %.1f", s[0], s[1]); o += b; } o += "\n";
	o += "crash"; for (auto &s : g_dm.crashes) { snprintf(b, sizeof b, " %.1f %.1f %.0f", s[0], s[1], s[2]); o += b; } o += "\n";
	o += "lat"; for (int i = 0; i < 41; i++) { snprintf(b, sizeof b, " %d", g_dm.latHist[i]); o += b; } o += "\n";
	snprintf(b, sizeof b, "caution %d %d %d\n", g_dm.cautions, g_dm.cautionCars, g_dm.pitters); o += b;
	for (auto &r : g_dm.reach) { snprintf(b, sizeof b, "reach %s %.3f\n", r.first.c_str(), r.second); o += b; }
	for (auto &a : g_dm.arms) { snprintf(b, sizeof b, "arm %s %d %.4f\n", a.first.c_str(), a.second.n, a.second.mean); o += b; }
	FILE *fp = fopen(f.c_str(), "wb");
	if (!fp) return;
	fwrite(o.data(), 1, o.size(), fp); fclose(fp);
	g_dm.dirty = false; g_dm.savedAt = Now();
}
static void DynLoad(const std::string &track) {
	g_dm = DynMem(); g_dm.track = track; g_dm.loadedAt = Now();
	std::string f = DynFile(track), t = f.empty() ? "" : ReadAll(f);
	size_t pos = 0;
	while (pos < t.size()) {
		size_t e = t.find('\n', pos); if (e == std::string::npos) e = t.size();
		std::string line = t.substr(pos, e - pos); pos = e + 1;
		if (!line.empty() && line.back() == '\r') line.pop_back();
		size_t sp = line.find(' '); std::string key = line.substr(0, sp), rest = sp == std::string::npos ? "" : line.substr(sp + 1);
		std::vector<double> nums; { const char *p = rest.c_str(); char *end; while (*p) { double v = strtod(p, &end); if (end == p) break; nums.push_back(v); p = end; } }
		if (key == "lap" && !nums.empty()) g_dm.lap = nums[0];
		else if (key == "pitin") for (double v : nums) g_dm.pitIn.push_back((float)v);
		else if (key == "pitout") for (double v : nums) g_dm.pitOut.push_back((float)v);
		else if (key == "side" && nums.size() >= 2) { g_dm.sideVotes[0] = (int)nums[0]; g_dm.sideVotes[1] = (int)nums[1]; }
		else if (key == "stall") for (size_t i = 0; i + 1 < nums.size(); i += 2) g_dm.stalls.push_back({ (float)nums[i], (float)nums[i + 1] });
		else if (key == "crash") for (size_t i = 0; i + 2 < nums.size(); i += 3) g_dm.crashes.push_back({ (float)nums[i], (float)nums[i + 1], (float)nums[i + 2] });
		else if (key == "lat" && nums.size() >= 41) { for (int i = 0; i < 41; i++) { g_dm.latHist[i] = (int)nums[i]; g_dm.latN += (int)nums[i]; } }
		else if (key == "caution" && nums.size() >= 3) { g_dm.cautions = (int)nums[0]; g_dm.cautionCars = (int)nums[1]; g_dm.pitters = (int)nums[2]; }
		else if (key == "reach") {
			char id[64] = {}; double v = 1;
			if (sscanf(rest.c_str(), "%63s %lf", id, &v) == 2 && v >= 0.3 && v <= 1.0) g_dm.reach[id] = (float)v;
		}
		else if (key == "arm") {
			char id[64] = {}; int n = 0; double mean = 0;
			if (sscanf(rest.c_str(), "%63s %d %lf", id, &n, &mean) == 3) g_dm.arms[id] = { n, mean };
		}
	}
	if (!t.empty()) Log("Dynamic cameras: remembered %s (pit in %d, out %d, %d stalls, %d crash spots, %d shots scored)\n", track.c_str(), (int)g_dm.pitIn.size(),
	                    (int)g_dm.pitOut.size(), (int)g_dm.stalls.size(), (int)g_dm.crashes.size(), (int)g_dm.arms.size());
}
static void DynKeep(std::vector<float> &v, float x, size_t cap = 40) { v.push_back(x); if (v.size() > cap) v.erase(v.begin()); g_dm.dirty = true; }

// ---- the shot on air
struct DynShot {
	bool on = false;
	int camId = 0; const DynT *t = nullptr; std::string role, key, label; int side = 0;
	int car = -1, other = -1; std::vector<int> wreck;
	double began = 0, hold = 10, lastT = 0, camD = 0, endM = 0;
	float cx = 0, cy = 0, cz = 0, yaw = 0, pitch = 0, fov = 30, aimx = 0, aimy = 0, aimz = 0;
	bool first = true;
	double qSum = 0, wSum = 0, blockedSince = 0; bool blocked = false; double lastBlockCheck = 0;
	float lastYaw = 0;
	bool rig = false; double rigRefresh = 0, rigStarted = 0; int rigN = 0, rigSlots[6] = {};   // the follow rig drives its aim (racevision_rig.cpp)
};
static DynShot g_dyn;
static int g_dynNext = 0;                              // 0 -> DYN_A next, 1 -> DYN_B
static std::vector<std::string> g_dynRecent;           // template ids, newest last
static std::string g_dynLastKey;                       // the shot that ended last (thumbs go there)
static double g_dynNoUntil = 0;
static int g_dynCommitFor = -1, g_dynFinishFor = -1, g_dynPitSeenFor = -1;
static std::set<DWORD> g_dynTouched;                   // pit blocks we wrote directly (before they were on screen)

static void DynBlockWrite(Game &g, int camId, const std::vector<float> &v) {     // v = plain units (x y z turn tilt roll lens)
	DWORD blk = camId == DYN_A ? PIT1_BLOCK : PIT2_BLOCK;
	std::vector<float> stock = StockOf(g, blk, 9);                // the game's own values, kept before the first change
	if (stock.empty()) return;
	std::vector<float> raw = stock;
	ToRaw(ckPit, v, raw.data());
	SIZE_T w = 0;
	if (WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)blk, raw.data(), 36, &w) && w == 36) g_dynTouched.insert(blk);
}
static void DynBlockStock(Game &g, int camId) {                   // the game's own values back in one block
	DWORD blk = camId == DYN_A ? PIT1_BLOCK : PIT2_BLOCK;
	auto st = g_stockRaw.find(blk);
	if (st != g_stockRaw.end() && g.h) { SIZE_T w = 0; WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)blk, st->second.data(), 36, &w); }
	g_dynTouched.erase(blk);
}
static void DynRestore(Game &g) {                                  // RaceVision closing
	if (g_rigThread) { RigCmd off; RigPost(g, off); RigShutdown(); }
	if (!g.h) return;
	for (int id : { DYN_A, DYN_B }) { DWORD blk = id == DYN_A ? PIT1_BLOCK : PIT2_BLOCK; if (g_dynTouched.count(blk)) DynBlockStock(g, id); }
	g_liveTune.erase(DYN_A); g_liveTune.erase(DYN_B);
}
static std::vector<float> DynTune(const DynShot &s) {
	return { s.cx, s.cy, s.cz, (float)(s.yaw / DEG + S.dynYawFix), (float)(s.pitch / DEG * S.dynPitchSign), 0.f, s.fov };
}
static const char *DynSideName(int s) { return s > 0 ? "L" : "R"; }

// ---- scoring a shot
static void DynScoreShot(const std::string &key, double r, double weight) {
	DynArm &a = g_dm.arms[key];
	int n = std::min(a.n, 40);
	a.mean = (a.mean * n + r * weight) / (n + weight);
	a.n += (int)std::max(1.0, weight);
	g_dm.dirty = true;
}
// ---- the follow rig: who it aims at, and starting / stopping it
static int DynRigPosMode(Game &g, int slot) {      // 1 = the car object ([record + 0x5AE] + 24), if it agrees with the car block; else 0
	static int pidSeen = 0, mode = -1;
	if (pidSeen == (int)g.pid && mode >= 0) return mode;
	DWORD ptr = g.Get<DWORD>(REC0 + slot * REC_SIZE + 0x5AE, 0);
	float o[3] = {}, bx, by, bz;
	mode = 0;
	if (ptr >= 0x400000 && ptr < 0x7FFFFFFF && g.Read(ptr + 24, o, 12) && DynCarPos(g, slot, bx, by, bz) && RigOk(o[0]) && RigOk(o[1]) && hypotf(o[0] - bx, o[1] - by) < 25.f) mode = 1;
	pidSeen = (int)g.pid;
	Log("Dynamic cameras: the rig follows the %s position\n", mode ? "car object's" : "car block's");
	return mode;
}
static void DynRigTargets(Game &g, const DynShot &s, int &n, int *slots, int &mode) {
	n = 0; mode = 0; slots[n++] = s.car;
	if (s.role == "battle" && s.other >= 0 && s.other != s.car) { slots[n++] = s.other; mode = 1; }
	else if (s.role == "crash") {
		float ax, ay, az; DynCarPos(g, s.car, ax, ay, az);
		for (int j : g_order) {
			if (n >= 6) break;
			if (j == s.car || !g_car[j].valid || g_car[j].pit) continue;
			float x, y, z;
			if (DynCarPos(g, j, x, y, z) && hypotf(x - ax, y - ay) < 60 && (g_car[j].speed < 12 || g_car[j].incidentAt > 0)) slots[n++] = j;
		}
		if (n > 1) mode = 2;
	}
}
static bool DynRigStart(Game &g, DynShot &s) {
	if (!S.dynRig || !g.h) return false;
	RigCmd c; c.on = true; c.block = s.camId == DYN_A ? PIT1_BLOCK : PIT2_BLOCK;
	c.posMode = DynRigPosMode(g, s.car);
	DynRigTargets(g, s, c.n, c.slots, c.p.mode);
	for (int i = 0; i < c.n; i++) s.rigSlots[i] = c.slots[i];
	s.rigN = c.n;
	float ax, ay, az;
	if (!DynCarPos(g, s.car, ax, ay, az)) return false;
	c.p.cx = s.cx; c.p.cy = s.cy; c.p.cz = s.cz;
	float fov0 = s.fov * DEG, d0 = sqrtf((ax - s.cx) * (ax - s.cx) + (ay - s.cy) * (ay - s.cy) + (az + 0.8f - s.cz) * (az + 0.8f - s.cz));
	c.p.fov0 = fov0; c.p.width0 = 2.f * std::max(8.f, d0) * tanf(fov0 / 2);
	c.p.fovMin = std::max(0.07f, fov0 * 0.7f); c.p.fovMax = std::min(1.05f, fov0 * 3.0f);
	c.p.autoZoom = S.dynAutoZoom; c.p.smooth = (float)S.dynSmooth; c.p.lead = 1.f;
	c.yawFix = (float)(S.dynYawFix * DEG); c.pitchSign = (float)S.dynPitchSign;
	if (!RigPost(g, c)) return false;
	s.rig = true; s.rigStarted = Now(); s.rigRefresh = Now();
	return true;
}
static void DynRigStop(Game &g) {
	if (!g_dyn.rig) return;
	RigCmd off; RigPost(g, off);                          // the rig has stopped writing when this returns
	g_dyn.rig = false;
}
static void DynEnd(Game &g, bool switchAway, const char *why) {
	if (!g_dyn.on) return;
	DynRigStop(g);
	double dur = Now() - g_dyn.began;
	if (g_dyn.wSum >= 2.0 && S.dynLearn) {
		double r = std::max(0.0, std::min(1.0, g_dyn.qSum / g_dyn.wSum));
		DynScoreShot(g_dyn.key, r, 1);
		Log("           dynamic camera ended (%s) after %.1f s: %s, score %.2f\n", why, dur, g_dyn.label.c_str(), r);
	} else Log("           dynamic camera ended (%s) after %.1f s: %s\n", why, dur, g_dyn.label.c_str());
	g_dynLastKey = g_dyn.key;
	g_liveTune.erase(g_dyn.camId);
	int id = g_dyn.camId;
	g_dyn.on = false;
	g_dynNoUntil = Now() + 6;
	if (switchAway && g_lastCam == id && g_shot.kind != kManual && g_shot.car >= 0) {
		int c = NextCam(PitCamKey(g_shot.kind, g_car[g_shot.car].pitPhase) + (g_shot.kind == kPit ? "" : "_follow"));
		if (c < 0) c = NextCam(KindKey[g_shot.kind]);
		if (c >= 0 && c != id) { g_tlCamWhy = std::string("dynamic camera ended: ") + why; SetCam(g, c); }
	}
}
static void DynAbort(Game &g) {                                    // the cut to it didn't happen
	if (!g_dyn.on) return;
	DynRigStop(g);
	DynBlockStock(g, g_dyn.camId);
	g_liveTune.erase(g_dyn.camId);
	g_dyn.on = false;
}

// ---- choosing a template and a side
static double DynArmScore(const DynT &t, int side, bool pitSideArm) {
	std::string key = std::string(t.id) + "|" + DynSideName(side);
	auto it = g_dm.arms.find(key);
	double n = it == g_dm.arms.end() ? 0 : it->second.n, mean = n ? it->second.mean : 0.62;
	double s = mean + 0.35 / sqrt(1 + n) + DynRnd(0.f, 0.2f);
	for (size_t i = 0; i < g_dynRecent.size(); i++) {               // not the same angle back to back
		size_t back = g_dynRecent.size() - i;
		if (g_dynRecent[i] == t.id) s -= back <= 2 ? 0.45 : back <= 5 ? 0.22 : 0.08;
	}
	if (!pitSideArm) s -= 0.25;
	return s;
}

// ---- the choose cone (RaceControl's race_control.log: "double file: cars choose their lane at the cone, N m from the line" ... "choose cone done")
struct DynCone { double d = -1, since = 0; bool active = false; int shownFor = 0, seq = 0; ULONGLONG off = 0; std::string path; double polled = 0; };
static DynCone g_cone;
static std::string DynRcFile(const char *name) {                     // a file in RaceControl's app folder, found the way its ini is
	std::string e = CsExeDir();
	std::string envIni; { char env[MAX_PATH]; if (GetEnvironmentVariableA("RV_RCINI", env, MAX_PATH)) envIni = env; }
	std::vector<std::string> c = { envIni, Raw("racecontrol_ini"), e + "\\..\\..\\RaceControl\\app\\RaceControl.ini", e + "\\..\\RaceControl\\app\\RaceControl.ini", e + "\\RaceControl.ini" };
	for (auto &p : c) {
		if (p.empty() || !IsFile(p)) continue;
		size_t cut = p.find_last_of("\\/");
		std::string f = p.substr(0, cut == std::string::npos ? 0 : cut + 1) + name;
		if (IsFile(f)) return f;
	}
	return "";
}
static void DynConePoll() {
	double now = Now();
	if (now - g_cone.polled < 1.0) return;
	g_cone.polled = now;
	if (g_cone.active && now - g_cone.since > 150) g_cone.active = false;
	if (g_cone.path.empty()) { g_cone.path = DynRcFile("race_control.log"); if (g_cone.path.empty()) return; g_cone.off = ~0ull; }
	HANDLE h = CreateFileA(g_cone.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return;
	LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
	ULONGLONG size = (ULONGLONG)sz.QuadPart;
	if (g_cone.off == ~0ull || size < g_cone.off) g_cone.off = size;      // first look (old lines are history) or the log was restarted
	if (size - g_cone.off > 65536) g_cone.off = size - 65536;
	if (size > g_cone.off) {
		std::string buf((size_t)(size - g_cone.off), '\0');
		LARGE_INTEGER at; at.QuadPart = (LONGLONG)g_cone.off; SetFilePointerEx(h, at, NULL, FILE_BEGIN);
		DWORD got = 0; ReadFile(h, &buf[0], (DWORD)buf.size(), &got, NULL);
		buf.resize(got); g_cone.off += got;
		size_t pos = 0;
		while (pos < buf.size()) {
			size_t e = buf.find('\n', pos); if (e == std::string::npos) e = buf.size();
			std::string ln = buf.substr(pos, e - pos); pos = e + 1;
			size_t k = ln.find("cars choose their lane at the cone, ");
			if (k != std::string::npos) {
				g_cone.d = atof(ln.c_str() + k + 36); g_cone.active = g_cone.d >= 0; g_cone.since = now; g_cone.seq++;
				Log("Dynamic cameras: the cone is at %.0f m (RaceControl's log)\n", g_cone.d);
			} else if (ln.find("choose cone done") != std::string::npos || ln.find("double file done") != std::string::npos) g_cone.active = false;
		}
	}
	CloseHandle(h);
}
// a camera set the user chose for this track (saved for it, or switched on live) owns the pit cameras there: the dynamic ones stay out of it
static bool DynUserSet() {
	if (g_trackName.empty()) return false;
	std::string a = CsApplied("track", g_trackName);
	if (!a.empty() && a != "stock") return true;
	return !g_csLiveTrackSet.empty() && g_csLiveTrackSet != "stock";
}

// ---- placing a camera
struct DynPlan { const DynT *t = nullptr; int side = 1; float cx, cy, cz, fov; double camD, endM, hold; bool ok = false; };
static bool DynPlace(Game &g, const std::string &role, int car, int other, DynPlan &p, const DynT *forced = nullptr) {
	float tx, ty, tz;
	if (!DynCarPos(g, car, tx, ty, tz)) return false;
	double len = g_tvLen, hw = DynHalfWidth(), v = std::max(25.0f, g_car[car].speed);
	double inD = 0, outD = 0; bool pitKnown = DynPitEnds(inD, outD);
	double carD = DynCarD(g, car);
	int pitSide = DynPitSide();
	// candidates
	struct Opt { const DynT *t; int side; double score; };
	std::vector<Opt> opts;
	for (const DynT &t : DYN_T) {
		if (forced ? &t != forced : role != t.role) continue;
		bool pitAnch = t.anchor == daPitIn || t.anchor == daPitOut;
		if (pitAnch && !pitKnown) continue;
		if (t.anchor == daCone && g_cone.d < 0) continue;
		if (t.anchor == daTarget && carD < 0) continue;
		for (int side : { 1, -1 }) {
			bool pitArm = !pitAnch || side == pitSide;
			opts.push_back({ &t, side, DynArmScore(t, side, pitArm) });
		}
	}
	if (forced) opts.push_back({ forced, 1, 1.0 });
	if (opts.empty()) return false;
	if (!forced && !g_dynRecent.empty()) {                          // never the same template back to back (when the role has another)
		const std::string &lastId = g_dynRecent.back();
		std::vector<Opt> other;
		for (auto &o : opts) if (lastId != o.t->id) other.push_back(o);
		if (!other.empty()) opts = other;
	}
	std::sort(opts.begin(), opts.end(), [](const Opt &a, const Opt &b) { return a.score > b.score; });
	if (!forced && g_dynRecent.size() >= 2) {                       // exploring: not one of the last two either
		std::vector<Opt> fresh;
		const std::string &l1 = g_dynRecent[g_dynRecent.size() - 1], &l2 = g_dynRecent[g_dynRecent.size() - 2];
		for (size_t i = 1; i < opts.size(); i++) if (l1 != opts[i].t->id && l2 != opts[i].t->id) fresh.push_back(opts[i]);
		if (!fresh.empty() && DynRnd(0.f, 1.f) < 0.22f) { opts[0] = fresh[(size_t)(DynRnd(0.f, 1.f) * std::min<size_t>(4, fresh.size())) % std::min<size_t>(4, fresh.size())]; }
	}
	size_t pick = 0;                                                // mostly the best; now and then one of the next few, so the others keep being tried
	const DynT &t = *opts[pick].t; int side = opts[pick].side;
	p.t = &t; p.side = side; p.fov = DynRnd(t.fov); p.hold = DynRnd(std::max(2.f, t.holdMax * 0.6f), t.holdMax);
	float h = DynRnd(t.height), cx = 0, cy = 0, cz = tz + h;
	double edge = hw + std::min(DynRnd(t.lat), DYN_MAX_OUT) * DynReach(t.id);
	p.camD = carD >= 0 ? carD : 0; p.endM = t.end;
	switch (t.anchor) {
	case daTarget: {
		double lead = DynRnd(t.ahead) * v;
		p.camD = fmod(carD + lead, len);
		DynAt(p.camD, side * edge, cx, cy);
		p.endM = t.end * v;
		break; }
	case daPitIn: p.camD = fmod(inD + DynRnd(t.ahead), len); DynAt(p.camD, side * edge, cx, cy); break;
	case daPitOut: p.camD = fmod(outD + DynRnd(t.ahead), len); DynAt(p.camD, side * edge, cx, cy); break;
	case daCone: p.camD = fmod(g_cone.d + DynRnd(t.ahead) + len, len); DynAt(p.camD, side * edge, cx, cy); break;
	case daLine: p.camD = DynRnd(t.ahead); DynAt(p.camD, side * edge, cx, cy); break;
	case daPose: {
		float head = g_car[car].head, dist = DynRnd(t.ahead);
		double bearing = head + side * DynRnd(0.45f, 1.2f);           // in front, to one side: the nose and the crew
		cx = tx + (float)(cos(bearing) * dist); cy = ty + (float)(sin(bearing) * dist);
		double d, l; p.camD = DynProject(cx, cy, d, l) ? d : carD;
		break; }
	case daWreck: {
		double sx = tx, sy = ty; int n = 1;
		for (int j : g_order) {
			if (j == car || !g_car[j].valid || g_car[j].pit) continue;
			float x, y, z;
			if (!DynCarPos(g, j, x, y, z) || hypot(x - tx, y - ty) > 90) continue;
			if (g_car[j].speed < 12 || g_car[j].incidentAt > 0) { sx += x; sy += y; n++; }
		}
		sx /= n; sy /= n;
		double dd, ll, tdx = 1, tdy = 0;
		if (DynProject(sx, sy, dd, ll)) DynTangent(dd, tdx, tdy);
		double base = atan2(tdy, tdx) + side * (CS_PI / 2) + DynRnd(-0.7f, 0.7f);   // beside the wreck, either side of the track
		double dist = DynRnd(t.ahead);
		cx = (float)(sx + cos(base) * dist); cy = (float)(sy + sin(base) * dist);
		p.camD = DynProject(cx, cy, dd, ll) ? dd : carD;
		break; }
	}
	p.cx = cx; p.cy = cy; p.cz = cz; p.ok = true;
	return true;
}

static void DynStart(Game &g, const DynPlan &p, const std::string &role, int car, int other, int id) {
	g_dyn = DynShot();
	g_dyn.on = true; g_dyn.camId = id; g_dyn.t = p.t; g_dyn.role = role; g_dyn.side = p.side;
	g_dyn.key = std::string(p.t->id) + "|" + DynSideName(p.side);
	g_dyn.car = car; g_dyn.other = other; g_dyn.began = g_dyn.lastT = Now(); g_dyn.hold = p.hold;
	g_dyn.cx = p.cx; g_dyn.cy = p.cy; g_dyn.cz = p.cz; g_dyn.fov = p.fov; g_dyn.camD = p.camD; g_dyn.endM = p.endM;
	char b[160]; snprintf(b, sizeof b, "Dynamic Camera - Static: %s (%s side)", p.t->label, p.side > 0 ? "left" : "right");
	g_dyn.label = b;
	g_dynRecent.push_back(p.t->id); if (g_dynRecent.size() > 6) g_dynRecent.erase(g_dynRecent.begin());
	// aim now, so the first frame is already right
	float ax, ay, az;
	if (DynCarPos(g, car, ax, ay, az)) {
		g_dyn.yaw = (float)atan2(ay - p.cy, ax - p.cx);
		g_dyn.pitch = (float)atan2(p.cz - (az + 0.8f), std::max(1.0, (double)hypot(ax - p.cx, ay - p.cy)));
		g_dyn.aimx = ax; g_dyn.aimy = ay; g_dyn.aimz = az; g_dyn.lastYaw = g_dyn.yaw;
	}
	std::vector<float> v = DynTune(g_dyn);
	g_liveTune[id] = v;
	DynBlockWrite(g, id, v);                                        // in place before the cut (this block isn't on screen)
	bool rig = DynRigStart(g, g_dyn);                               // from here the rig aims it (smooth, 500 times a second)
	Log("           dynamic camera: %s for %s (%s), %.0f m from the car, lens %.0f deg%s\n", g_dyn.label.c_str(), Name(car).c_str(), role.c_str(),
	    hypot((double)g_dyn.aimx - p.cx, (double)g_dyn.aimy - p.cy), p.fov, rig ? ", follow rig" : "");
	g_tlCamWhy = "dynamic camera: " + g_dyn.label;
}

// ---- the director asks: a dynamic camera for this shot? (returns the camera to use)
static const char *DynRole(int kind, int car, int phase) {
	switch (kind) {
	case kPit: return !S.dynPit || DynUserSet() ? nullptr : phase == 2 ? "pit_stall" : phase == 3 ? "pit_out" : "pit_in";
	case kIncident: return S.dynCrash ? "crash" : nullptr;
	case kRestart: return S.dynRestart ? "restart" : nullptr;
	case kBattle: case kPass: return S.dynBattle ? "battle" : nullptr;
	case kLeader: {
		if (!S.dynFinish || g_lapsTotal <= 0 || g_leaderLaps < g_lapsTotal - 1) return nullptr;
		float d = DynCarD(*g_game, car);
		return d >= 0 && g_tvLen - d < 700 ? "finish" : nullptr; }
	}
	return nullptr;
}
static int DynPick(Game &g, int kind, int car, int other, int phase, int cam) {
	if (!S.dynCams || !S.customCams || car < 0 || !g.camOk || g_rp.phase || g_youDrive || g_driveCar >= 0 || g_qual) return cam;
	const char *role = DynRole(kind, car, phase);
	if (!role || !DynGeoOk(g)) return cam;
	bool sure = !strcmp(role, "crash") || !strcmp(role, "pit_stall");
	if (!sure && Now() < g_dynNoUntil) return cam;
	if (!sure && DynRnd(0.f, 100.f) >= S.dynChance) return cam;
	if (g_dyn.on) DynEnd(g, false, "next shot");
	DynPitCams(g);
	DynPlan p;
	if (!DynPlace(g, role, car, other, p) || !p.ok) return cam;
	int id = g_dynNext ? DYN_B : DYN_A;
	g_dynNext ^= 1;
	DynStart(g, p, role, car, other, id);
	if (!strcmp(role, "pit_in")) g_dynCommitFor = g_greens;
	return id;
}

// the leader is about to reach pit entry under caution: the pit decision is a shot (who dives in, who stays out)
static bool DynPitDecision(int &car, std::string &why) {
	if (!S.dynCams || !S.dynPit || !g_game || !g_game->camOk || g_flag == FLAG_GREEN || g_rp.phase || g_youDrive || g_driveCar >= 0 || g_qual) return false;
	if (g_dynCommitFor == g_greens || g_tvLen <= 0 || !DynGeoOk(*g_game)) return false;
	DynPitCams(*g_game);
	double inD, outD;
	if (!DynPitEnds(inD, outD)) return false;
	for (int i : g_score) {
		const Car &c = g_car[i];
		if (!c.valid || c.pit || c.speed < 8) continue;
		float d = DynCarD(*g_game, i);
		if (d < 0) return false;
		double to = DynWrap(inD - d, g_tvLen);                      // + = pit entry is still ahead of him
		if (to > -30 && to < std::max(260.0, c.speed * 9.0)) { car = i; why = "the pit decision: who comes in, who stays out"; return true; }
		return false;                                              // the best placed car on track isn't there yet
	}
	return false;
}

// ---- every loop: aim the camera on air at its car(s), watch for the end of the shot
static void DynTick(Game &g) {
	double now = Now();
	// the last lap: the finish line
	if (S.dynCams && S.dynFinish && !g_dyn.on && g_game && g_game->camOk && !g_rp.phase && g_lapsTotal > 0 && g_leaderLaps >= g_lapsTotal - 1 && g_flag == FLAG_GREEN &&
	    g_dynFinishFor != g_raceNo && g_shot.car >= 0 && !g_youDrive && g_driveCar < 0 && !g_qual && g_shot.kind != kManual && g_tvLen > 0 && g_shot.car == (g_score.empty() ? -1 : g_score[0])) {
		float d = DynCarD(g, g_shot.car);
		if (d >= 0 && g_tvLen - d < 520 && g_tvLen - d > 120 && DynGeoOk(g) && DynRnd(0.f, 100.f) < std::max(S.dynChance, 70.0)) {
			g_dynFinishFor = g_raceNo;
			int c = DynPick(g, kLeader, g_shot.car, -1, 0, -1);
			if (c >= 0) { SetCam(g, c); if (g_lastCam != c) DynAbort(g); }
		}
	}
	// the choose cone: a camera at the cone while the cars pick their lane
	DynConePoll();
	if (S.dynCams && S.dynRestart && g_cone.active && g_cone.shownFor != g_cone.seq && !g_dyn.on && g_game && g_game->camOk && !g_rp.phase && !g_youDrive && g_driveCar < 0 && !g_qual && g_tvLen > 0 && DynGeoOk(g)) {
		int pick = -1; double best = 1e9;
		for (int i : g_order) {
			if (!g_car[i].valid || g_car[i].pit || g_car[i].speed < 5) continue;
			float d = DynCarD(g, i);
			if (d < 0) continue;
			double to = DynWrap(g_cone.d - d, g_tvLen);               // + = the cone is still ahead of him
			if (to > -15 && to < 600 && to < best) { best = to; pick = i; }
		}
		if (pick >= 0) {
			g_cone.shownFor = g_cone.seq;
			if (g_dyn.on) DynEnd(g, false, "next shot");
			DynPitCams(g);
			DynPlan p;
			if (DynPlace(g, "cone", pick, -1, p) && p.ok) {
				int id = g_dynNext ? DYN_B : DYN_A;
				g_dynNext ^= 1;
				DynStart(g, p, "cone", pick, -1, id);
				SetCam(g, id); if (g_lastCam != id) DynAbort(g);
			}
		}
	}
	if (!g_dyn.on) return;
	if (!S.dynCams || g_rp.phase || g_youDrive || g_driveCar >= 0) { DynEnd(g, false, "off"); return; }
	if (g_lastCam != g_dyn.camId) {
		if (now - g_dyn.began > 1.5) DynEnd(g, false, "replaced");     // somebody else changed the camera
		else if (now - g_dyn.began > 1.2) DynAbort(g);                // the cut never happened
		return;
	}
	double dt = std::min(0.1, std::max(0.001, now - g_dyn.lastT));
	g_dyn.lastT = now;
	DynShot &s = g_dyn;
	// the aim point: the car (a battle: between the pair when they are close; a crash: the middle of the wreck)
	float ax, ay, az;
	if (!DynCarPos(g, s.car, ax, ay, az)) { DynEnd(g, true, "car gone"); return; }
	double vx = g_car[s.car].speed * cos(g_car[s.car].head), vy = g_car[s.car].speed * sin(g_car[s.car].head);
	ax += (float)(vx * 0.10); ay += (float)(vy * 0.10);
	if (s.role == "battle" && s.other >= 0) {
		float bx, by, bz;
		if (DynCarPos(g, s.other, bx, by, bz) && hypot(bx - ax, by - ay) < 28) { ax = (ax + bx) / 2; ay = (ay + by) / 2; az = (az + bz) / 2; }
	} else if (s.role == "crash") {
		double sx = ax, sy = ay; int n = 1;
		for (int j : g_order) {
			if (j == s.car || !g_car[j].valid || g_car[j].pit) continue;
			float x, y, z;
			if (DynCarPos(g, j, x, y, z) && hypot(x - ax, y - ay) < 60 && (g_car[j].speed < 12 || g_car[j].incidentAt > 0)) { sx += x; sy += y; n++; }
		}
		ax = (float)(sx / n); ay = (float)(sy / n);
	}
	az += 0.8f;
	double wantYaw = atan2(ay - s.cy, ax - s.cx), dist2 = hypot(ax - s.cx, ay - s.cy);
	double wantPitch = atan2(s.cz - az, std::max(1.0, dist2));
	double stepY = 0;
	if (s.rig) {
		// the rig (its own thread) does the aim: here only what it reports, and who it follows when a wreck grows
		if (!g_rigAlive && now - s.rigStarted > 0.5) { Log("           the follow rig isn't running - the main loop aims this camera\n"); DynRigStop(g); }
		else {
			if (g_rigValid) { s.yaw = g_rigYaw; s.pitch = g_rigPitch; s.fov = g_rigFov / DEG; }
			stepY = g_rigYawRate * DEG * dt;
			g_liveTune[s.camId] = DynTune(s);               // (what CamWork would write - it leaves this block to the rig)
			if (s.role == "crash" && now - s.rigRefresh > 0.5) {
				s.rigRefresh = now;
				RigCmd c; int n, sl[6], md; DynRigTargets(g, s, n, sl, md);
				bool same = n == s.rigN; for (int i = 0; same && i < n; i++) same = sl[i] == s.rigSlots[i];
				if (!same) {                                // the wreck changed: tell the rig (the camera's place and lens as they are)
					EnterCriticalSection(&g_rigCs); c = g_rigCmd; LeaveCriticalSection(&g_rigCs);
					c.n = n; c.p.mode = md; for (int i = 0; i < n; i++) c.slots[i] = sl[i];
					RigPost(g, c); s.rigN = n; for (int i = 0; i < n; i++) s.rigSlots[i] = sl[i];
				}
			}
		}
	}
	if (!s.rig) {
		double dy = wantYaw - s.yaw; while (dy > CS_PI) dy -= 2 * CS_PI; while (dy < -CS_PI) dy += 2 * CS_PI;
		double dp = wantPitch - s.pitch;
		double k = 1 - exp(-dt * 14), maxStep = 3.8 * dt;                // eased, and never faster than ~220 degrees a second
		stepY = std::max(-maxStep, std::min(maxStep, dy * k)); double stepP = std::max(-maxStep, std::min(maxStep, dp * k));
		s.yaw += (float)stepY; s.pitch += (float)stepP;
		while (s.yaw > CS_PI) s.yaw -= (float)(2 * CS_PI); while (s.yaw < -CS_PI) s.yaw += (float)(2 * CS_PI);
		g_liveTune[s.camId] = DynTune(s);
	}
	// how it is looking: the car at a good size, not a whip pan, nobody in the way
	{
		double d3 = hypot(dist2, s.cz - az), width = 2 * d3 * tan(s.fov * DEG / 2);       // how much of the track the lens takes in at the car
		double ld = log(std::max(1.0, width) / 42.0), qd = exp(-ld * ld / (2 * 0.55 * 0.55));
		double rate = fabs(stepY) / dt / DEG, qp = std::max(0.0, std::min(1.0, 1 - std::max(0.0, rate - 60) / 140));
		if (now - s.lastBlockCheck > 0.25) {
			s.lastBlockCheck = now; s.blocked = false;
			float cx = (float)ax - s.cx, cy = (float)ay - s.cy, l2 = cx * cx + cy * cy;
			if (l2 > 400)
				for (int j : g_order) {
					if (j == s.car || j == s.other || !g_car[j].valid || g_car[j].pit) continue;
					float x, y, z; if (!DynCarPos(g, j, x, y, z)) continue;
					float t = ((x - s.cx) * cx + (y - s.cy) * cy) / l2;
					if (t <= 0.08f || t >= 0.92f) continue;
					float px = s.cx + t * cx - x, py = s.cy + t * cy - y;
					if (px * px + py * py < 2.0f * 2.0f && z < s.cz + 1.0f) { s.blocked = true; break; }
				}
		}
		s.qSum += (0.5 * qd + 0.3 * qp + 0.2 * (s.blocked ? 0.0 : 1.0)) * dt; s.wSum += dt;
	}
	// the end: hold time, or the car has come by
	double carD = DynCarD(g, s.car), along = carD >= 0 && g_tvLen > 0 ? DynWrap(s.camD - carD, g_tvLen) : 1e9;   // + = still short of the camera
	if (now - s.began > s.hold) { DynEnd(g, true, "held long enough"); return; }
	if (s.t && s.t->end != 0 && now - s.began > 1.0) {
		if (along < -s.endM) { DynEnd(g, true, s.endM >= 0 ? "the car has passed" : "the car is on top of it"); return; }
	}
	if (s.role == "pit_stall" && g_car[s.car].pitPhase != 2 && now - s.began > 2.0) { DynEnd(g, true, "left the stall"); return; }
	if (s.role == "pit_in" && g_car[s.car].pitPhase >= 2 && now - s.began > 2.0) { DynEnd(g, false, "in the stall"); return; }
	if (g_car[s.car].pit && s.role == "battle") { DynEnd(g, true, "pitted"); return; }
}

// ---- learning from the race (after each poll)
static void DynLearnTick(Game &g) {
	static int prevPhase[NUM_SLOTS]; static bool init = false; static size_t crashN = 0; static int prevFlag = -1; static double lastLat = 0;
	if (!init) { init = true; for (int &p : prevPhase) p = 0; }
	if (!S.dynLearn || g_trackName.empty() || g_tvLen <= 0) return;
	if (g_dm.track != g_trackName) {
		DynSave(); DynLoad(g_trackName); g_dm.lap = g_tvLen; g_dynRcTrack.clear();
		for (int &p : prevPhase) p = 0;
		crashN = g_crashes.size(); prevFlag = -1;
	}
	if (!DynGeoOk(g)) return;
	DynPitCams(g);
	double now = Now();
	bool caution = g_flag != FLAG_GREEN && g_session == SESSION_RACE;
	if (caution && prevFlag == FLAG_GREEN) {
		g_dm.cautions++; int cars = 0; for (int i : g_order) if (g_car[i].valid && !g_car[i].pit) cars++;
		g_dm.cautionCars += cars; g_dm.dirty = true;
	}
	prevFlag = g_flag;
	for (int i : g_order) {
		Car &c = g_car[i];
		if (!c.valid) { prevPhase[i] = 0; continue; }
		int ph = c.pitPhase;
		if (ph != prevPhase[i]) {
			float x, y, z; double d, lat; float dl = DynCarD(g, i);
			bool have = DynCarPos(g, i, x, y, z) && DynProject(x, y, d, lat);
			if (ph == 1 && prevPhase[i] == 0 && dl >= 0) { DynKeep(g_dm.pitIn, dl); if (caution) { g_dm.pitters++; g_dynPitSeenFor = g_greens; } }
			if (ph == 2 && have) {
				g_dm.stalls.push_back({ x, y }); if (g_dm.stalls.size() > 120) g_dm.stalls.erase(g_dm.stalls.begin());
				g_dm.sideVotes[lat >= 0 ? 1 : 0]++; g_dm.dirty = true;
			}
			if (ph == 0 && prevPhase[i] == 3 && dl >= 0) DynKeep(g_dm.pitOut, dl);
			prevPhase[i] = ph;
		}
	}
	if (now - lastLat > 0.5) {                                       // where the cars drive (racing, not in the pits)
		lastLat = now;
		if (g_flag == FLAG_GREEN)
			for (int i : g_order) {
				Car &c = g_car[i];
				if (!c.valid || c.pit || c.speed < 30) continue;
				float x, y, z; double d, lat;
				if (DynCarPos(g, i, x, y, z) && DynProject(x, y, d, lat)) { int b = std::min(40, (int)fabs(lat)); g_dm.latHist[b]++; g_dm.latN++; g_dm.dirty = true; }
			}
	}
	if (g_crashes.size() > crashN) {                                 // a new crash: where
		for (size_t k = crashN; k < g_crashes.size(); k++) {
			const Crash &cr = g_crashes[k];
			int car = cr.impactCar >= 0 ? cr.impactCar : (cr.cars.empty() ? -1 : cr.cars[0]);
			float x, y, z;
			if (car < 0 || !DynCarPos(g, car, x, y, z)) continue;
			bool merged = false;
			for (auto &s : g_dm.crashes) if (hypot(s[0] - x, s[1] - y) < 45) { s[0] = (s[0] * s[2] + x) / (s[2] + 1); s[1] = (s[1] * s[2] + y) / (s[2] + 1); s[2] += 1; merged = true; break; }
			if (!merged) g_dm.crashes.push_back({ x, y, 1.f });
			g_dm.dirty = true;
		}
		crashN = g_crashes.size();
	}
	if (g_dm.dirty && now - g_dm.savedAt > 60) DynSave();
}

// ---- the director page's calls
static std::string DynLiveJson() {                                  // part of /api/director/live
	char b[400];
	if (g_dyn.on)
		snprintf(b, sizeof b, "\"dynOn\":%s,\"dyn\":{\"on\":true,\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"yaw\":%.4f,\"fov\":%.1f,\"cam\":%d,\"role\":\"%s\",\"tmpl\":\"%s\",\"key\":\"%s\"},",
		         S.dynCams ? "true" : "false", g_dyn.cx, g_dyn.cy, g_dyn.cz, g_dyn.yaw, g_dyn.fov, g_dyn.camId, g_dyn.role.c_str(), g_dyn.t ? g_dyn.t->id : "", g_dyn.key.c_str());
	else snprintf(b, sizeof b, "\"dynOn\":%s,\"dyn\":null,", S.dynCams ? "true" : "false");
	return b + std::string("\"dynLabel\":") + J(g_dyn.on ? g_dyn.label : "") + ",";
}
static bool DynPoseFor(int cam, float *pose, std::string &label) {  // the timeline's events carry where the camera stood
	if (!g_dyn.on || cam != g_dyn.camId) return false;
	pose[0] = g_dyn.cx; pose[1] = g_dyn.cy; pose[2] = g_dyn.cz; pose[3] = g_dyn.yaw; pose[4] = g_dyn.fov;
	label = g_dyn.label; return true;
}
static std::string DynInfoBody(Game &g);
static std::string DynInfoJson(Game &g) { EnterCriticalSection(&g_lock); std::string o = DynInfoBody(g); LeaveCriticalSection(&g_lock); return o; }
static std::string DynInfoBody(Game &g) {
	double inD = -1, outD = -1; bool pit = g.h ? DynPitEnds(inD, outD) : false;
	std::string o = "{\"on\":" + std::string(S.dynCams ? "true" : "false") + ",\"track\":" + J(g_dm.track) + ",\"learning\":" + (S.dynLearn ? "true" : "false") +
	                ",\"pitIn\":" + (pit ? TlNum(inD, 0) : "null") + ",\"pitOut\":" + (pit ? TlNum(outD, 0) : "null") +
	                ",\"pitKnown\":" + (g_dm.pitIn.size() >= 2 ? "true" : "false") + ",\"pitSide\":" + std::to_string(g_game && g_game->h ? DynPitSide() : 0) +
	                ",\"halfWidth\":" + TlNum(DynHalfWidth(), 1) + ",\"widthKnown\":" + (g_dm.latN >= 400 ? "true" : "false") +
	                ",\"stalls\":" + std::to_string(g_dm.stalls.size()) + ",\"crashes\":" + std::to_string(g_dm.crashes.size()) +
	                ",\"cautions\":" + std::to_string(g_dm.cautions) + ",\"pitRate\":" + TlNum(g_dm.cautionCars ? 100.0 * g_dm.pitters / g_dm.cautionCars : -1, 0) +
	                ",\"spots\":[";
	for (size_t i = 0; i < g_dm.crashes.size(); i++) o += std::string(i ? "," : "") + "[" + TlNum(g_dm.crashes[i][0], 0) + "," + TlNum(g_dm.crashes[i][1], 0) + "," + TlNum(g_dm.crashes[i][2], 0) + "]";
	o += "],\"stallSpots\":[";
	for (size_t i = 0; i < g_dm.stalls.size(); i++) o += std::string(i ? "," : "") + "[" + TlNum(g_dm.stalls[i][0], 0) + "," + TlNum(g_dm.stalls[i][1], 0) + "]";
	o += "],\"templates\":[";
	bool first = true;
	for (const DynT &t : DYN_T) {
		for (int side : { 1, -1 }) {
			auto it = g_dm.arms.find(std::string(t.id) + "|" + DynSideName(side));
			o += std::string(first ? "" : ",") + "{\"id\":" + J(t.id) + ",\"label\":" + J(t.label) + ",\"role\":" + J(t.role) + ",\"side\":" + J(DynSideName(side)) +
			     ",\"n\":" + std::to_string(it == g_dm.arms.end() ? 0 : it->second.n) + ",\"score\":" + (it == g_dm.arms.end() ? "null" : TlNum(it->second.mean, 2)) + "}";
			first = false;
		}
	}
	o += "],\"templateSpecs\":[";
	first = true;
	for (const DynT &t : DYN_T) {
		char b[360];
		snprintf(b, sizeof b, "%s{\"id\":\"%s\",\"label\":\"%s\",\"role\":\"%s\",\"anchor\":%d,\"ahead\":[%.1f,%.1f],\"lat\":[%.1f,%.1f],\"height\":[%.1f,%.1f],\"fov\":[%.1f,%.1f],\"end\":%.1f,\"hold\":%.1f}",
		         first ? "" : ",", t.id, t.label, t.role, (int)t.anchor, t.ahead.lo, t.ahead.hi, t.lat.lo, t.lat.hi, t.height.lo, t.height.hi, t.fov.lo, t.fov.hi, t.end, t.holdMax);
		o += b; first = false;
	}
	o += "]}";
	return o;
}
// a test shot: a camera 24 m from the car on air, 3 m up, aimed at it for 8 s - to see the aim is right (dyn_yaw_fix, dyn_pitch_sign)
static std::string DynTest(Game &g) {
	if (!g.camOk || g_rp.phase) return "NR2003 isn't ready for a camera change";
	if (!S.customCams) return "Own angles are off (Cameras page)";
	if (!DynGeoOk(g)) return "no track map yet";
	int car = g_shot.car >= 0 && g_car[g_shot.car].valid ? g_shot.car : (g_score.empty() ? -1 : g_score[0]);
	if (car < 0) return "no car on track";
	if (g_dyn.on) DynEnd(g, false, "test");
	DynPitCams(g);
	DynPlan p;
	if (!DynPlace(g, "test", car, -1, p, &DYN_TEST) || !p.ok) return "couldn't place it";
	int id = g_dynNext ? DYN_B : DYN_A; g_dynNext ^= 1;
	DynStart(g, p, "test", car, -1, id);
	g_shot.car = car; g_shot.kind = kManual; g_shot.other = -1; g_shot.since = Now() - std::max(0.0, S.manualHold - 9); g_shot.why = "dynamic camera test";
	int c = id;
	if (!SetShotCam(g, c, car)) { DynAbort(g); return "the camera change didn't go through"; }
	g_lastCam = id;
	return "";
}
static std::string DynApi(Game &g, const std::string &what, std::map<std::string, std::string> &q, int &code) {
	code = 200;
	if (what == "") return DynInfoJson(g);
	std::string err;
	EnterCriticalSection(&g_lock);
	if (what == "test") err = DynTest(g);
	else if (what == "rate") {
		int v = atoi(q["v"].c_str());
		std::string key = g_dyn.on ? g_dyn.key : g_dynLastKey;
		if (key.empty()) err = "no dynamic shot to rate yet";
		else {
			// a thumbs down also pulls that angle in on this track (the scenery is out there); a thumbs up lets it back out
			std::string id = key.substr(0, key.find('|'));
			float r = DynReach(id.c_str());
			g_dm.reach[id] = v > 0 ? std::min(1.f, r * 1.1f) : std::max(0.3f, r * 0.8f); g_dm.dirty = true;
			DynScoreShot(key, v > 0 ? 1.0 : 0.0, 3); Log("Dynamic cameras: %s %s\n", v > 0 ? "thumbs up for" : "thumbs down for", key.c_str()); }
	} else if (what == "forget") {
		std::string t = g_dm.track; g_dm = DynMem(); g_dm.track = t; g_dm.dirty = true; g_dm.savedAt = 0; DynSave();
		Log("Dynamic cameras: forgot what was learnt about %s\n", t.c_str());
	} else err = "unknown";
	LeaveCriticalSection(&g_lock);
	if (!err.empty()) { code = 400; return "{\"error\":" + J(err) + "}"; }
	return "{\"ok\":true}";
}

static void DynGone() {                                             // NR2003 closed: its blocks went with it
	DynSave();
	RigShutdown();
	g_dyn.on = false; g_dynTouched.clear(); g_dynCommitFor = g_dynFinishFor = g_dynPitSeenFor = -1;
	g_dg.ok = false; g_dg.pitCamOk = false;
}

// ---- self-test (RaceVision.exe --selftest out.txt <track>): the placement geometry on a real recorded track map, no game needed
static void DynSelfTest(FILE *out, const std::string &track) {
	int ok = 0, bad = 0;
	auto check = [&](bool c, const std::string &what) { if (c) ok++; else { bad++; fprintf(out, "FAIL  %s\n", what.c_str()); } };
	Game gx;                                                       // no game attached: the tables below stand in for its memory
	TrackMapPts m;
	if (!LoadTrackMap(track, m)) { fprintf(out, "dynamic cameras: no track map for %s - skipped\n", track.c_str()); return; }
	g_trackName = track; g_tvTrack = track; g_tvLen = m.len;
	g_dg = DynGeo(); g_dm = DynMem(); g_dm.track = track; g_dynRecent.clear(); g_dynNoUntil = 0;
	check(DynGeoOk(gx), "the map loads");
	g_dynSim = true; S.customCams = true; S.dynCams = true; S.dynChance = 100;
	g_order.clear();
	for (int i = 0; i < 12; i++) {
		double d = fmod(150.0 + i * 55.0, m.len), lat = ((i % 3) - 1) * 3.0, tx, ty; float x, y;
		DynAt(d, lat, x, y); DynTangent(d, tx, ty);
		Car &c = g_car[i]; c = Car(); c.valid = true; c.speed = 55; c.head = (float)atan2(ty, tx); c.pit = false;
		g_dynSimXYZ[i][0] = x; g_dynSimXYZ[i][1] = y; g_dynSimXYZ[i][2] = 5.f; g_dynSimD[i] = (float)d;
		g_order.push_back(i);
	}
	double hw = DynHalfWidth();
	fprintf(out, "dynamic cameras on %s: lap %.0f m, half width %.1f m\n", track.c_str(), m.len, hw);
	g_dm.pitIn = { (float)(m.len * 0.82), (float)(m.len * 0.82) }; g_dm.pitOut = { (float)(m.len * 0.11), (float)(m.len * 0.11) };
	g_dm.sideVotes[1] = 5;                                          // pit road on the left
	const char *roles[] = { "battle", "restart", "finish", "pit_in", "pit_stall", "pit_out", "crash" };
	for (const char *role : roles) {
		std::set<std::string> seen; std::string last, last2; int runs = 0;
		for (int k = 0; k < 40; k++) {
			DynPlan p; int car = 3, other = 4;
			bool placed = DynPlace(gx, role, car, other, p);
			check(placed && p.ok, std::string(role) + ": a camera is placed");
			if (!placed) break;
			DynStart(gx, p, role, car, other, DYN_A);
			const DynT &t = *p.t;
			float ax = g_dynSimXYZ[car][0], ay = g_dynSimXYZ[car][1];
			double want = atan2(ay - g_dyn.cy, ax - g_dyn.cx), diff = fabs(DynWrap(want - g_dyn.yaw, 2 * CS_PI));
			check(diff < 0.002, std::string(role) + ": the camera is aimed at the car (yaw)");
			double look = atan2(g_dyn.cz - (g_dynSimXYZ[car][2] + 0.8), std::max(1.0, (double)hypot(ax - g_dyn.cx, ay - g_dyn.cy)));
			check(fabs(look - g_dyn.pitch) < 0.002, std::string(role) + ": tilted at the car (pitch)");
			check(g_dyn.fov >= t.fov.lo - 0.01 && g_dyn.fov <= t.fov.hi + 0.01, std::string(role) + ": lens in range");
			check(g_dyn.hold <= t.holdMax + 0.01 && g_dyn.hold >= 2, std::string(role) + ": hold time in range");
			float hgt = g_dyn.cz - g_dynSimXYZ[car][2];
			check(hgt >= t.height.lo - 0.01 && hgt <= t.height.hi + 0.01, std::string(role) + ": height in range");
			if (t.anchor == daTarget || t.anchor == daPitIn || t.anchor == daPitOut || t.anchor == daLine) {
				double d, lat;
				check(DynProject(g_dyn.cx, g_dyn.cy, d, lat) && fabs(lat) > hw + std::min(t.lat.lo, DYN_MAX_OUT) * DynReach(t.id) - 2.5, std::string(role) + "/" + t.id + ": the camera stands outside the track");
				check(lat * p.side > 0, std::string(role) + "/" + t.id + ": on the side it chose");
			}
			if ((t.anchor == daPitIn || t.anchor == daPitOut) && p.side == DynPitSide()) { double d, lat; DynProject(g_dyn.cx, g_dyn.cy, d, lat); check(lat > 0, std::string(role) + ": at pit road's side (left)"); }
			if (t.anchor == daTarget) {                            // it is ahead of the car by the lead time
				double along = DynWrap(g_dyn.camD - g_dynSimD[car], m.len), lead = along / 55.0;
				check(lead >= t.ahead.lo - 0.05 && lead <= t.ahead.hi + 0.05, std::string(role) + "/" + t.id + ": lead time in range");
			}
			seen.insert(std::string(t.id) + "|" + DynSideName(p.side));
			if (last == t.id && last2 == t.id) runs++;
			last2 = last; last = t.id;
			g_dyn.on = false;
		}
		fprintf(out, "  %-9s %zu different (template, side) over 40 shots\n", role, seen.size());
		if (!strcmp(role, "battle")) check(seen.size() >= 4, "battle shots vary (>= 4 different template / side combinations in 40)");
		if (!strcmp(role, "battle") || !strcmp(role, "crash")) check(runs == 0, std::string(role) + ": never the same template three times running");
	}
	// learning: the angle that scored well is chosen more, and the others are still tried
	{
		g_dm.arms.clear();
		for (const DynT &t : DYN_T) for (int s : { 1, -1 }) g_dm.arms[std::string(t.id) + "|" + DynSideName(s)] = { 20, 0.25 };
		g_dm.arms["side_low|L"] = { 20, 0.95 };
		std::map<std::string, int> count; int total = 120;
		for (int k = 0; k < total; k++) {
			DynPlan p; if (!DynPlace(gx, "battle", 3, 4, p)) break;
			g_dynRecent.push_back(p.t->id); if (g_dynRecent.size() > 6) g_dynRecent.erase(g_dynRecent.begin());
			count[std::string(p.t->id) + "|" + DynSideName(p.side)]++;
		}
		int best = count["side_low|L"], others = 0; for (auto &kv : count) if (kv.first != "side_low|L") others += kv.second;
		fprintf(out, "  learning: best-scored angle %d of %d, the others %d (%zu different)\n", best, total, others, count.size());
		check(best >= total * 0.30, "the angle that scored best is chosen far more than a fair share (fair would be 1 in 12)");
		check(others >= total * 0.10 && count.size() >= 4, "the other angles are still tried");
		// scoring moves the mean
		g_dm.arms.clear(); DynScoreShot("x|L", 1.0, 1); DynScoreShot("x|L", 0.0, 1);
		check(fabs(g_dm.arms["x|L"].mean - 0.5) < 1e-9 && g_dm.arms["x|L"].n == 2, "scores average");
		DynScoreShot("y|L", 1.0, 3); check(g_dm.arms["y|L"].mean == 1.0 && g_dm.arms["y|L"].n == 3, "a thumbs up counts three times");
	}
	// pit road from the stock pit cameras, before a single stop has been seen
	{
		CsSet st; std::string from;
		if (CsSource("track", track, st, from) && st.track) {
			g_dm = DynMem(); g_dm.track = track; g_dg.pitCamOk = true;
			for (int p = 0; p < 2; p++) for (int k = 0; k < 3; k++) g_dg.pitCam[p][k] = st.pit[p][k];
			double inD = 0, outD = 0; bool got = DynPitEnds(inD, outD);
			check(got, "pit road is estimated from the pit cameras");
			fprintf(out, "  pit road from the pit cameras (%s): in at %.0f m, out at %.0f m of a %.0f m lap, side %s\n", from.c_str(), inD, outD, m.len, DynPitSide() > 0 ? "left" : "right");
		}
	}
	g_dynSim = false;
	fprintf(out, "dynamic cameras self-test: %d ok, %d failed\n", ok, bad);
}
// the real JSON the page gets, written out for the page tests (a mock server serves these files)
static void CsDumpSamples(const std::string &dir, const std::string &track) {
	Game gx;
	std::map<std::string, std::string> q; int code;
	q["track"] = track;
	auto put = [&](const std::string &name, const std::string &body) { FILE *f = fopen((dir + "\\" + name).c_str(), "wb"); if (f) { fwrite(body.data(), 1, body.size(), f); fclose(f); } };
	put("camsets.json", CsApi(gx, "", q, false, code));
	for (auto &e : CsEntries("track", track)) {
		q["name"] = e.name;
		std::string n = e.name; for (char &c : n) if (c == ' ' || c == '+') c = '_';
		put("preview_" + n + ".json", CsApi(gx, "preview", q, false, code));
	}
	put("dyn.json", DynInfoBody(gx));
}
