// ---------------------------------------------------------------- cameras: RaceVision's own, and the live editor
// (2026-10-27) NR2003 keeps each camera's set-up in memory blocks that it reads every frame:
//   chase / Chase 2 (4, 6)  0xC5C190  10 floats {x ahead, y left, z, turn, tilt, roll, height, lens, -, -}
//   far chase / 2 (5, 7)    0xC5856C  same            rear chase (8)  0xC5C168  same
//   car cameras (10-15)     0xC57450 + ((make + class * 4) * 6 + cam - 10) * 36   9 floats {x, y, z, turn, tilt, roll, lens, -, -}
//                           (make / class from the car's record +0x592 / +0x596; one block per car model)
//   pit lane 1 / 2 (17, 18) 0xC57378 / 0xC5739C  9 floats, world position
//   in-car (1)              the eye offset 0x74EBBC {ahead, left, up}; turn / tilt / roll: a small patch adds RaceVision's
//                           angles to the in-car view's (the calls at 0x569C91 / 0x56AB9E); lens: see InCarLens
//   helicopter (16)         its height 0xC5C1B8 (the track's camera file sets it, 100 m on most)
// (FreeCamera, in the game's free_camera folder, edits the same car / pit blocks.) While a camera with an adjustment is
// on screen RaceVision writes it into its block; the game's own values go back the moment another camera is on.
// Adjustments are kept in racevision.ini in plain units (metres, degrees): chase2_view (6), far_chase2_view (7),
// incar3_eye (22), drone_height (23), cam_tune_<n> for the rest; RaceVision's own cameras 22.. run on a game camera
// ("base") with their own adjustment; cam_new_<n> = base, name for the ones made in the editor.

enum CamKind { ckNone, ckChase, ckCar, ckPit, ckInCar, ckHeli };
static bool DynRigOwns(DWORD block);              // racevision_rig.cpp: a dynamic camera's block, written by the rig's own thread
struct CamParam { const char *name, *unit; float step; };
static const CamParam P_CHASE[] = { { "back", "m", 0.25f }, { "side", "m", 0.25f }, { "height", "m", 0.1f }, { "turn", "deg", 2 },
	{ "tilt", "deg", 1 }, { "roll", "deg", 1 }, { "lens", "deg", 2 } };
static const CamParam P_CAR[] = { { "ahead", "m", 0.05f }, { "left", "m", 0.05f }, { "up", "m", 0.05f }, { "turn", "deg", 2 },
	{ "tilt", "deg", 1 }, { "roll", "deg", 1 }, { "lens", "deg", 2 } };
static const CamParam P_PIT[] = { { "x", "m", 0.5f }, { "y", "m", 0.5f }, { "z", "m", 0.25f }, { "turn", "deg", 2 },
	{ "tilt", "deg", 1 }, { "roll", "deg", 1 }, { "lens", "deg", 2 } };
static const CamParam P_INCAR[] = { { "ahead", "m", 0.02f }, { "left", "m", 0.02f }, { "up", "m", 0.02f }, { "turn", "deg", 2 },
	{ "tilt", "deg", 1 }, { "roll", "deg", 1 }, { "lens", "deg", 2 } };
static const CamParam P_HELI[] = { { "height", "m", 2 } };
static const DWORD CHASE_BLOCK = 0xC5C190, FAR_BLOCK = 0xC5856C, REAR_BLOCK = 0xC5C168, PIT1_BLOCK = 0xC57378, PIT2_BLOCK = 0xC5739C;
static const DWORD EYE_OFFSET = 0x74EBBC, HELI_HEIGHT = 0xC5C1B8, CAR_CAMS = 0xC57450, COCKPIT_FOV = 0x894EE8;
static float g_cockpitFov = 78;                  // your own cockpit field of view (degrees), read while RaceVision's isn't in
static const int OFF_POS = 6622;                 // car block: world position x, y (m)
static const float DEG = 3.14159265f / 180;
static std::vector<double> NumList(const std::string &v) {      // "1, -2.5, 3" -> numbers
	std::vector<double> n;
	const char *p = v.c_str();
	while (*p) {
		char *end; double x = strtod(p, &end);
		if (end == p) { p++; continue; }
		n.push_back(x); p = end;
	}
	return n;
}

static CamKind KindOf(int gameCam) {
	switch (gameCam) {
	case 4: case 5: case 6: case 7: case 8: return ckChase;
	case 10: case 11: case 12: case 13: case 14: case 15: return ckCar;
	case 17: case 18: return ckPit;
	case 1: return ckInCar;
	case 16: return ckHeli;
	}
	return ckNone;
}
static const CamParam *ParamsOf(CamKind k, int &n) {
	switch (k) {
	case ckChase: n = 7; return P_CHASE;
	case ckCar: n = 7; return P_CAR;
	case ckPit: n = 7; return P_PIT;
	case ckInCar: n = 7; return P_INCAR;
	case ckHeli: n = 1; return P_HELI;
	default: n = 0; return nullptr;
	}
}
static const char *CamKindName(CamKind k) {
	return k == ckChase ? "chase" : k == ckCar ? "car" : k == ckPit ? "pit" : k == ckInCar ? "incar" : k == ckHeli ? "heli" : "";
}
// where the game camera's set-up lives (car cameras: the model of the car on screen); raw float count
static DWORD BlockOf(Game &g, int gameCam, int focus, int &nRaw) {
	nRaw = 0;
	switch (gameCam) {
	case 4: case 6: nRaw = 10; return CHASE_BLOCK;
	case 5: case 7: nRaw = 10; return FAR_BLOCK;
	case 8: nRaw = 10; return REAR_BLOCK;
	case 17: nRaw = 9; return PIT1_BLOCK;
	case 18: nRaw = 9; return PIT2_BLOCK;
	case 1: nRaw = 3; return EYE_OFFSET;
	case 16: nRaw = 1; return HELI_HEIGHT;
	}
	if (gameCam >= 10 && gameCam <= 15 && focus >= 0) {
		DWORD rec = REC0 + focus * REC_SIZE;
		int make = g.Get<int>(rec + 0x592, -1), cls = g.Get<int>(rec + 0x596, -1);
		if (make >= 0 && make < 4 && cls >= 0 && cls < 5) { nRaw = 9; return CAR_CAMS + ((make + cls * 4) * 6 + gameCam - 10) * 36; }
	}
	return 0;
}
// plain units <-> the block's floats (the floats not edited stay the game's own)
static void ToRaw(CamKind k, const std::vector<float> &v, float *raw) {
	if (k == ckChase) { raw[0] = -v[0]; raw[1] = v[1]; raw[6] = v[2]; raw[3] = v[3] * DEG; raw[4] = v[4] * DEG; raw[5] = v[5] * DEG; raw[7] = v[6] * DEG; }
	else if (k == ckCar || k == ckPit) { for (int i = 0; i < 3; i++) raw[i] = v[i]; for (int i = 3; i < 7; i++) raw[i] = v[i] * DEG; }
	else if (k == ckInCar) { for (int i = 0; i < 3; i++) raw[i] = v[i]; }
	else if (k == ckHeli) raw[0] = v[0];
}
static std::vector<float> FromRaw(CamKind k, const float *raw) {
	if (k == ckChase) return { -raw[0], raw[1], raw[6], raw[3] / DEG, raw[4] / DEG, raw[5] / DEG, raw[7] / DEG };
	if (k == ckCar || k == ckPit) return { raw[0], raw[1], raw[2], raw[3] / DEG, raw[4] / DEG, raw[5] / DEG, raw[6] / DEG };
	if (k == ckInCar) return { raw[0], raw[1], raw[2], 0, 0, 0, g_cockpitFov };
	if (k == ckHeli) return { raw[0] };
	return {};
}

// ---- RaceVision's own cameras
static const char *PresetTune(int id) {          // their adjustment when racevision.ini has none
	switch (id) {
	case 6: return "4.5, -2.2, 1.0, 26, 4";      // Chase 2: low rear quarter from the right
	case 7: return "15, 0, 7.5, 0, 20";          // Far chase 2: high trailing
	case 22: return "-0.25, -0.5, 0.05, 20";     // In-car right: right of the driver, turned in to the hands
	case 23: return "30";                        // Drone: the helicopter, low
	case 24: return "0.5, -5.5, 1.2, 90, 3";     // Side tracking: broadside from the right
	case 25: return "3.2, 0, 0.55, 0, 2";        // Low rear: bumper height behind the car
	case 26: return "-4.5, -2.5, 1.0, 151, 4";   // Front quarter: ahead on the right, looking back at the car
	case 27: return "0.5, 0, 16, 0, 80";         // Overhead: straight down
	case 28: return "32, 0, 10, 0, 12";          // Pack view: far back and up, the cars around it
	}
	return "";
}
static std::string TuneKey(int id) {
	return id == 6 ? "chase2_view" : id == 7 ? "far_chase2_view" : id == 22 ? "incar3_eye" : id == 23 ? "drone_height" :
	       "cam_tune_" + std::to_string(id);
}
static void LoadCustomCams() {                   // the ones made in the editor: cam_new_<id> = base, name
	g_vcams.erase(std::remove_if(g_vcams.begin(), g_vcams.end(), [](const VCam &v) { return !v.preset; }), g_vcams.end());
	for (auto &kv : S.raw) {
		if (kv.first.rfind("cam_new_", 0) != 0 || kv.second.empty()) continue;
		int id = atoi(kv.first.c_str() + 8), base = atoi(kv.second.c_str());
		size_t c = kv.second.find(',');
		std::string name = c == std::string::npos ? "" : Trim(kv.second.substr(c + 1));
		if (id > 28 && id <= CAM_MAX && KindOf(base) != ckNone && !VCamById(id))
			g_vcams.push_back({ id, base, name.empty() ? "Custom " + std::to_string(id) : name, false });
	}
	std::sort(g_vcams.begin(), g_vcams.end(), [](const VCam &a, const VCam &b) { return a.id < b.id; });
}

// ---- adjustments: saved (racevision.ini / preset) and live (being edited, not saved yet)
static std::map<int, std::vector<float>> g_liveTune;
static std::vector<float> SavedTune(int id, CamKind k, const std::vector<float> &stock) {
	auto it = S.raw.find(TuneKey(id));
	std::string v = it != S.raw.end() ? it->second : PresetTune(id);
	if (it != S.raw.end() && Trim(v).empty()) return {};              // reset: none
	std::vector<double> n = NumList(v);
	if (n.empty()) return {};
	int np; ParamsOf(k, np);
	std::vector<float> out(np);
	std::vector<double> pre = NumList(PresetTune(id));
	for (int i = 0; i < np; i++)                 // missing ones: the preset's, else the game's own
		out[i] = i < (int)n.size() ? (float)n[i] : i < (int)pre.size() ? (float)pre[i] : i < (int)stock.size() ? stock[i] : 0;
	return out;
}

// ---- writing them while the camera is on screen
struct CamNow { int id = -1, gameCam = -1, focus = -1; CamKind kind = ckNone; DWORD block = 0; int nRaw = 0; bool driving = false; };
static CamNow g_camNow;                          // what CamWork saw last (the editor works on it)
static std::map<DWORD, std::vector<float>> g_stockRaw;   // the game's own floats of each block RaceVision changed
static std::map<DWORD, int> g_written;           // blocks holding RaceVision's values now
static std::vector<float> StockOf(Game &g, DWORD block, int nRaw) {
	auto it = g_stockRaw.find(block);
	if (it != g_stockRaw.end()) return it->second;
	if (block == EYE_OFFSET) return g_stockRaw[block] = { 0.1f, 0.0f, 0.1f };   // NR2003.exe's own (not reloaded per session)
	std::vector<float> raw(nRaw);
	if (!g.Read(block, raw.data(), nRaw * 4)) return {};
	g_stockRaw[block] = raw;
	return raw;
}

// In-car turn / tilt / roll: the in-car view's angles are made by 0x5F6F20(matrix, turn, tilt, roll) (radians). Two
// camera routines call it: 0x569B10 (the camera object, call at 0x569C91) and 0x56A9A0 (the picture you see, reached
// through a function pointer at 0x38B2970; call at 0x56AB9E - live 2026-10-27: patching only the first did nothing).
// Both calls go into one cave that adds RaceVision's three angles (floats at cave+0x40, 0 = the game's own view) and
// jumps on to 0x5F6F20. (new19 had a turn-only patch at 0x569C7E: left in place if found, its turn kept at 0.)
static DWORD g_viewCave = 0;
static const DWORD VIEW_SITES[] = { 0x569C91, 0x56AB9E }, VIEW_FN = 0x5F6F20, OLD_TURN_AT = 0x569C7E;
static const DWORD VIEW_MAGIC = 0x32495652;      // "RVI2" at cave+0x3C
static bool ViewPatch(Game &g) {
	if (g_viewCave) return true;
	unsigned char cur[6];
	if (g.Read(OLD_TURN_AT, cur, 6) && cur[0] == 0xE8) {    // new19's turn cave ("RVIT" at +16): its turn to none
		DWORD rel; memcpy(&rel, cur + 1, 4);
		DWORD old = OLD_TURN_AT + 5 + rel;
		if (g.Get<DWORD>(old + 16, 0) == 0x54495652) { float t = 0; g.Write(old + 20, *(int *)&t); }
	}
	// each site calls 0x5F6F20 (not patched yet) or a cave of ours (RaceVision restarted: use that cave)
	DWORD cave = 0, to[2] = {};
	for (int i = 0; i < 2; i++) {
		if (!g.Read(VIEW_SITES[i], cur, 5) || cur[0] != 0xE8) return false;
		DWORD rel; memcpy(&rel, cur + 1, 4);
		to[i] = VIEW_SITES[i] + 5 + rel;
		if (to[i] == VIEW_FN) continue;
		if (g.Get<DWORD>(to[i] + 0x3C, 0) != VIEW_MAGIC) return false;   // someone else's patch: leave it
		cave = to[i];
	}
	if (!cave) {
		cave = (DWORD)(UINT_PTR)VirtualAllocEx(g.h, NULL, 0x50, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!cave) return false;
		unsigned char c[0x50] = {};
		int n = 0;
		for (int i = 0; i < 3; i++) {            // fld [esp+8+4i]; fadd [cave+40h+4i]; fstp [esp+8+4i]
			unsigned char sp = (unsigned char)(8 + 4 * i);
			DWORD f = cave + 0x40 + 4 * i;
			c[n++] = 0xD9; c[n++] = 0x44; c[n++] = 0x24; c[n++] = sp;
			c[n++] = 0xD8; c[n++] = 0x05; memcpy(c + n, &f, 4); n += 4;
			c[n++] = 0xD9; c[n++] = 0x5C; c[n++] = 0x24; c[n++] = sp;
		}
		c[n++] = 0xE9; DWORD j = VIEW_FN - (cave + n + 4); memcpy(c + n, &j, 4); n += 4;   // jmp 0x5F6F20
		memcpy(c + 0x3C, &VIEW_MAGIC, 4);
		SIZE_T w = 0;
		if (!WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)cave, c, sizeof c, &w)) return false;
	}
	bool ok = true;
	for (int i = 0; i < 2; i++) {
		if (to[i] != VIEW_FN) continue;
		DWORD at = VIEW_SITES[i];
		unsigned char call[5] = { 0xE8 };
		DWORD r = cave - (at + 5); memcpy(call + 1, &r, 4);
		DWORD prot = 0; SIZE_T w = 0;
		if (!VirtualProtectEx(g.h, (LPVOID)(UINT_PTR)at, 5, PAGE_EXECUTE_READWRITE, &prot)) { ok = false; continue; }
		ok &= WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)at, call, 5, &w) && w == 5;
		VirtualProtectEx(g.h, (LPVOID)(UINT_PTR)at, 5, prot, &prot);
		FlushInstructionCache(g.h, (LPCVOID)(UINT_PTR)at, 5);
	}
	g_viewCave = cave;                           // (the first site may hold it even if the second failed)
	Log(ok ? "Cameras: in-car turn / tilt / roll ready.\n" : "Cameras: in-car turn / tilt / roll - COULDN'T patch the game.\n");
	return true;
}
// NR2003 keeps the last view it built (0xC5C1C8, flag byte 0xC5C279, camera 0xC5C271, car 0xC5C275; 0x56AA15 / 0x56B66C)
// and reuses it while the camera and car stay the same - so a lens / angle / eye change only showed after the next cut
// (user 2026-11-01: "FOV doesn't change in the real time cam"). Any change of ours clears the flag for a few loops.
static const DWORD VIEW_CACHE_ON = 0xC5C279;
static int g_viewDirty = 0;
static void ViewCacheClear(Game &g) {
	static DWORD okPid = 0; static bool ok = false;
	if (okPid != g.pid) {                        // the game's code reads that flag where we think: mov al, [0xC5C279]
		unsigned char b[5]; okPid = g.pid;
		ok = g.Read(0x56AA15, b, 5) && b[0] == 0xA0 && *(DWORD *)(b + 1) == VIEW_CACHE_ON;
	}
	if (!ok) return;
	unsigned char z = 0; SIZE_T w = 0;
	WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)VIEW_CACHE_ON, &z, 1, &w);
}
static float g_viewLast[3] = { 1e9f, 1e9f, 1e9f };
static void ViewAngles(Game &g, float turn, float tilt, float roll) {   // degrees
	if (!g_viewCave && (turn != 0 || tilt != 0 || roll != 0)) ViewPatch(g);
	if (!g_viewCave) return;
	float *last = g_viewLast;
	float a[3] = { turn * DEG, tilt * DEG, roll * DEG };
	if (!memcmp(a, last, sizeof a)) return;
	SIZE_T w = 0;
	if (WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)(g_viewCave + 0x40), a, sizeof a, &w)) { memcpy(last, a, sizeof a); g_viewDirty = 3; }
}
// The in-car lens. The picture's routine (0x56A9A0, in-car case) takes the lens from your cockpit field of view setting
// (0x411850 = [0x894EE8] degrees x DEG, called at 0x56AC30 and 0x56AC4B) and then holds every lens to 0.25..110 degrees
// (0x56B3AB fmul [0x6EE574] = 110). new21 wrote the setting itself and nothing changed (live 2026-10-27). Now both calls go
// into a cave: RaceVision's lens (radians at cave+0x40, 0 = yours) or, when none, on to 0x411850; and the 110 cap reads
// 150 from cave+0x44 instead. Your own setting is never touched.
static DWORD g_lensCave = 0;
static const DWORD LENS_CALLS[] = { 0x56AC30, 0x56AC4B }, LENS_FN = 0x411850, LENS_CAP_AT = 0x56B3AB, LENS_CAP_GAME = 0x6EE574;
static const DWORD LENS_MAGIC = 0x314C5652;      // "RVL1" at cave+0x3C
static bool g_fovIn = false;                     // RaceVision's lens is in
static bool LensPatch(Game &g) {
	if (g_lensCave) return true;
	unsigned char cur[6];
	DWORD cave = 0, to[2] = {};
	for (int i = 0; i < 2; i++) {
		if (!g.Read(LENS_CALLS[i], cur, 5) || cur[0] != 0xE8) return false;
		DWORD rel; memcpy(&rel, cur + 1, 4);
		to[i] = LENS_CALLS[i] + 5 + rel;
		if (to[i] == LENS_FN) continue;
		if (g.Get<DWORD>(to[i] + 0x3C, 0) != LENS_MAGIC) return false;   // someone else's patch: leave it
		cave = to[i];
	}
	if (!g.Read(LENS_CAP_AT, cur, 6) || cur[0] != 0xD8 || cur[1] != 0x0D) return false;
	if (!cave) {
		cave = (DWORD)(UINT_PTR)VirtualAllocEx(g.h, NULL, 0x50, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!cave) return false;
		unsigned char c[0x50] = {};
		int n = 0;
		DWORD lens = cave + 0x40;
		c[n++] = 0xA1; memcpy(c + n, &lens, 4); n += 4;        // mov eax, [lens]
		c[n++] = 0x85; c[n++] = 0xC0;                          // test eax, eax
		c[n++] = 0x74; c[n++] = 0x07;                          // jz game
		c[n++] = 0xD9; c[n++] = 0x05; memcpy(c + n, &lens, 4); n += 4;   // fld dword [lens]
		c[n++] = 0xC3;                                         // ret
		c[n++] = 0xE9; DWORD j = LENS_FN - (cave + n + 4); memcpy(c + n, &j, 4); n += 4;   // game: jmp 0x411850
		memcpy(c + 0x3C, &LENS_MAGIC, 4);
		float cap = 150; memcpy(c + 0x44, &cap, 4);
		SIZE_T w = 0;
		if (!WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)cave, c, sizeof c, &w)) return false;
	}
	auto code = [&](DWORD at, const void *b, SIZE_T len) {
		DWORD prot = 0; SIZE_T w = 0;
		if (!VirtualProtectEx(g.h, (LPVOID)(UINT_PTR)at, len, PAGE_EXECUTE_READWRITE, &prot)) return false;
		bool ok = WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)at, b, len, &w) && w == len;
		VirtualProtectEx(g.h, (LPVOID)(UINT_PTR)at, len, prot, &prot);
		FlushInstructionCache(g.h, (LPCVOID)(UINT_PTR)at, len);
		return ok;
	};
	bool ok = true;
	for (int i = 0; i < 2; i++) {
		if (to[i] != LENS_FN) continue;
		unsigned char call[5] = { 0xE8 };
		DWORD r = cave - (LENS_CALLS[i] + 5); memcpy(call + 1, &r, 4);
		ok &= code(LENS_CALLS[i], call, 5);
	}
	DWORD capAt; memcpy(&capAt, cur + 2, 4);
	if (capAt == LENS_CAP_GAME) { DWORD mine = cave + 0x44; ok &= code(LENS_CAP_AT + 2, &mine, 4); }
	g_lensCave = cave;
	Log(ok ? "Cameras: in-car lens ready (up to 150 degrees).\n" : "Cameras: in-car lens - COULDN'T patch the game.\n");
	return true;
}
static void InCarLens(Game &g, float deg) {      // 0 = your own cockpit field of view
	static float last = -1;
	if (deg > 0 && fabsf(deg - g_cockpitFov) < 0.05f) deg = 0;   // the same as yours: nothing to do
	if (deg > 0 && !LensPatch(g)) return;
	if (!g_lensCave) return;
	float r = deg > 0 ? std::max(20.f, std::min(150.f, deg)) * DEG : 0;
	if (r == last) return;
	SIZE_T w = 0;
	if (WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)(g_lensCave + 0x40), &r, 4, &w)) {
		g_viewDirty = 3;
		if (deg > 0 && fabsf(r / DEG - (last > 0 ? last / DEG : 0)) >= 5) Log("Cameras: in-car lens %.0f deg (yours %.0f)\n", r / DEG, g_cockpitFov);
		last = r;
	}
	g_fovIn = r > 0;
}

// Rear (11) panning: riding on the car ahead of the TV car (subject, see RearKeep) it watches only the TV car. Otherwise
// the car it watches is kept while it stays within 60 m behind or alongside; another one takes over only when it is
// gone, or after the camera has stayed on it 6 s and the other is clearly closer.
static void RearPan(Game &g, int focus, int subject, std::vector<float> &v) {
	static int target = -1;
	static double since = 0;
	static float yaw = 180;
	if (focus < 0) { target = -1; return; }
	auto pos = [&](int i, float &x, float &y) { return g.Read(BLOCK0 + i * BLOCK_SIZE + OFF_POS, &x, 4) && g.Read(BLOCK0 + i * BLOCK_SIZE + OFF_POS + 4, &y, 4); };
	float fx, fy;
	if (!pos(focus, fx, fy)) return;
	float h = g_car[focus].head;
	auto rel = [&](int j, float &fwd, float &left) {
		float x, y;
		if (!pos(j, x, y)) return false;
		float dx = x - fx, dy = y - fy;
		fwd = dx * cosf(h) + dy * sinf(h); left = -dx * sinf(h) + dy * cosf(h);
		return true;
	};
	auto inRange = [&](int j) { if (j < 0 || j == focus || !g_car[j].valid || g_car[j].pit) return false; double a = Ahead(j, focus); return a < 4 && a > -60; };
	double now = Now();
	int best = -1; float bestD = 1e9f;
	if (subject >= 0 && subject != focus) { target = subject; since = now; }
	else for (int j : g_order) {
		if (!inRange(j) || Ahead(j, focus) < -45) continue;
		float fw, lf;
		if (!rel(j, fw, lf)) continue;
		float d = sqrtf(fw * fw + lf * lf);
		if (d < bestD) { bestD = d; best = j; }
	}
	float curD = 1e9f, fw, lf;
	bool keep = subject >= 0 && subject != focus ? true : inRange(target) && rel(target, fw, lf) && (curD = sqrtf(fw * fw + lf * lf)) < 60;
	if (!keep || (best >= 0 && best != target && now - since > 6 && bestD < curD - 12)) {
		if (best != target) { target = best; since = now; }
	}
	float want = v[3];                           // nobody near: straight back (the adjustment's own turn)
	if (target >= 0 && rel(target, fw, lf)) {
		float a = atan2f(lf, fw) / DEG;
		while (a < 0) a += 360;
		float span = (float)S.rearPanSpan;
		want = std::max(v[3] - span, std::min(v[3] + span, a));
	}
	// eased against time, not per loop (the loop rate varies): about rear_pan_smooth seconds to settle on the car
	static double lastT = 0;
	double dt = std::min(0.25, std::max(0.0, now - lastT));
	lastT = now;
	yaw += (want - yaw) * (float)(1 - exp(-dt * 2.3 / S.rearPanSmooth));
	v[3] = yaw; v[6] = (float)S.rearPanFov;
}

// The helicopter's follow rule (0x56A060..0x56A3C0, constants used by nothing else): within sqrt([0x6F5F3C]) = 25 m of the
// car it holds still; further out it closes at ([d^2] - 625) * [0x6F5F38] * 275 / d (so it settles ~87 m behind at race
// speed, recorded 2026-11-01), capped at [0x6F5F34] = 78.4 m/s; past 250 m it jumps back beside the car. RaceVision's
// helicopter cameras get drone_distance / drone_top_mph / drone_hover_m; anything else, the game's own.
static void DroneFollow(Game &g, bool ours) {
	static const DWORD TOP = 0x6F5F34, GAIN = 0x6F5F38, HOVER2 = 0x6F5F3C;
	static const float STOCK[3] = { 78.4f, 1.616161534911953e-05f, 625.0f };
	static DWORD okPid = 0; static bool ok = false; static float last[3] = { -1, -1, -1 };
	if (okPid != g.pid) {                        // the code reads these where we think: fcom [0x6F5F34] / fmul [0x6F5F38]
		unsigned char a[6], b[6]; okPid = g.pid; last[0] = -1;
		ok = g.Read(0x56A2A5, a, 6) && a[0] == 0xD8 && a[1] == 0x15 && *(DWORD *)(a + 2) == TOP &&
		     g.Read(0x56A113, b, 6) && b[0] == 0xD8 && b[1] == 0x0D && *(DWORD *)(b + 2) == GAIN;
	}
	if (!ok) return;
	float want[3] = { STOCK[0], STOCK[1], STOCK[2] };
	if (ours) {
		want[0] = (float)(S.droneTopMph / 2.23694);
		want[1] = (float)(STOCK[1] * 100.0 / std::max(20.0, S.droneDistance));
		want[2] = (float)(S.droneHoverM * S.droneHoverM);
	}
	if (!memcmp(want, last, sizeof want)) return;
	DWORD prot = 0; SIZE_T w = 0;
	if (!VirtualProtectEx(g.h, (LPVOID)(UINT_PTR)TOP, 12, PAGE_READWRITE, &prot)) return;
	bool done = WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)TOP, want, sizeof want, &w) != 0;
	VirtualProtectEx(g.h, (LPVOID)(UINT_PTR)TOP, 12, prot, &prot);
	if (done) memcpy(last, want, sizeof want);
}

static void CamWork(Game &g) {
	static std::string session;
	std::string ses = g_trackName + "|" + std::to_string(g_raceNo) + "|" + std::to_string(g_session);
	if (ses != session) { session = ses; g_stockRaw.clear(); g_written.clear(); }   // a session loads its own values
	static bool loaded = false;                  // the editor's cameras, once
	if (!loaded) { loaded = true; LoadCustomCams(); }
	{ float f = g.Get<float>(COCKPIT_FOV, 0); if (f > 10 && f < 180) g_cockpitFov = f; }   // yours, as it is now (read only)
	bool rp = g.Get<int>(RP_MODE, 0) == 1;
	DWORD obj = rp ? REPLAY_CAMOBJ : CAMOBJ;
	int gameCam = g.Get<int>(obj + 4, -1);
	if (g_virtCam && GameCam(g_virtCam) != gameCam) g_virtCam = 0;   // the camera was changed some other way
	g.HeliSound(!(g_virtCam == CAM_DRONE && g_droneQuiet));
	g.HeliNear(g_virtCam && gameCam == 16);          // our low helicopter cameras: near clip 2 m
	DroneFollow(g, g_virtCam && gameCam == 16);      // ... and their own follow distance / speed
	CamNow n;
	n.gameCam = gameCam; n.id = g_virtCam ? g_virtCam : gameCam; n.kind = KindOf(gameCam);
	DWORD fr = g.Get<DWORD>(obj + 0xC, 0), base0 = rp ? REPLAY_REC0 : REC0;
	if (fr >= base0 && (fr - base0) % REC_SIZE == 0 && (fr - base0) / REC_SIZE < NUM_SLOTS) n.focus = (int)((fr - base0) / REC_SIZE);
	n.driving = g.Get<int>(DRIVER_STATE, -1) == 3;
	if (n.kind == ckInCar && n.driving) n.kind = ckNone;              // never your own driving view
	if (n.kind != ckNone) n.block = BlockOf(g, gameCam, n.focus, n.nRaw);
	if (!n.block) n.kind = ckNone;
	g_camNow = n;
	// what goes into the block now
	std::vector<float> want;
	bool inCar = false;
	if (n.kind != ckNone && S.customCams) {
		std::vector<float> stockRaw = StockOf(g, n.block, n.nRaw);
		if (!stockRaw.empty()) {
			std::vector<float> stock = FromRaw(n.kind, stockRaw.data());
			auto lt = g_liveTune.find(n.id);
			std::vector<float> v = lt != g_liveTune.end() ? lt->second : SavedTune(n.id, n.kind, stock);
			if (gameCam == 11 && (S.rearPan || g_mount >= 0) && !rp) {
				if (v.empty()) v = stock;
				RearPan(g, n.focus, g_mount >= 0 && g_mount == n.focus ? g_shot.car : -1, v);
			}
			if (!v.empty()) { want = stockRaw; ToRaw(n.kind, v, want.data()); }
			inCar = n.kind == ckInCar && v.size() >= 7;
			if (inCar) { ViewAngles(g, v[3], v[4], v[5]); InCarLens(g, v[6]); }
		}
	}
	if (!inCar) { if (g_viewCave) ViewAngles(g, 0, 0, 0); InCarLens(g, 0); }
	// the game's own values back in every block that isn't this one's
	for (auto it = g_written.begin(); it != g_written.end();) {
		if (!want.empty() && it->first == n.block) { ++it; continue; }
		auto st = g_stockRaw.find(it->first);
		if (st != g_stockRaw.end()) { SIZE_T w = 0; WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)it->first, st->second.data(), st->second.size() * 4, &w); }
		it = g_written.erase(it);
	}
	if (!want.empty()) {
		std::vector<float> cur(n.nRaw);
		if (!DynRigOwns(n.block) && g.Read(n.block, cur.data(), n.nRaw * 4) && memcmp(cur.data(), want.data(), n.nRaw * 4)) {
			DWORD prot = 0; SIZE_T w = 0;
			if (n.block == EYE_OFFSET) VirtualProtectEx(g.h, (LPVOID)(UINT_PTR)n.block, n.nRaw * 4, PAGE_READWRITE, &prot);
			WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)n.block, want.data(), n.nRaw * 4, &w);
			g_viewDirty = 3;
		}
		g_written[n.block] = n.id;
	}
	if (g_viewDirty > 0) { g_viewDirty--; ViewCacheClear(g); }   // rebuilt with our values (a few loops: a frame in flight)
}

// ---- the live editor (the phone): it works on the camera on screen. All of these run under g_lock.
static std::vector<float> &EditTune() {          // the adjustment being edited, made from the saved one or the game's
	static std::vector<float> none;
	const CamNow &n = g_camNow;
	if (n.kind == ckNone) return none;
	auto it = g_liveTune.find(n.id);
	if (it != g_liveTune.end()) return it->second;
	if (!g_game) return none;
	std::vector<float> stockRaw = StockOf(*g_game, n.block, n.nRaw);
	if (stockRaw.empty()) return none;
	std::vector<float> stock = FromRaw(n.kind, stockRaw.data());
	std::vector<float> v = SavedTune(n.id, n.kind, stock);
	if (v.empty()) v = stock;
	return g_liveTune[n.id] = v;
}
static bool g_droneLive = false;                 // drone follow settings changed on the phone, not saved yet
// the phone's drone stick / sliders: height change (m), trail distance change (%), top speed / hold-still (absolute, < 0 = keep)
static void CamEditDrone(double up, double dDist, double top, double hover) {
	if (g_camNow.kind != ckHeli || !VCamById(g_camNow.id)) return;
	if (up != 0) { std::vector<float> &v = EditTune(); if (!v.empty()) v[0] = std::max(3.f, std::min(400.f, v[0] + (float)up)); }
	if (dDist != 0) S.droneDistance = std::max(20.0, std::min(400.0, S.droneDistance + dDist));
	if (top >= 0) S.droneTopMph = std::max(60.0, std::min(400.0, top));
	if (hover >= 0) S.droneHoverM = std::max(0.0, std::min(150.0, hover));
	if (dDist != 0 || top >= 0 || hover >= 0) g_droneLive = true;
}
static std::string CamEditJson() {
	const CamNow &n = g_camNow;
	std::string o = "{\"cam\":" + std::to_string(n.id) + ",\"game\":" + std::to_string(n.gameCam) + ",\"kind\":\"" + CamKindName(n.kind) + "\"";
	o += ",\"custom\":" + std::string(n.id > 28 && VCamById(n.id) ? "true" : "false");
	o += ",\"own\":" + std::string(VCamById(n.id) ? "true" : "false");
	o += ",\"live\":" + std::string(g_liveTune.count(n.id) || (n.kind == ckHeli && g_droneLive) ? "true" : "false");
	if (n.kind == ckHeli && VCamById(n.id)) {          // one of our helicopter cameras: its follow settings (live on the phone)
		char b[160];
		snprintf(b, sizeof b, ",\"drone\":{\"dist\":%.0f,\"top\":%.0f,\"hover\":%.0f}", S.droneDistance, S.droneTopMph, S.droneHoverM);
		o += b;
	}
	o += ",\"why\":\"" + std::string(n.kind != ckNone ? "" : n.driving && KindOf(n.gameCam) == ckInCar ? "Not your own view while you drive." :
	     !S.customCams ? "Own angles are off (Cameras page)." : "This camera can't be adjusted yet (TV cameras come next).") + "\"";
	o += ",\"params\":[";
	if (n.kind != ckNone) {
		std::vector<float> &v = EditTune();
		int np; const CamParam *p = ParamsOf(n.kind, np);
		for (int i = 0; i < np && i < (int)v.size(); i++) {
			char b[160];
			snprintf(b, sizeof b, "%s{\"n\":\"%s\",\"u\":\"%s\",\"v\":%.3f,\"step\":%.3f}", i ? "," : "", p[i].name, p[i].unit, v[i], p[i].step);
			o += b;
		}
	}
	return o + "]}";
}
// a nudge of parameter i by k steps
static void CamEditNudge(int i, double k) {
	std::vector<float> &v = EditTune();
	int np; const CamParam *p = ParamsOf(g_camNow.kind, np);
	if (i < 0 || i >= np || i >= (int)v.size()) return;
	v[i] += (float)(k * p[i].step);
	if (g_camNow.kind == ckHeli) v[0] = std::max(3.f, v[0]);
	if (i == 6) v[i] = std::max(g_camNow.kind == ckInCar ? 20.f : 10.f, std::min(g_camNow.kind == ckInCar ? 150.f : 170.f, v[i]));   // lens
	if (i == 6) { static double t = 0; if (Now() - t > 2) { t = Now(); Log("           lens now %.0f deg (camera %d)\n", v[i], g_camNow.id); } }
}
// a move in the direction the camera looks: f ahead, s to the left, u up (in steps)
static void CamEditMove(double f, double s, double u) {
	std::vector<float> &v = EditTune();
	CamKind k = g_camNow.kind;
	if (v.empty() || k == ckHeli) { if (k == ckHeli && !v.empty()) v[0] = std::max(3.f, v[0] + (float)(u * P_HELI[0].step)); return; }
	int np; const CamParam *p = ParamsOf(k, np);
	float step = p[0].step, t = v[3] * DEG;
	float dx = (float)(f * cos(t) - s * sin(t)) * step, dy = (float)(f * sin(t) + s * cos(t)) * step;
	if (k == ckChase) { v[0] -= dx; v[1] += dy; v[2] += (float)u * p[2].step; }
	else { v[0] += dx; v[1] += dy; v[2] += (float)u * p[2].step; }
}
static void CamEditLook(double dTurn, double dTilt) {   // the drag pad: degrees
	std::vector<float> &v = EditTune();
	if (v.size() > 3) v[3] += (float)dTurn;
	if (v.size() > 4) v[4] = std::max(-89.f, std::min(89.f, v[4] + (float)dTilt));
}
static std::string CamEditValue() {              // the adjustment as racevision.ini keeps it
	std::vector<float> &v = EditTune();
	std::string s;
	for (size_t i = 0; i < v.size(); i++) { char b[32]; snprintf(b, sizeof b, "%s%.2f", i ? ", " : "", fabs(v[i]) < 0.005f ? 0.f : v[i]); s += b; }
	return s;
}

// RaceVision closing: every camera back to the game's own, and the in-car turn to none
static void DynRestore(Game &g);                  // racevision_dyncams.cpp
static void CamRestore(Game &g) {
	if (!g.h) return;
	DynRestore(g);
	for (auto &w : g_written) {
		auto st = g_stockRaw.find(w.first);
		if (st != g_stockRaw.end()) { SIZE_T n = 0; WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)w.first, st->second.data(), st->second.size() * 4, &n); }
	}
	g_written.clear();
	if (g_viewCave) ViewAngles(g, 0, 0, 0);
	InCarLens(g, 0);
	g.HeliSound(true);
	g.HeliNear(false);
	DroneFollow(g, false);
}

// NR2003 closed: its patch and values went with it
static void CamGone() { if (g_game) { g_game->heliKind = -1; g_game->heliNear = -1; } g_viewCave = 0; g_lensCave = 0; g_fovIn = false; g_viewLast[0] = 1e9f; g_mount = -1; g_written.clear(); g_stockRaw.clear(); }
