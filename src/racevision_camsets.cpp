// ---------------------------------------------------------------- camera sets (2026-11-03)
// RaceVision's native version of camsets\camsets.py, and the director view's "Camera sets" list.
//
// NR2003 takes its cameras from two kinds of file, read when a session loads:
//   tracks\<track>\<track>.cam   MACT v2: TV1, TV2, spectator cameras (57-byte records) + the two pit lane cameras + a tail
//   series\<series>\make_a.cam   MACC: the six car-mounted cameras (9 floats each)
// A "set" is one such file. Presets live as text in camsets\presets\<kind>\<who>\<name>.txt (made by camsets.py, the same
// files the double-click menu uses); the recipes of camsets.py are ported here too, so a track or series with no preset
// files still offers its standard sets (built from the game's own file kept in camsets\stock, else the file in the game).
//
// LIVE SWITCHING (found by reading NR2003.exe 2026-11-03, signature checked before anything is written):
//   at session load 0x568560.. parses the .cam into the "master" tables and copies them to the "working" tables:
//     TV1/TV2/spectator  0xC548A0 -> 0xC585A0   (3 groups x 3652 bytes: a count, then 64 x 57-byte records) [0x5685A5]
//     pit lane cameras   0xC573F8 -> 0xC57378   (2 x 9 floats)                                              [0x5685C1]
//     car cameras        0xC5B070 -> 0xC57450   (20 models x 6 cameras x 36 bytes)                          [0x568590]
//   and the per-frame camera code reads the working tables (0x569876 lea eax,[edx*4+0xC585A0] picks the TV camera, the
//   view routines 0x56A6B8 / 0x56B1C2 read its position and lens). So a new set written into the working tables (and the
//   master, which RaceVision's TvLoad reads) is in use on the next frame: no session reload.
//   Records are written first and the counts after, so the game never sees a count larger than what is in place.
// The pit camera blocks are also what the dynamic cameras write (racevision_dyncams.cpp), so a pit change here forgets the
// "stock" RaceVision remembered for them.
static std::string J(const std::string &ansi);       // racevision_remote.cpp

struct CsTv { float f[14] = {}; unsigned char type = 0; };
struct CsSet {
	bool ok = false, track = true;
	std::vector<CsTv> grp[3];
	float pit[2][9] = {};
	std::string tail, extra;                 // raw bytes after the pit cameras / after the body
	int ver = 2;
	std::vector<std::array<float, 9>> cams;   // car cameras
};
static const char *CS_TV_FIELDS[14] = { "sf", "cl", "height", "takeover", "u4", "zoom_in", "zoom", "zoom_out", "aim", "tilt", "lean", "view", "u12", "u13" };
static const char *CS_CAM_FIELDS[9] = { "x", "y", "z", "yaw", "pitch", "roll", "fov", "u7", "u8" };   // pit lane (world) and car cameras
static const char *CS_GROUPS[3] = { "TV1", "TV2", "SPECTATOR" };
static const DWORD CS_TV_WORK = 0xC585A0, CS_TV_MASTER = 0xC548A0, CS_PIT_MASTER = 0xC573F8;
static const int CS_GROUP_BYTES = 4 + 64 * 57, CS_MODELS = 20;

// ---- binary <-> CsSet
static bool CsReadBin(const std::string &b, CsSet &s) {
	s = CsSet();
	if (b.size() < 12) return false;
	unsigned ver, size; memcpy(&ver, b.data() + 4, 4); memcpy(&size, b.data() + 8, 4);
	if (!memcmp(b.data(), "MACT", 4)) {
		if (ver != 2) return false;
		size_t o = 12;
		for (int k = 0; k < 3; k++) {
			if (o + 4 > b.size()) return false;
			unsigned n; memcpy(&n, b.data() + o, 4); o += 4;
			if (n > 64 || o + n * 57 > b.size()) return false;
			for (unsigned i = 0; i < n; i++) { CsTv c; memcpy(c.f, b.data() + o, 56); c.type = (unsigned char)b[o + 56]; s.grp[k].push_back(c); o += 57; }
		}
		if (o + 72 + 16 > b.size()) return false;
		memcpy(s.pit, b.data() + o, 72); o += 72;
		s.tail = b.substr(o, 16); o += 16;
		s.extra = b.substr(o);
		s.track = true; s.ver = 2; s.ok = true; return true;
	}
	if (!memcmp(b.data(), "MACC", 4)) {
		size_t n = size / 36;
		if (12 + n * 36 > b.size()) return false;
		for (size_t k = 0; k < n; k++) { std::array<float, 9> c; memcpy(c.data(), b.data() + 12 + 36 * k, 36); s.cams.push_back(c); }
		s.extra = b.substr(12 + n * 36);
		s.track = false; s.ver = (int)ver; s.ok = true; return true;
	}
	return false;
}
static std::string CsWriteBin(const CsSet &s) {
	std::string body;
	if (s.track) {
		for (int k = 0; k < 3; k++) {
			unsigned n = (unsigned)s.grp[k].size(); body.append((const char *)&n, 4);
			for (auto &c : s.grp[k]) { body.append((const char *)c.f, 56); body.push_back((char)c.type); }
		}
		body.append((const char *)s.pit, 72);
		body += s.tail;
		unsigned two = 2, len = (unsigned)body.size();
		return std::string("MACT") + std::string((const char *)&two, 4) + std::string((const char *)&len, 4) + body + s.extra;
	}
	for (auto &c : s.cams) body.append((const char *)c.data(), 36);
	unsigned ver = (unsigned)s.ver, len = (unsigned)body.size();
	return std::string("MACC") + std::string((const char *)&ver, 4) + std::string((const char *)&len, 4) + body + s.extra;
}

// ---- text <-> CsSet (camsets.py's format: one camera per line, named fields)
static std::string CsHex(const std::string &b) { std::string o; char t[4]; for (unsigned char c : b) { snprintf(t, sizeof t, "%02x", c); o += t; } return o; }
static std::string CsUnhex(const std::string &h) { std::string o; for (size_t i = 0; i + 1 < h.size(); i += 2) o.push_back((char)strtol(h.substr(i, 2).c_str(), nullptr, 16)); return o; }
static std::string CsF(float v) { char b[40]; snprintf(b, sizeof b, "%.9g", v); return b; }
static bool CsFromText(const std::string &t, CsSet &s, std::string &err) {
	s = CsSet();
	std::string group, line;
	bool haveKind = false;
	int pitN = 0;
	size_t pos = 0;
	while (pos <= t.size()) {
		size_t e = t.find('\n', pos); if (e == std::string::npos) e = t.size();
		line = t.substr(pos, e - pos); pos = e + 1;
		size_t h = line.find('#'); if (h != std::string::npos) line.resize(h);
		line = Trim(line);
		if (line.empty()) continue;
		if (line[0] == '[') { group = Trim(line.substr(1, line.find(']') - 1)); continue; }
		if (line.rfind("cam ", 0) == 0) {
			std::map<std::string, std::string> kv;
			std::string rest = line.substr(4);
			size_t a = 0;
			while (a < rest.size()) {
				while (a < rest.size() && rest[a] == ' ') a++;
				size_t b = rest.find(' ', a); if (b == std::string::npos) b = rest.size();
				std::string tok = rest.substr(a, b - a); a = b;
				size_t q = tok.find('=');
				if (q != std::string::npos) kv[tok.substr(0, q)] = tok.substr(q + 1);
			}
			auto get = [&](const char *k, float &out) { auto it = kv.find(k); if (it == kv.end()) return false; out = (float)strtod(it->second.c_str(), nullptr); return true; };
			if (group == "CAR") {
				std::array<float, 9> c;
				for (int i = 0; i < 9; i++) if (!get(CS_CAM_FIELDS[i], c[i])) { err = std::string("a car camera lacks ") + CS_CAM_FIELDS[i]; return false; }
				s.cams.push_back(c);
			} else if (group == "PIT") {
				if (pitN >= 2) { err = "more than 2 pit cameras"; return false; }
				for (int i = 0; i < 9; i++) if (!get(CS_CAM_FIELDS[i], s.pit[pitN][i])) { err = std::string("a pit camera lacks ") + CS_CAM_FIELDS[i]; return false; }
				pitN++;
			} else {
				int gi = -1; for (int i = 0; i < 3; i++) if (group == CS_GROUPS[i]) gi = i;
				if (gi < 0) { err = "a camera outside [TV1] / [TV2] / [SPECTATOR]"; return false; }
				CsTv c;
				for (int i = 0; i < 14; i++) if (!get(CS_TV_FIELDS[i], c.f[i])) { err = std::string("a TV camera lacks ") + CS_TV_FIELDS[i]; return false; }
				float ty = 0; if (!get("type", ty)) { err = "a TV camera lacks type"; return false; }
				c.type = (unsigned char)(int)ty;
				s.grp[gi].push_back(c);
			}
			continue;
		}
		size_t q = line.find('=');
		if (q == std::string::npos) continue;
		std::string k = Trim(line.substr(0, q)), v = Trim(line.substr(q + 1));
		if (k == "kind") { s.track = v == "track"; haveKind = true; }
		else if (k == "version") s.ver = atoi(v.c_str());
		else if (k == "tail") s.tail = CsUnhex(v);
		else if (k == "extra") s.extra = CsUnhex(v);
	}
	if (!haveKind) { err = "no kind line"; return false; }
	if (s.track && (s.tail.size() != 16 || pitN != 2)) { err = s.tail.size() != 16 ? "the tail is not 16 bytes" : "a track set needs exactly 2 pit cameras"; return false; }
	if (!s.track && s.cams.size() < 6) { err = "a car set needs 6 cameras"; return false; }
	s.ok = true;
	return true;
}

// ---- recipes (camsets.py): new sets made from the game's own file
static const double CS_PI = 3.14159265358979;
static void CsScaleLens(CsSet &d, double k) {
	for (int g = 0; g < 2; g++)
		for (auto &c : d.grp[g]) {
			for (int i : { 5, 6, 7 }) c.f[i] = (float)(c.f[i] * k);
			if (c.type == 1) c.f[11] = (float)(c.f[11] * k);
		}
}
static double CsMod(double a, double b) { double r = fmod(a, b); return r < 0 ? r + b : r; }
static void CsMoreCuts(CsSet &d) {
	double L = 0; bool any = false;
	for (int g = 0; g < 3; g++) for (auto &c : d.grp[g]) { L = std::max(L, (double)c.f[0]); any = true; }
	L = any ? L * 1.02 : 1000.0;
	std::vector<CsTv> pool = d.grp[0];
	for (auto &c : d.grp[1]) {
		if (c.type != 0) continue;
		bool isFar = true;
		for (auto &o : pool) if (fabs(CsMod((double)c.f[0] - o.f[0] + L / 2, L) - L / 2) <= 60) { isFar = false; break; }
		if (isFar) pool.push_back(c);
	}
	std::stable_sort(pool.begin(), pool.end(), [](const CsTv &a, const CsTv &b) { return a.f[0] < b.f[0]; });
	for (size_t n = 0; n < pool.size(); n++) {
		const CsTv &prev = pool[(n + pool.size() - 1) % pool.size()];
		double gap = CsMod((double)pool[n].f[0] - prev.f[0], L); if (gap == 0) gap = L;
		pool[n].f[3] = (float)std::max((double)pool[n].f[3], -0.65 * gap);
	}
	if (pool.size() > 64) pool.resize(64);
	d.grp[0] = pool;
}
static std::array<float, 9> CsLookAt(double cx, double cy, double cz, double fov, float u7, float u8, double tz = 0.6) {
	double yaw = atan2(-cy, -cx), pitch = atan2(cz - tz, hypot(cx, cy));
	return { (float)cx, (float)cy, (float)cz, (float)yaw, (float)pitch, 0.f, (float)fov, u7, u8 };
}
struct CsCarBits { std::array<float, 9> roof, right; double half; float u7, u8; };
static CsCarBits CsCar(const CsSet &d) {
	CsCarBits b; b.roof = d.cams[3]; b.right = d.cams[4];
	double half = std::max(fabs((double)d.cams[4][1]), fabs((double)d.cams[5][1])) + 0.26;
	b.half = std::min(std::max(half, 0.9), 1.05); b.u7 = d.cams[4][7]; b.u8 = d.cams[4][8];
	return b;
}
static void CsCarTvOnboard(CsSet &d) {
	CsCarBits b = CsCar(d);
	d.cams[4] = { -0.9f, (float)-(b.half + 0.12), (float)((double)b.roof[2] - 0.15), 0.20f, 0.08f, 0.f, 1.2f, b.u7, b.u8 };
	d.cams[5] = { (float)((double)b.roof[0] + 0.35), 0.f, (float)((double)b.roof[2] + 0.10), (float)CS_PI, 0.18f, 0.f, 1.25f, b.u7, b.u8 };
}
struct CsRecipe { const char *name; const char *track; const char *desc; void (*run)(CsSet &); };
static const CsRecipe CS_RECIPES[] = {
	{ "tv tight", "1", "TV1 / TV2: a lens about a third tighter (x 0.7) - the car fills more of the picture.", [](CsSet &d) { CsScaleLens(d, 0.7); } },
	{ "tv wide", "1", "TV1 / TV2: a lens about a third wider (x 1.4) - more of the pack in shot.", [](CsSet &d) { CsScaleLens(d, 1.4); } },
	{ "tv more cuts", "1", "TV1 also gets TV2's following cameras: about twice the cameras, so the picture cuts like a TV broadcast does.", [](CsSet &d) { CsMoreCuts(d); } },
	{ "tv more cuts tight", "1", "More cuts, and the tighter lens.", [](CsSet &d) { CsMoreCuts(d); CsScaleLens(d, 0.7); } },
	{ "tv onboard", "", "Onboards that show the car: right side = door cam high on the right of the car, turned in so the door, hood and right front tyre are in the picture; left side = roof cam on the front of the roof looking back down the roof and rear deck at the cars behind.", [](CsSet &d) { CsCarTvOnboard(d); } },
	{ "tv onboard + nose", "", "TV onboards, and the front bumper camera turned round: on the nose looking back up the hood at the windshield and the driver.", [](CsSet &d) {
		CsCarTvOnboard(d);
		std::array<float, 9> front = d.cams[0], roof = d.cams[3];
		d.cams[0] = { (float)((double)front[0] - 0.1), 0.f, (float)std::max((double)front[2] + 0.55, (double)roof[2] * 0.65), (float)CS_PI, -0.06f, 0.f, 1.25f, front[7], front[8] }; } },
	{ "external", "", "Outside the car, fixed to it (it moves with the car, like a TV car-to-car shot): right side = low front quarter on the right, looking back at the car; left side = rear quarter on the left, higher, looking forward at it.", [](CsSet &d) {
		CsCarBits b = CsCar(d); d.cams[4] = CsLookAt(3.2, -2.4, 0.55, 1.0, b.u7, b.u8); d.cams[5] = CsLookAt(-4.0, 2.2, 1.3, 0.95, b.u7, b.u8); } },
	{ "external 2", "", "Outside the car, fixed to it: right side = broadside from 5.5 m off the right, left side = head-on from 6 m ahead, low (the car coming at the lens).", [](CsSet &d) {
		CsCarBits b = CsCar(d); d.cams[4] = CsLookAt(0.0, -5.5, 1.0, 0.9, b.u7, b.u8); d.cams[5] = CsLookAt(6.0, 0.0, 0.9, 0.75, b.u7, b.u8); } },
	{ "mix", "", "One of each: right side = TV door cam, left side = low front quarter outside the car (looking back at it).", [](CsSet &d) {
		CsCarTvOnboard(d); CsCarBits b = CsCar(d); d.cams[5] = CsLookAt(3.2, 2.4, 0.55, 1.0, b.u7, b.u8); } },
};

// ---- where things are
static bool CsDir(const std::string &p) { DWORD a = GetFileAttributesA(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static std::string CsExeDir() { char p[MAX_PATH]; GetModuleFileNameA(NULL, p, MAX_PATH); std::string s = p; size_t k = s.find_last_of("\\/"); return k == std::string::npos ? "." : s.substr(0, k); }
static std::string CsRoot() {                         // camsets\ (presets + stock): beside the app folder, or in it
	static std::string cached;
	if (!cached.empty() && CsDir(cached)) return cached;
	std::string want = Raw("camsets_dir");
	{ char env[MAX_PATH]; if (GetEnvironmentVariableA("RV_CAMSETS", env, MAX_PATH)) want = env; }   // (tests)
	std::vector<std::string> c;
	if (!want.empty()) c.push_back(want);
	std::string e = CsExeDir();
	c.push_back(e + "\\camsets"); c.push_back(e + "\\..\\camsets");
	for (auto &d : c) if (CsDir(d + "\\presets") || CsDir(d)) { char full[MAX_PATH]; cached = GetFullPathNameA(d.c_str(), MAX_PATH, full, NULL) ? full : d; return cached; }
	return "";
}
static std::string CsGame() {
	if (!g_gameDir.empty()) return g_gameDir;
	std::string d = Raw("game_dir");
	if (!d.empty() && CsDir(d)) return d;
	d = "C:\\Papyrus\\NASCAR Racing 2003 Season";
	return CsDir(d + "\\tracks") ? d : "";
}
static bool CsSafeName(const std::string &n) { return !n.empty() && n.size() < 80 && n.find_first_of("\\/:*?\"<>|") == std::string::npos && n.find("..") == std::string::npos && n[0] != '.'; }
static std::string CsStockDir(const std::string &kind, const std::string &who) { return CsRoot() + "\\stock\\" + kind + "\\" + who; }
static std::string CsPresetPath(const std::string &kind, const std::string &who, const std::string &name) { return CsRoot() + "\\presets\\" + kind + "\\" + who + "\\" + name + ".txt"; }
// the loose camera file the game reads (it may not exist: some tracks keep theirs inside the .dat)
static std::string CsGameFile(const std::string &kind, const std::string &who) {
	std::string game = CsGame();
	if (game.empty()) return "";
	if (kind != "track") return game + "\\series\\" + who + "\\make_a.cam";
	std::string folder = game + "\\tracks\\" + who, first;
	WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((folder + "\\*.cam").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (_stricmp(fd.cFileName, (who + ".cam").c_str()) == 0) { FindClose(h); return folder + "\\" + fd.cFileName; }
			if (first.empty()) first = fd.cFileName;
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	return folder + "\\" + (first.empty() ? who + ".cam" : first);
}
static bool CsWriteFile(const std::string &path, const std::string &bytes) {
	std::string tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f) return false;
	bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
	ok = fclose(f) == 0 && ok;
	if (!ok) { DeleteFileA(tmp.c_str()); return false; }
	return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
static std::vector<std::string> CsSubdirs(const std::string &dir) {
	std::vector<std::string> v;
	WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return v;
	do if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') v.push_back(fd.cFileName); while (FindNextFileA(h, &fd));
	FindClose(h);
	std::sort(v.begin(), v.end(), [](const std::string &a, const std::string &b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
	return v;
}
static std::vector<std::string> CsFiles(const std::string &dir, const char *ext) {
	std::vector<std::string> v;
	WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((dir + "\\*" + ext).c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return v;
	do if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) v.push_back(fd.cFileName); while (FindNextFileA(h, &fd));
	FindClose(h);
	return v;
}

// ---- the live tables
static bool CsLiveSig(Game &g) {                     // the copy code is where it was found: only then write the tables
	static DWORD pid = 0; static bool ok = false;
	if (!g.h) return false;
	if (pid != g.pid) {
		static const unsigned char w1[15] = { 0xB9, 0xB3, 0x0A, 0, 0, 0xBE, 0xA0, 0x48, 0xC5, 0, 0xBF, 0xA0, 0x85, 0xC5, 0 };
		static const unsigned char w2[15] = { 0xB9, 0x12, 0, 0, 0, 0xBE, 0xF8, 0x73, 0xC5, 0, 0xBF, 0x78, 0x73, 0xC5, 0 };
		unsigned char a[15], b[15];
		ok = g.Read(0x5685A5, a, 15) && !memcmp(a, w1, 15) && g.Read(0x5685C1, b, 15) && !memcmp(b, w2, 15);
		pid = g.pid;
		if (!ok) Log("Camera sets: this NR2003.exe's camera tables aren't where RaceVision knows them - live switching off.\n");
	}
	return ok;
}
static void CsImage(const CsSet &s, std::vector<unsigned char> &img) {
	img.assign(3 * CS_GROUP_BYTES, 0);
	for (int k = 0; k < 3; k++) {
		unsigned char *p = img.data() + k * CS_GROUP_BYTES;
		unsigned n = (unsigned)std::min<size_t>(64, s.grp[k].size());
		memcpy(p, &n, 4);
		for (unsigned i = 0; i < n; i++) { memcpy(p + 4 + i * 57, s.grp[k][i].f, 56); p[4 + i * 57 + 56] = s.grp[k][i].type; }
	}
}
static bool CsWriteTv(Game &g, DWORD base, const std::vector<unsigned char> &img) {
	SIZE_T w = 0; bool ok = true;
	for (int k = 0; k < 3; k++) ok &= WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)(base + k * CS_GROUP_BYTES + 4), img.data() + k * CS_GROUP_BYTES + 4, CS_GROUP_BYTES - 4, &w) && w == CS_GROUP_BYTES - 4;
	for (int k = 0; k < 3; k++) ok &= WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)(base + k * CS_GROUP_BYTES), img.data() + k * CS_GROUP_BYTES, 4, &w) && w == 4;
	return ok;
}
static bool CsReadTv(Game &g, DWORD base, std::vector<unsigned char> &img) {
	img.assign(3 * CS_GROUP_BYTES, 0);
	return g.Read(base, img.data(), img.size());
}
// two images agree where they matter: the counts and the records in use
static bool CsSameTv(const std::vector<unsigned char> &a, const std::vector<unsigned char> &b) {
	if (a.size() != b.size() || a.size() != 3 * (size_t)CS_GROUP_BYTES) return false;
	for (int k = 0; k < 3; k++) {
		const unsigned char *p = a.data() + k * CS_GROUP_BYTES, *q = b.data() + k * CS_GROUP_BYTES;
		unsigned n, m; memcpy(&n, p, 4); memcpy(&m, q, 4);
		if (n != m || n > 64 || memcmp(p, q, 4 + n * 57)) return false;
	}
	return true;
}
// what the session loaded (taken once before our first live write; a new session loads its own tables)
static std::vector<unsigned char> g_csSnapTv, g_csSnapPit, g_csSnapCar;
static std::string g_csSnapKey, g_csLiveTrackSet, g_csLiveCarSet;
static std::string CsSessionKey() { return g_trackName + "|" + std::to_string(g_raceNo) + "|" + std::to_string(g_session); }
static void CsSnapshot(Game &g) {
	std::string key = CsSessionKey();
	if (key == g_csSnapKey && !g_csSnapTv.empty()) return;
	g_csSnapKey = key; g_csLiveTrackSet.clear(); g_csLiveCarSet.clear();
	CsReadTv(g, CS_TV_WORK, g_csSnapTv);
	g_csSnapPit.assign(72, 0); g.Read(PIT1_BLOCK, g_csSnapPit.data(), 72);
	g_csSnapCar.assign(CS_MODELS * 6 * 36, 0); g.Read(CAR_CAMS, g_csSnapCar.data(), g_csSnapCar.size());
}
static void CsAfterLive(Game &g) {                    // the pit blocks and views changed under RaceVision's feet
	g_stockRaw.erase(PIT1_BLOCK); g_stockRaw.erase(PIT2_BLOCK); g_written.erase(PIT1_BLOCK); g_written.erase(PIT2_BLOCK);
	g_viewDirty = 3; ViewCacheClear(g);
	TvLoad(g);
}
static bool CsApplyTrackLive(Game &g, const CsSet &s, std::string &msg) {
	if (!g.h) { msg = "NR2003 isn't running"; return false; }
	if (g.Get<int>(RP_MODE, 0) == 1) { msg = "a replay is playing - try again when it's over"; return false; }
	if (!CsLiveSig(g)) { msg = "this NR2003.exe isn't the one RaceVision knows (live switching off)"; return false; }
	unsigned n0 = g.Get<unsigned>(CS_TV_WORK, 99999);
	if (n0 > 64) { msg = "no session is loaded in NR2003 yet"; return false; }
	CsSnapshot(g);
	std::vector<unsigned char> img, back;
	CsImage(s, img);
	bool ok = CsWriteTv(g, CS_TV_WORK, img) && CsWriteTv(g, CS_TV_MASTER, img);
	SIZE_T w = 0;
	ok &= WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)PIT1_BLOCK, s.pit, 72, &w) && w == 72;
	ok &= WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)CS_PIT_MASTER, s.pit, 72, &w) && w == 72;
	if (!ok) { msg = "the game wouldn't take the camera tables"; return false; }
	if (!CsReadTv(g, CS_TV_WORK, back) || !CsSameTv(img, back)) { msg = "the tables didn't read back the same"; return false; }
	CsAfterLive(g);
	return true;
}
// the car cameras: the models whose six cameras are the ones a known set has (the game's, the file now, the last live set)
static std::vector<std::string> CsCarBlocks(const CsSet &s) {
	std::vector<std::string> v;
	for (int i = 0; i < 6 && i < (int)s.cams.size(); i++) v.push_back(std::string((const char *)s.cams[i].data(), 36));
	return v;
}
static int CsMatchCar(Game &g, const std::vector<std::string> &want, std::vector<int> &models) {
	std::string all(CS_MODELS * 6 * 36, '\0');
	if (!g.Read(CAR_CAMS, &all[0], all.size()) || want.size() < 6) return 0;
	models.clear();
	for (int m = 0; m < CS_MODELS; m++) {
		bool same = true;
		for (int i = 0; i < 6 && same; i++) same = all.compare((m * 6 + i) * 36, 36, want[i]) == 0;
		if (same) models.push_back(m);
	}
	return (int)models.size();
}
static bool CsApplyCarLive(Game &g, const CsSet &s, const std::vector<CsSet> &known, std::string &msg) {
	if (!g.h) { msg = "NR2003 isn't running"; return false; }
	if (!CsLiveSig(g)) { msg = "this NR2003.exe isn't the one RaceVision knows (live switching off)"; return false; }
	if (g.Get<unsigned>(CS_TV_WORK, 99999) > 64) { msg = "no session is loaded in NR2003 yet"; return false; }
	CsSnapshot(g);
	std::vector<int> models;
	for (auto &k : known) if (CsMatchCar(g, CsCarBlocks(k), models)) break;
	if (models.empty()) { msg = "couldn't find this series' car cameras in the game's memory"; return false; }
	std::vector<std::string> mine = CsCarBlocks(s);
	for (int m : models)
		for (int i = 0; i < 6; i++) {
			SIZE_T w = 0;
			if (!WriteProcessMemory(g.h, (LPVOID)(UINT_PTR)(CAR_CAMS + (m * 6 + i) * 36), mine[i].data(), 36, &w) || w != 36) { msg = "the game wouldn't take the car cameras"; return false; }
		}
	g_stockRaw.clear(); g_written.clear();            // (RaceVision's own car-camera adjustments restart from these)
	g_viewDirty = 3; ViewCacheClear(g);
	msg = std::to_string(models.size()) + (models.size() == 1 ? " car model" : " car models");
	return true;
}

// ---- sets: stock source, presets, recipes
static bool CsSource(const std::string &kind, const std::string &who, CsSet &out, std::string &from) {
	std::string stock = CsStockDir(kind, who);
	std::string file = CsGameFile(kind, who);
	std::string fn = kind == "track" ? (file.empty() ? who + ".cam" : file.substr(file.find_last_of("\\/") + 1)) : "make_a.cam";
	std::string b = ReadAll(stock + "\\" + fn);
	if (!b.empty() && CsReadBin(b, out) && out.track == (kind == "track")) { from = "stock"; return true; }
	if (!file.empty()) { b = ReadAll(file); if (!b.empty() && CsReadBin(b, out) && out.track == (kind == "track")) { from = "game"; return true; } }
	std::string pt = ReadAll(CsPresetPath(kind, who, "stock"));      // a series whose file is packed: the saved text of it
	if (!pt.empty()) { std::string err; if (CsFromText(pt, out, err) && out.track == (kind == "track")) { from = "preset"; return true; } }
	if (kind == "track" && g_game && g_game->h && who == g_trackName && g_game->Get<unsigned>(CS_TV_WORK, 99999) <= 64) {   // the loaded tables
		out = CsSet(); out.track = true;
		std::vector<unsigned char> img;
		if (g_csSnapKey == CsSessionKey() && !g_csSnapTv.empty()) img = g_csSnapTv; else CsReadTv(*g_game, CS_TV_WORK, img);
		for (int k = 0; k < 3; k++) {
			unsigned n; memcpy(&n, img.data() + k * CS_GROUP_BYTES, 4);
			for (unsigned i = 0; i < n && i < 64; i++) { CsTv c; memcpy(c.f, img.data() + k * CS_GROUP_BYTES + 4 + i * 57, 56); c.type = img[k * CS_GROUP_BYTES + 4 + i * 57 + 56]; out.grp[k].push_back(c); }
		}
		std::string pit = g_csSnapPit.size() == 72 && g_csSnapKey == CsSessionKey() ? std::string((const char *)g_csSnapPit.data(), 72) : std::string(72, '\0');
		if (pit.find_first_not_of('\0') == std::string::npos) g_game->Read(PIT1_BLOCK, &pit[0], 72);
		memcpy(out.pit, pit.data(), 72);
		out.tail = std::string(16, '\0'); g_game->Read(0xC57440, &out.tail[0], 16);
		out.ok = true; from = "memory"; return true;
	}
	return false;
}
static const CsRecipe *CsRecipeFor(const std::string &kind, const std::string &name) {
	for (auto &r : CS_RECIPES) if (name == r.name && (kind == "track") == (r.track[0] == '1')) return &r;
	return nullptr;
}
static bool CsGetSet(const std::string &kind, const std::string &who, const std::string &name, CsSet &out, std::string &err) {
	std::string from;
	if (!CsSafeName(who) || !CsSafeName(name)) { err = "bad name"; return false; }
	if (name == "stock") {
		if (CsSource(kind, who, out, from)) return true;
		err = "the game's own camera file isn't saved yet (and NR2003 has no session loaded for it)"; return false;
	}
	std::string txt = ReadAll(CsPresetPath(kind, who, name));
	if (!txt.empty()) {
		if (!CsFromText(txt, out, err)) { err = name + ": " + err; return false; }
		if (out.track != (kind == "track")) { err = name + " is a " + (out.track ? "track" : "car") + " set"; return false; }
		return true;
	}
	if (const CsRecipe *r = CsRecipeFor(kind, name)) {
		if (!CsSource(kind, who, out, from)) { err = "no camera file to make " + name + " from"; return false; }
		r->run(out);
		return true;
	}
	err = "no such set"; return false;
}
struct CsEntry { std::string name, desc; bool preset, builtin; };
static std::string CsDescribe(const std::string &kind, const std::string &who, const std::string &name) {
	std::string txt = ReadAll(CsPresetPath(kind, who, name));
	if (txt.empty()) return "";
	std::string first = txt.substr(0, txt.find('\n'));
	size_t i = first.find('(');
	std::string d = i == std::string::npos ? "" : first.substr(i + 1);
	while (!d.empty() && (d.back() == ')' || d.back() == '\r')) d.pop_back();
	size_t c = d.find(": "); if (c != std::string::npos) d = d.substr(c + 2);
	return d;
}
static std::vector<CsEntry> CsEntries(const std::string &kind, const std::string &who) {
	std::vector<CsEntry> v;
	std::set<std::string> have;
	v.push_back({ "stock", "The game's own cameras.", true, false }); have.insert("stock");
	std::vector<std::string> files = CsFiles(CsRoot() + "\\presets\\" + kind + "\\" + who, ".txt");
	std::sort(files.begin(), files.end());
	for (auto &f : files) {
		std::string n = f.substr(0, f.size() - 4);
		if (have.count(n)) continue;
		have.insert(n);
		std::string d = CsDescribe(kind, who, n);
		if (d.empty() && n.rfind("tv from ", 0) == 0) d = "TV cameras borrowed from another version of the track.";
		v.push_back({ n, d, true, false });
	}
	for (auto &r : CS_RECIPES)
		if ((kind == "track") == (r.track[0] == '1') && !have.count(r.name)) v.push_back({ r.name, r.desc, false, true });
	return v;
}

// ---- what is in use: saved for the next session (applied.txt) and loaded now (the memory tables match a set)
static std::string CsApplied(const std::string &kind, const std::string &who) {
	std::string t = Trim(ReadAll(CsStockDir(kind, who) + "\\applied.txt"));
	return t.empty() ? "stock" : t;
}
static bool CsPacked(const std::string &kind, const std::string &who) { return IsFile(CsStockDir(kind, who) + "\\packed.txt"); }
static std::string CsLoadedTrack(Game &g, const std::string &who, const std::vector<CsEntry> &es) {
	if (!g.h || who != g_trackName || g.Get<unsigned>(CS_TV_WORK, 99999) > 64) return "";
	std::vector<unsigned char> mem, img;
	if (!CsReadTv(g, CS_TV_WORK, mem)) return "";
	std::string first;
	for (auto &e : es) {
		CsSet s; std::string err;
		if (!CsGetSet("track", who, e.name, s, err)) continue;
		CsImage(s, img);
		if (CsSameTv(mem, img)) { if (e.name != "stock") return e.name; if (first.empty()) first = e.name; }
	}
	return first.empty() ? "custom" : first;
}
static std::string CsLoadedCar(Game &g, const std::string &who, const std::vector<CsEntry> &es) {
	if (!g.h || g.Get<unsigned>(CS_TV_WORK, 99999) > 64) return "";
	std::vector<int> models; std::string first;
	for (auto &e : es) {
		CsSet s; std::string err;
		if (!CsGetSet("series", who, e.name, s, err)) continue;
		if (CsMatchCar(g, CsCarBlocks(s), models)) { if (e.name != "stock") return e.name; if (first.empty()) first = e.name; }
	}
	return first.empty() ? "" : first;
}
// which series' car cameras these are. The cameras in memory are matched against each series' make_a.cam, but many mods keep the
// default car cameras, so a match is only a GUESS (the first series that has them, often the Busch one) until the user picks the
// mod: the pick is remembered per art series (the mod the director is showing) in racevision\car_series.txt.
static std::string g_csSeriesWhy;                          // "chosen" | "guess" | ""
static std::vector<std::string> g_csSeriesCands;           // every series folder the user can pick
static std::string CsSeriesKey() { return g_artSeries.empty() ? "default" : g_artSeries; }
static std::string CsSeriesPickFile() { std::string r = CsRoot(); return r.empty() ? "" : r + "\\car_series.txt"; }
static std::vector<std::pair<std::string, std::string>> CsSeriesPicks() {         // "<art series>=<game series folder>" lines
	std::vector<std::pair<std::string, std::string>> v;
	std::string f = CsSeriesPickFile(), t = f.empty() ? "" : ReadAll(f);
	size_t a = 0;
	while (a < t.size()) {
		size_t b = t.find('\n', a);
		std::string ln = t.substr(a, b == std::string::npos ? std::string::npos : b - a);
		a = b == std::string::npos ? t.size() : b + 1;
		while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
		size_t e = ln.find('=');
		if (e != std::string::npos && e > 0) v.push_back({ ln.substr(0, e), ln.substr(e + 1) });
	}
	return v;
}
static std::string CsSeriesChoice() {
	for (auto &kv : CsSeriesPicks()) if (kv.first == CsSeriesKey()) return kv.second;
	return "";
}
static bool CsSeriesChoose(const std::string &name) {
	std::string f = CsSeriesPickFile(), k = CsSeriesKey();
	if (f.empty() || !CsSafeName(name)) return false;
	std::string out;
	for (auto &kv : CsSeriesPicks()) if (kv.first != k) out += kv.first + "=" + kv.second + "\n";
	return CsWriteFile(f, out + k + "=" + name + "\n");
}
static std::string CsCurrentSeries(Game &g) {
	std::string game = CsGame();
	g_csSeriesWhy.clear(); g_csSeriesCands.clear();
	std::set<std::string> seen;
	for (auto &sr : CsSubdirs(game + "\\series")) if (CsSafeName(sr) && seen.insert(sr).second) g_csSeriesCands.push_back(sr);
	for (auto &sr : CsSubdirs(CsRoot() + "\\presets\\series")) if (CsSafeName(sr) && seen.insert(sr).second) g_csSeriesCands.push_back(sr);
	std::string ch = CsSeriesChoice();
	if (!ch.empty() && seen.count(ch)) { g_csSeriesWhy = "chosen"; return ch; }
	std::vector<int> models;
	if (g.h && !game.empty() && g.Get<unsigned>(CS_TV_WORK, 99999) <= 64) {
		for (auto &sr : CsSubdirs(game + "\\series")) {
			CsSet s, st; std::string err, from;
			std::string b = ReadAll(game + "\\series\\" + sr + "\\make_a.cam");
			if (!b.empty() && CsReadBin(b, s) && !s.track && CsMatchCar(g, CsCarBlocks(s), models)) { g_csSeriesWhy = "guess"; return sr; }
			if (CsSource("series", sr, st, from) && !st.track && CsMatchCar(g, CsCarBlocks(st), models)) { g_csSeriesWhy = "guess"; return sr; }
		}
	}
	std::vector<std::string> d = CsSubdirs(CsRoot() + "\\presets\\series");
	if (!d.empty()) g_csSeriesWhy = "guess";
	return d.empty() ? "" : d[0];
}

// ---- applying: the live tables and / or the file the game reads at the next session load
static bool CsSaveFile(const std::string &kind, const std::string &who, const std::string &name, const CsSet &s, std::string &msg) {
	std::string file = CsGameFile(kind, who);
	if (CsRoot().empty()) { msg = "no camsets folder found"; return false; }
	if (file.empty()) { msg = "the NR2003 folder isn't known yet"; return false; }
	std::string stock = CsStockDir(kind, who);
	std::string stockFile = stock + "\\" + file.substr(file.find_last_of("\\/") + 1);
	if (!IsFile(stockFile)) {                                   // the game's own file, saved once before anything is changed
		if (!IsFile(file)) { msg = "this track's cameras are packed inside its .dat - saved for next session isn't possible (live only)"; return false; }
		std::string b = ReadAll(file);
		if (b.empty()) { msg = "could not read " + file; return false; }
		std::string d = stock; for (size_t p = 0; (p = d.find('\\', p + 1)) != std::string::npos;) CreateDirectoryA(d.substr(0, p).c_str(), NULL);
		CreateDirectoryA(stock.c_str(), NULL);
		if (!CsWriteFile(stockFile, b)) { msg = "could not save the game's own file to " + stock; return false; }
	}
	if (!CsWriteFile(file, CsWriteBin(s))) { msg = "could not write " + file; return false; }
	CsWriteFile(stock + "\\applied.txt", name + "\n");
	return true;
}
static bool CsRestoreFile(const std::string &kind, const std::string &who, std::string &msg) {
	std::string file = CsGameFile(kind, who);
	if (file.empty() || CsRoot().empty()) { msg = "folders not found"; return false; }
	std::string stock = CsStockDir(kind, who);
	if (CsPacked(kind, who)) { DeleteFileA(file.c_str()); }
	else {
		std::string b = ReadAll(stock + "\\" + file.substr(file.find_last_of("\\/") + 1));
		if (b.empty()) { msg = "the game's own file was never saved here, so there is nothing to put back"; return false; }
		if (!CsWriteFile(file, b)) { msg = "could not write " + file; return false; }
	}
	DeleteFileA((stock + "\\applied.txt").c_str());
	return true;
}

// ---- JSON for the page
static std::string CsTrackOnMap(const std::string &t) {
	char pd[MAX_PATH];
	if (!GetEnvironmentVariableA("PROGRAMDATA", pd, MAX_PATH)) return "";
	return IsFile(std::string(pd) + "\\NR2003 Companions\\tracks\\" + t + "\\map.json") ? t : "";
}
// RaceVision's own name for a set: "RaceVision's Short Track Wide Set" (the file names stay as they were: they are what the files are called)
static std::string CsTitle(const std::string &kind, const std::string &name, double lap) {
	if (name == "stock") return "The game's own";
	static const char *pairs[][2] = { { "tv tight", "Tight" }, { "tv wide", "Wide" }, { "tv more cuts", "More Cuts" }, { "tv more cuts tight", "More Cuts Tight" },
		{ "tv onboard", "Door and Roof Onboard" }, { "tv onboard + nose", "Door, Roof and Nose Onboard" }, { "tv onboard + roof fwd test", "Roof Forward Onboard" },
		{ "external", "Outside Chase" }, { "external 2", "Broadside and Head-On" }, { "mix", "Mixed Onboard" }, { "broadcast", "Broadcast Onboard" }, { "broadcast tight", "Broadcast Onboard Tight" } };
	std::string style;
	for (auto &p : pairs) if (name == p[0]) style = p[1];
	if (style.empty() && name.rfind("tv from ", 0) == 0) style = "Borrowed Cameras (" + name.substr(8) + ")";
	if (style.empty()) { style = name; bool up = true; for (char &c : style) { if (up && c >= 'a' && c <= 'z') c = (char)(c - 32); up = c == ' '; } }
	std::string cls = kind != "track" || lap <= 0 ? "" : lap < 1400 ? "Short Track " : lap < 2900 ? "Intermediate " : lap < 3700 ? "Large Oval " : "Superspeedway ";
	return "RaceVision's " + cls + style + " Set";
}
static std::string CsEntriesJson(const std::string &kind, const std::string &who, const std::vector<CsEntry> &es, const std::string &loaded, const std::string &saved) {
	std::string o = "[";
	double lap = 0;
	if (kind == "track") { if (who == g_trackName && g_tvLen > 0) lap = g_tvLen; else { TrackMapPts m; if (LoadTrackMap(who, m)) lap = m.len; } }
	for (size_t i = 0; i < es.size(); i++) {
		const CsEntry &e = es[i];
		o += std::string(i ? "," : "") + "{\"name\":" + J(e.name) + ",\"title\":" + J(CsTitle(kind, e.name, lap)) + ",\"desc\":" + J(e.desc) + ",\"builtin\":" + (e.builtin ? "true" : "false") +
		     ",\"loaded\":" + (e.name == loaded ? "true" : "false") + ",\"saved\":" + (e.name == saved ? "true" : "false");
		if (kind == "track") {
			CsSet s; std::string err;
			if (CsGetSet(kind, who, e.name, s, err)) o += ",\"cams\":[" + std::to_string(s.grp[0].size()) + "," + std::to_string(s.grp[1].size()) + "," + std::to_string(s.grp[2].size()) + "]";
		}
		o += "}";
	}
	return o + "]";
}
static std::string CsListJson(Game &g, const std::string &trackWanted) {
	std::string root = CsRoot();
	bool att = g.h != NULL, loadedSession = att && g.Get<unsigned>(CS_TV_WORK, 99999) <= 64 && !g_trackName.empty();
	std::string track = !trackWanted.empty() ? trackWanted : (!g_trackName.empty() ? g_trackName : g_tvTrack);
	std::string o = std::string("{\"root\":") + J(root) + ",\"game\":" + J(CsGame()) + ",\"attached\":" + (att ? "true" : "false") +
	                ",\"session\":" + (loadedSession ? "true" : "false") + ",\"live\":" + (loadedSession && CsLiveSig(g) ? "true" : "false") +
	                ",\"current\":" + J(g_trackName) + ",\"track\":" + J(track);
	std::vector<std::string> names;
	std::set<std::string> seen;
	auto add = [&](const std::string &t) { if (CsSafeName(t) && seen.insert(t).second) names.push_back(t); };
	if (!track.empty()) add(track);
	for (auto &t : CsSubdirs(root + "\\presets\\track")) add(t);
	for (auto &t : CsSubdirs(CsGame() + "\\tracks")) if (CsTrackOnMap(t) != "") add(t);
	o += ",\"tracks\":[";
	for (size_t i = 0; i < names.size(); i++)
		o += std::string(i ? "," : "") + "{\"name\":" + J(names[i]) + ",\"map\":" + (CsTrackOnMap(names[i]).empty() ? "false" : "true") +
		     ",\"presets\":" + (CsDir(root + "\\presets\\track\\" + names[i]) ? "true" : "false") + "}";
	o += "]";
	if (!track.empty() && CsSafeName(track)) {
		std::vector<CsEntry> es = CsEntries("track", track);
		std::string loaded = CsLoadedTrack(g, track, es);
		bool haveFile = !CsGameFile("track", track).empty() && (IsFile(CsGameFile("track", track)) || CsPacked("track", track));
		o += ",\"trackSets\":" + CsEntriesJson("track", track, es, loaded, CsApplied("track", track)) + ",\"trackLoaded\":" + J(loaded) +
		     ",\"trackSaved\":" + J(CsApplied("track", track)) + ",\"trackFile\":" + (haveFile ? "true" : "false") +
		     ",\"packed\":" + (CsPacked("track", track) ? "true" : "false") + ",\"liveTrack\":" + J(g_csLiveTrackSet);
	}
	std::string series = CsCurrentSeries(g);
	o += ",\"series\":" + J(series) + ",\"seriesWhy\":" + J(g_csSeriesWhy) + ",\"seriesCands\":[";
	for (size_t i = 0; i < g_csSeriesCands.size(); i++) o += std::string(i ? "," : "") + J(g_csSeriesCands[i]);
	o += "]";
	if (!series.empty()) {
		std::vector<CsEntry> es = CsEntries("series", series);
		std::string loaded = CsLoadedCar(g, series, es);
		o += ",\"seriesSets\":" + CsEntriesJson("series", series, es, loaded, CsApplied("series", series)) + ",\"seriesLoaded\":" + J(loaded) + ",\"seriesSaved\":" + J(CsApplied("series", series));
	}
	return o + "}";
}
// one set as the map's camera bands: same shape as the live map (TlTvArrayJson, racevision_timeline.cpp)
static std::string TlTvArrayJson(const TrackMapPts &m, double lap, const std::vector<TvCam> g[3]);
static std::string CsPreviewJson(Game &g, const std::string &track, const std::string &name) {
	CsSet s; std::string err;
	if (!CsSafeName(track) || !CsGetSet("track", track, name, s, err)) return "{\"error\":" + J(err.empty() ? "bad name" : err) + "}";
	TrackMapPts m; bool own = false;
	if (!LoadTrackMap(track, m)) {
		if (track == g_tvTrack && g_tlMapOwn && g_tlMap.x.size() >= 100) { m = g_tlMap; own = true; }
		else return "{\"error\":\"No track map for " + track + " yet - Race Control or RaceVision draws it from the cars in the first lap of a session.\"}";
	}
	double lap = track == g_tvTrack && g_tvLen > 0 ? g_tvLen : m.len;
	std::vector<TvCam> v[3];
	for (int k = 0; k < 3; k++) {
		for (size_t i = 0; i < s.grp[k].size(); i++) {
			const CsTv &c = s.grp[k][i];
			TvCam t; t.sf = c.f[0]; t.cl = c.f[1]; t.h = c.f[2]; t.take = c.f[3]; t.type = c.type; t.n = (int)i + 1; t.zoom = c.f[6];
			t.from = (float)CsMod((double)t.sf + t.take, lap);
			v[k].push_back(t);
		}
		std::sort(v[k].begin(), v[k].end(), [](const TvCam &a, const TvCam &b) { return a.from < b.from; });
	}
	std::string o = "{\"track\":" + J(track) + ",\"name\":" + J(name) + ",\"len\":" + TlNum(m.len, 2) + ",\"lap\":" + TlNum(lap, 2) +
	                ",\"from\":" + J(own ? "cars" : "racecontrol") + ",\"pts\":[";
	for (size_t i = 0; i < m.x.size(); i++) { char b[48]; snprintf(b, sizeof b, "%s%.1f,%.1f", i ? "," : "", m.x[i], m.y[i]); o += b; }
	o += "],\"tv\":" + TlTvArrayJson(m, lap, v) + ",\"pit\":[";
	for (int p = 0; p < 2; p++) {
		char b[160]; snprintf(b, sizeof b, "%s{\"cam\":%d,\"x\":%.1f,\"y\":%.1f}", p ? "," : "", 17 + p, s.pit[p][0], s.pit[p][1]);
		o += b;
	}
	return o + "]}";
}

// ---- the page's calls: /api/camsets (GET), /api/camsets/preview (GET), /api/camsets/apply + /restore (POST)
static std::string CsApi(Game &g, const std::string &what, std::map<std::string, std::string> &q, bool post, int &code) {
	code = 200;
	auto err = [&](const std::string &m) { code = 400; return "{\"error\":" + J(m) + "}"; };
	if (what == "") return CsListJson(g, q["track"]);
	if (what == "series") {
		if (!post) return err("use POST");
		if (!CsSeriesChoose(q["name"])) return err("couldn't remember that series");
		Log("Camera sets: car cameras belong to %s (remembered for %s)\n", q["name"].c_str(), CsSeriesKey().c_str());
		return "{\"ok\":true}";
	}
	if (what == "preview") {
		std::string r = CsPreviewJson(g, q["track"], q["name"]);
		if (r.rfind("{\"error\"", 0) == 0) code = 400;
		return r;
	}
	if ((what != "apply" && what != "restore") || !post) return err("use POST");
	std::string kind = q["kind"] == "series" ? "series" : "track", who = q["who"], name = what == "restore" ? "stock" : q["name"];
	bool live = q["live"] != "0", save = q["save"] != "0";
	if (!CsSafeName(who) || !CsSafeName(name)) return err("bad name");
	if (CsRoot().empty()) return err("RaceVision can't find the camsets folder (camsets_dir in racevision.ini)");
	CsSet s; std::string e;
	if (!CsGetSet(kind, who, name, s, e)) return err(e);
	std::string liveMsg, saveMsg;
	bool liveOk = false, saveOk = false;
	bool here = kind == "series" || who == g_trackName;
	if (live) {
		if (!g.h) liveMsg = "NR2003 isn't running";
		else if (!here) liveMsg = "that isn't the track NR2003 has loaded";
		else if (kind == "track") liveOk = CsApplyTrackLive(g, s, liveMsg);
		else {
			std::vector<CsSet> known;
			CsSet st, ap; std::string from, ee;
			if (CsSource("series", who, st, from)) known.push_back(st);
			std::string cur = ReadAll(CsGameFile("series", who));
			if (!cur.empty() && CsReadBin(cur, ap) && !ap.track) known.push_back(ap);
			if (!g_csLiveCarSet.empty() && CsGetSet("series", who, g_csLiveCarSet, ap, ee)) known.push_back(ap);
			liveOk = CsApplyCarLive(g, s, known, liveMsg);
		}
		if (liveOk) { (kind == "track" ? g_csLiveTrackSet : g_csLiveCarSet) = name; }
	}
	if (save) {
		if (name == "stock") saveOk = CsRestoreFile(kind, who, saveMsg);
		else saveOk = CsSaveFile(kind, who, name, s, saveMsg);
	}
	Log("Camera sets: %s %s %s - live: %s, saved: %s\n", name == "stock" && what == "restore" ? "back to the game's own" : "using", who.c_str(), name.c_str(),
	    !live ? "not asked" : liveOk ? "yes" : liveMsg.c_str(), !save ? "not asked" : saveOk ? "yes" : saveMsg.c_str());
	std::string o = "{\"ok\":" + std::string((live ? liveOk : true) || (save && saveOk) ? "true" : "false") + ",\"live\":" + (live ? (liveOk ? "\"done\"" : "\"failed\"") : "\"off\"") +
	                ",\"liveMsg\":" + J(liveMsg) + ",\"saved\":" + (save ? (saveOk ? "\"done\"" : "\"failed\"") : "\"off\"") + ",\"saveMsg\":" + J(saveMsg) + "}";
	if (!liveOk && !saveOk && (live || save)) code = 400;
	return o;
}

// ---- self-test (RaceVision.exe --selftest out.txt): the binary and text formats round-trip exactly, the ported recipes make
// the same files camsets.py made, and the files in the game are the sets camsets.py says are applied
static void CsSelfTest(FILE *out) {
	int ok = 0, bad = 0;
	auto check = [&](bool c, const std::string &what) { if (c) ok++; else { bad++; fprintf(out, "FAIL  %s\n", what.c_str()); } };
	std::string root = CsRoot();
	fprintf(out, "camsets root: %s\ngame: %s\n", root.c_str(), CsGame().c_str());
	for (const char *kind : { "track", "series" }) {
		for (auto &who : CsSubdirs(root + std::string("\\presets\\") + kind)) {
			std::string stockBytes, stockName;
			for (auto &f : CsFiles(CsStockDir(kind, who), ".cam")) { stockName = f; stockBytes = ReadAll(CsStockDir(kind, who) + "\\" + f); }
			if (!stockBytes.empty()) {
				CsSet s; check(CsReadBin(stockBytes, s) && CsWriteBin(s) == stockBytes, std::string("stock round trip ") + kind + "/" + who);
			}
			for (auto &e : CsEntries(kind, who)) {
				CsSet s; std::string err;
				bool got = CsGetSet(kind, who, e.name, s, err);
				if (e.name == "stock" && stockBytes.empty()) continue;
				check(got, std::string("load ") + kind + "/" + who + "/" + e.name + " " + err);
				if (!got) continue;
				std::string bin = CsWriteBin(s);
				CsSet again; check(CsReadBin(bin, again) && CsWriteBin(again) == bin, std::string("bin round trip ") + who + "/" + e.name);
				// a preset made by a recipe = the ported recipe on the stock file
				const CsRecipe *r = CsRecipeFor(kind, e.name);
				if (r && e.preset && !stockBytes.empty()) {
					CsSet base; CsReadBin(stockBytes, base); r->run(base);
					bool same = CsWriteBin(base) == bin;
					check(same, std::string("recipe '") + e.name + "' reproduces the preset of " + who);
				}
			}
			std::string ap = Trim(ReadAll(CsStockDir(kind, who) + "\applied.txt"));
			if (!ap.empty()) {
				CsSet s; std::string err;
				if (CsGetSet(kind, who, ap, s, err)) check(ReadAll(CsGameFile(kind, who)) == CsWriteBin(s), std::string("the game's file is '") + ap + "' for " + who);
				else check(false, "applied set loads: " + who + "/" + ap + " " + err);
			}
		}
	}
	fprintf(out, "camera sets self-test: %d ok, %d failed\n", ok, bad);
}
