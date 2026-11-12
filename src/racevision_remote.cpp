// RaceVision phone remote (included by tv_director.cpp after racevision_ui.cpp)
//
// A small web server on the home network. The phone opens the page from the QR code on the
// window's Phone page (http://<this PC>:<port>/?k=<pairing code>) and gets the running order,
// the cameras, what the director sees and the settings. The page (racevision_phone.html, built
// into the exe) asks for /api/state twice a second; a tap is a POST. Every /api call needs the
// pairing code, so only phones that scanned it can steer. Each connection gets its own short
// thread: race data is read under g_lock, a car or camera pick goes to the main thread
// (g_reqFocus, g_reqCam, g_reqAuto) and settings and presets to the window's thread
// (WM_APP_REMOTE), the way the window itself does it.

#include "racevision_qr.cpp"

static std::string MakeKey() {
	static const char *abc = "abcdefghjkmnpqrstuvwxyz23456789";   // no 0/o, 1/l/i: easy to type
	std::random_device rd;
	std::string k;
	for (int i = 0; i < 8; i++) k += abc[rd() % 31];
	return k;
}
static void NewRemoteKey() {
	EnterCriticalSection(&g_lock);
	SaveKey("remote_key", MakeKey());
	g_phones.clear();
	Log("-- new phone pairing code: phones need to scan the new QR code --\n");
	LeaveCriticalSection(&g_lock);
}

// ---- text: the game's names are ANSI, the phone speaks UTF-8
static std::string Recode(const std::string &s, UINT from, UINT to) {
	if (s.empty()) return s;
	int n = MultiByteToWideChar(from, 0, s.c_str(), (int)s.size(), NULL, 0);
	std::wstring w(n, 0);
	MultiByteToWideChar(from, 0, s.c_str(), (int)s.size(), &w[0], n);
	int m = WideCharToMultiByte(to, 0, w.c_str(), n, NULL, 0, NULL, NULL);
	std::string out(m, 0);
	WideCharToMultiByte(to, 0, w.c_str(), n, &out[0], m, NULL, NULL);
	return out;
}
static std::string J(const std::string &ansi) {
	std::string o = "\"";
	for (unsigned char ch : Recode(ansi, CP_ACP, CP_UTF8)) {
		if (ch == '"' || ch == '\\') { o += '\\'; o += (char)ch; }
		else if (ch < 32) { char b[8]; snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
		else o += (char)ch;
	}
	return o + "\"";
}
static const char *B(bool b) { return b ? "true" : "false"; }
static std::string UrlDecode(const std::string &s) {
	std::string o;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '+') o += ' ';
		else if (s[i] == '%' && i + 2 < s.size()) { o += (char)strtol(s.substr(i + 1, 2).c_str(), NULL, 16); i += 2; }
		else o += s[i];
	}
	return o;
}

// ---- the replays list: every crash and highlight logged this race, best first, with what is playing now
static std::string ReplaysJson() {
	std::string o = "{\"crashes\":[";
	char b[256];
	EnterCriticalSection(&g_lock);
	std::vector<const Crash *> list;
	for (auto &c : g_crashes) list.push_back(&c);
	for (auto &c : g_highs) list.push_back(&c);
	std::sort(list.begin(), list.end(), [](const Crash *x, const Crash *y) { return Worth(*x) > Worth(*y); });
	bool first = true;
	for (const Crash *c : list) {
		snprintf(b, sizeof b, "{\"id\":%d,\"lap\":%d,\"cars\":%d,\"damage\":%.2f,\"worth\":%.1f,\"worthy\":%s,\"replayed\":%s,\"ago\":%.0f,",
		         c->id, c->lap, (int)c->cars.size(), c->damage, Worth(*c), B(c->kind || Worth(*c) >= S.replayWorthy), B(c->replayed),
		         std::max(0.0, g_simT - c->first));
		o += std::string(first ? "" : ",") + b + "\"who\":" + J(CrashWho(*c, 5)) + ",\"kind\":\"" + HlKey(c->kind) + "\",\"what\":" +
		     J(c->kind ? c->what : std::string()) + "}";
		first = false;
	}
	snprintf(b, sizeof b, "],\"playing\":%d,\"take\":%d,\"takes\":%d,\"reel\":%d,\"plan\":%d,\"inCar\":%s}",
	         g_rp.phase ? g_rp.crash : -1, g_rp.take + 1, (int)g_rp.takes.size(), (int)g_reel.size() + (g_rp.reel ? 1 : 0),
	         g_plan.crash, B(g_youDrive));
	o += b;
	LeaveCriticalSection(&g_lock);
	return o;
}

// ---- what the phone shows
static std::string StateJson() {
	std::string o;
	char b[256];
	EnterCriticalSection(&g_lock);
	bool att = g_game && g_game->h, race = att && g_session == SESSION_RACE;
	o += std::string("{\"attached\":") + B(att) + ",\"camOk\":" + B(g_game && g_game->camOk) + ",\"paused\":" + B(g_paused) +
	     ",\"overlays\":" + B(g_overlaysOn) + ",\"race\":" + B(race);
	o += ",\"flag\":" + J(!race ? "" : g_flag == FLAG_GREEN ? "green" : g_flag == 5 ? "one to go" : "caution");
	snprintf(b, sizeof b, ",\"lap\":%d,\"laps\":%d,\"cam\":%d,\"cars\":%d", g_leaderLaps, g_lapsTotal, g_lastCam, (int)g_order.size());
	o += b;
	o += ",\"track\":" + J(g_trackName);
	bool art = !g_artSeriesDir.empty() && g_artSeries != "?";
	o += ",\"series\":" + J(art ? g_artSeries : "") + ",\"logo\":" + B(art && IsFile(g_artSeriesDir + "\\serieslogo.png"));
	if (att && g_shot.car >= 0 && g_car[g_shot.car].valid) {
		const Car &c = g_car[g_shot.car];
		bool manual = g_shot.kind == kManual;
		double left = manual ? std::max(0.0, S.manualHold - (Now() - g_shot.since)) : 0;
		float rpm = g_game->RecF(g_shot.car, REC_RPM);
		float throttle = g_game->RecF(g_shot.car, REC_THROTTLE);
		float speedMph = c.speed * 2.23694f;
		snprintf(b, sizeof b, "{\"slot\":%d,\"num\":\"%s\",\"pos\":%d,\"manual\":%s,\"left\":%.0f,\"rpm\":%.0f,\"gear\":%d,\"throttle\":%.3f,\"speedMph\":%.1f,",
		         g_shot.car, NumLabel(c.number).c_str(), c.spos, B(manual && left > 0), left, rpm, c.gear, throttle, speedMph);
		o += std::string(",\"on\":") + b + "\"name\":" + J(c.first + " " + c.last) + ",\"kind\":" + J(KindName[g_shot.kind]) +
		     ",\"why\":" + J(g_shot.why) + "}";
	} else o += ",\"on\":null";
	// The personal ticker follows the same delayed handoff as the broadcast camera.
	// On the in-lap, keep the completed driver's best-lap context until the next runner is near green.
	bool qualifying = g_qual && g_session == SESSION_PRACTICE;
	int personalCar = qualifying ? g_qualCar : g_shot.car;
	bool personalDone = personalCar >= 0 && personalCar < NUM_SLOTS &&
	    g_car[personalCar].aiState == AI_PRACTICE && g_car[personalCar].runEnd > 0 &&
	    g_car[personalCar].runEnd <= g_car[personalCar].lapNum;
	bool personalInLap = personalCar >= 0 && personalCar < NUM_SLOTS &&
	    (g_car[personalCar].aiState == AI_INLAP_S || g_car[personalCar].aiState == AI_INLAP_F);
	if (qualifying && (personalCar < 0 || personalDone || personalInLap)) {
		personalCar = -1;
	}
	bool personalInCar = personalCar >= 0 && personalCar < NUM_SLOTS && g_car[personalCar].valid;
	int displayCar = personalCar;
	if (qualifying && !personalInCar && g_qualCar >= 0 && g_qualCar < NUM_SLOTS && g_car[g_qualCar].valid)
		displayCar = g_qualCar; // retain the completed runner's best time while its in-lap finishes
	int bestLapMs = qualifying && displayCar >= 0 && displayCar < NUM_SLOTS ? g_car[displayCar].qBestMs : 0;
	o += std::string(",\"qualifying\":") + B(qualifying) + ",\"eSessionType\":" + std::to_string(qualifying ? 1 : g_session) +
	     ",\"bestLapMs\":" + std::to_string(bestLapMs);
	bool rcSingle = qualifying && g_ko.on && g_ko.mode == "single";
	if (rcSingle) {
		o += ",\"qualifyingLineup\":[";
		bool firstSlot = true;
		for (int slot : g_ko.order) {
			if (!firstSlot) o += ",";
			firstSlot = false;
			o += std::to_string(slot);
		}
		o += "]";
	} else o += ",\"qualifyingLineup\":[]";
	o += ",\"personal\":";
	if (att && displayCar >= 0 && displayCar < NUM_SLOTS && g_car[displayCar].valid) {
		const Car &c = g_car[displayCar];
		float rpm = personalInCar ? g_game->RecF(displayCar, REC_RPM) : 0;
		float throttle = personalInCar ? g_game->RecF(displayCar, REC_THROTTLE) : 0;
		float speedMph = c.speed * 2.23694f;
		snprintf(b, sizeof b, "{\"slot\":%d,\"num\":\"%s\",\"inCar\":%s,\"bestLapMs\":%d,\"rpm\":%.0f,\"gear\":%d,\"throttle\":%.3f,\"speedMph\":%.1f,",
		         displayCar, NumLabel(c.number).c_str(), B(personalInCar), c.qBestMs, rpm, personalInCar ? c.gear : -9, throttle, speedMph);
		o += std::string(b) + "\"name\":" + J(c.first + " " + c.last) + "}";
	} else o += "null";
	auto col = [](int number) {
		auto it = g_colors.find(NumKey(number));
		char h[16] = "";
		if (it != g_colors.end()) snprintf(h, sizeof h, "#%02x%02x%02x", it->second.GetR(), it->second.GetG(), it->second.GetB());
		return std::string(h);
	};
	o += ",\"rows\":[";
	bool first = true;
	if (att)
		for (int i : g_score) {
			const Car &c = g_car[i];
			if (!c.valid) continue;
			snprintf(b, sizeof b, "%s{\"slot\":%d,\"pos\":%d,\"num\":\"%s\",\"pit\":%s,\"tv\":%s,\"bestLapMs\":%d,\"col\":", first ? "" : ",", i, c.spos,
			         NumLabel(c.number).c_str(), B(c.pit), B(i == g_shot.car), c.qBestMs);
			o += b + J(col(c.number)) + ",\"name\":" + J(c.first + " " + c.last) + "}";
			first = false;
		}
	o += "],\"story\":[";
	if (race) {
		std::vector<Cand> cs = g_cands;
		std::sort(cs.begin(), cs.end(), [](const Cand &x, const Cand &y) { return x.score > y.score; });
		std::vector<int> seen;
		first = true;
		for (auto &k : cs) {
			if (seen.size() >= 8 || k.car < 0 || !g_car[k.car].valid) continue;
			if (std::find(seen.begin(), seen.end(), k.car) != seen.end()) continue;
			seen.push_back(k.car);
			const Car &c = g_car[k.car];
			snprintf(b, sizeof b, "%s{\"slot\":%d,\"num\":\"%s\",\"pos\":%d,\"score\":%.0f,\"tv\":%s,\"col\":", first ? "" : ",", k.car,
			         NumLabel(c.number).c_str(), c.spos, k.score, B(k.car == g_shot.car));
			o += b + J(col(c.number)) + ",\"kind\":" + J(KindName[k.kind]) + ",\"name\":" + J(c.first + " " + c.last) +
			     ",\"why\":" + J(k.why) + "}";
			first = false;
		}
	}
	o += "]}";
	LeaveCriticalSection(&g_lock);
	return o;
}

static std::string SettingsJson() {
	static const char *kinds[] = { "num", "toggle", "text", "color", "choice", "action", "cams" };
	std::string o = "{\"pages\":[";
	EnterCriticalSection(&g_lock);
	bool firstPage = true;
	for (int p : { pDirector, pBattles, pCameras, pOverlays }) {
		o += std::string(firstPage ? "" : ",") + "{\"name\":" + J(PageName[p]) + ",\"fields\":[";
		firstPage = false;
		bool first = true;
		for (auto &f : g_fields) {
			if (f.page != p || f.kind == fAction) continue;
			std::string help = f.kind == fCams ? "Pick them on the Cameras tab. None = the camera stays as it is." : f.help;
			o += std::string(first ? "" : ",") + "{\"key\":" + J(f.key) + ",\"label\":" + J(KeyLabel(f)) + ",\"help\":" + J(help) +
			     ",\"kind\":" + J(kinds[f.kind]) + ",\"value\":" + J(Raw(f.key)) + ",\"choices\":[";
			first = false;
			std::string list = f.choices + ";";
			size_t a = 0, e;
			bool firstChoice = true;
			while (f.kind == fChoice && (e = list.find(';', a)) != std::string::npos) {
				std::string one = list.substr(a, e - a); a = e + 1;
				size_t bar = one.find('|');
				if (bar == std::string::npos) continue;
				o += std::string(firstChoice ? "" : ",") + "[" + J(one.substr(0, bar)) + "," + J(one.substr(bar + 1)) + "]";
				firstChoice = false;
			}
			o += "]}";
		}
		o += "]}";
	}
	o += "],\"cams\":[";
	// in the order set on the Cameras tab (cam_order; the rest after, by number), hidden ones flagged (cam_hidden)
	std::vector<int> ids, order = IntList(Raw("cam_order")), hidden = IntList(Raw("cam_hidden"));
	for (int c = 1; c <= CamLast(); c++) if (c <= 21 || VCamById(c)) ids.push_back(c);   // (the dynamic cameras, DYN_A and up, are the director's: listed hidden, so they have a name)
	auto at = [&](int c) { auto it = std::find(order.begin(), order.end(), c); return it == order.end() ? 1000 + c : (int)(it - order.begin()); };
	std::stable_sort(ids.begin(), ids.end(), [&](int a, int b) { return at(a) < at(b); });
	for (size_t i = 0; i < ids.size(); i++) {
		int c = ids[i];
		char b[96]; snprintf(b, sizeof b, "%s{\"n\":%d,\"onboard\":%s,\"hidden\":%s,\"name\":", i ? "," : "", c, B(IsOnboard(c)),
		                     B(c >= DYN_A || std::find(hidden.begin(), hidden.end(), c) != hidden.end()));
		o += b + J(CamName(c)) + "}";
	}
	LeaveCriticalSection(&g_lock);
	o += "],\"presets\":[";
	bool first = true;
	for (auto &p : PresetList()) {
		char b[64]; snprintf(b, sizeof b, ",\"builtin\":%s,\"diffs\":%d}", B(p.builtin), p.diffs);
		o += std::string(first ? "" : ",") + "{\"name\":" + J(p.name) + b;
		first = false;
	}
	return o + "]}";
}

static std::string EncodePng(Bitmap &bmp) {
	std::string png;
	CLSID clsid = {};
	UINT n = 0, bytes = 0;
	GetImageEncodersSize(&n, &bytes);
	std::vector<BYTE> buf(bytes);
	ImageCodecInfo *enc = (ImageCodecInfo *)buf.data();
	GetImageEncoders(n, bytes, enc);
	for (UINT k = 0; k < n; k++) if (wcscmp(enc[k].MimeType, L"image/png") == 0) clsid = enc[k].Clsid;
	IStream *st = NULL;
	if (SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &st))) {
		if (bmp.Save(st, &clsid, NULL) == Ok) {
			STATSTG ss; st->Stat(&ss, STATFLAG_NONAME);
			HGLOBAL hg; GetHGlobalFromStream(st, &hg);
			png.assign((const char *)GlobalLock(hg), (size_t)ss.cbSize.QuadPart);
			GlobalUnlock(hg);
		}
		st->Release();
	}
	return png;
}

// driver art for the phone, from the series the overlays use: number cards and the series logo
// as they are, headshots shrunk (they are photo size) and kept once made
static std::string ArtPng(const std::string &kind, const std::string &num) {
	EnterCriticalSection(&g_lock);
	std::string dir = g_artSeriesDir;
	std::vector<std::string> roots = g_artRoots;
	LeaveCriticalSection(&g_lock);
	if (dir.empty()) return "";
	std::string path;
	if (kind == "num") path = dir + "\\numbers\\" + num + ".png";
	else if (kind == "logo") path = dir + "\\serieslogo.png";
	else if (kind == "head") {
		path = dir + "\\headshots\\" + num + ".png";
		for (size_t r = 0; !IsFile(path) && r < roots.size(); r++) path = roots[r] + "\\images\\defaultheadshot.png";
	}
	if (path.empty() || !IsFile(path)) return "";
	if (kind != "head") return ReadAll(path);
	static SRWLOCK lock = SRWLOCK_INIT;
	static std::map<std::string, std::string> made;
	AcquireSRWLockShared(&lock);
	auto it = made.find(path);
	std::string png = it == made.end() ? "" : it->second;
	ReleaseSRWLockShared(&lock);
	if (!png.empty()) return png;
	std::wstring wpath(MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, NULL, 0), 0);
	MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, &wpath[0], (int)wpath.size());
	Bitmap src(wpath.c_str());
	if (src.GetLastStatus() != Ok || src.GetHeight() == 0) return ReadAll(path);
	const UINT H = 240;
	if (src.GetHeight() <= H) png = ReadAll(path);
	else {
		UINT w = std::max<UINT>(1, src.GetWidth() * H / src.GetHeight());
		Bitmap thumb(w, H, PixelFormat32bppARGB);
		{
			Graphics gr(&thumb);
			gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
			gr.SetPixelOffsetMode(PixelOffsetModeHighQuality);
			gr.DrawImage(&src, 0, 0, (INT)w, (INT)H);
		}
		png = EncodePng(thumb);
		if (png.empty()) png = ReadAll(path);
	}
	AcquireSRWLockExclusive(&lock);
	made[path] = png;
	ReleaseSRWLockExclusive(&lock);
	return png;
}

// the home screen icon: the window's RV mark, square (phones round the corners themselves)
// the home screen icon and the page's logo: the RaceVision logo at the size asked for, on the app's dark
// background for icons (phones put a transparent icon on white or black), see-through for the page
static std::string LogoPng(int size, bool icon) {
	static std::map<int, std::string> made;
	EnterCriticalSection(&g_lock);
	std::string &png = made[size * 2 + icon];
	Bitmap *logo = png.empty() ? LogoBitmap() : NULL;
	if (logo) {
		Bitmap bmp(size, size, PixelFormat32bppARGB);
		{
			Graphics gr(&bmp);
			gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
			gr.SetPixelOffsetMode(PixelOffsetModeHighQuality);
			gr.Clear(icon ? C(255, 17, 19, 24) : C(0, 0, 0, 0));
			float pad = icon ? size * 0.04f : 0;
			gr.DrawImage(logo, pad, pad, size - 2 * pad, size - 2 * pad);
		}
		png = EncodePng(bmp);
	}
	std::string out = png;
	LeaveCriticalSection(&g_lock);
	return out;
}

static std::string PageHtml() {
	HRSRC r = FindResourceA(NULL, MAKEINTRESOURCEA(101), (LPCSTR)RT_RCDATA);
	if (!r) return "<h1>RaceVision</h1><p>The phone page is missing from this build.</p>";
	HGLOBAL g = LoadResource(NULL, r);
	return std::string((const char *)LockResource(g), SizeofResource(NULL, r));
}

// ---- one request
static void Reply(SOCKET s, int code, const char *type, const std::string &body, bool keep = false, bool cors = false) {
	const char *what = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : "Not Found";
	char head[512];
	snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nCache-Control: %s\r\n%s"
	         "Connection: close\r\n\r\n", code, what, type, (int)body.size(), keep ? "private, max-age=3600" : "no-store",
	         cors ? "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, OPTIONS\r\nAccess-Control-Allow-Private-Network: true\r\n" : "");
	std::string all = head + body;
	for (size_t sent = 0; sent < all.size();) {
		int n = send(s, all.c_str() + sent, (int)(all.size() - sent), 0);
		if (n <= 0) break;
		sent += n;
	}
}
static std::string Err(const std::string &msg) { return "{\"error\":" + J(msg) + "}"; }

// ---- AIMOD's scanner, passed on to the phone: AIMOD's web server only answers this PC, and the phone
// keeps talking to the one address it's paired with. Returns the HTTP status, 0 when AIMOD isn't there.
static int AimodGet(const std::string &target, std::string &body) {
	EnterCriticalSection(&g_lock);
	int port = atoi(Raw("aimod_port").c_str());
	LeaveCriticalSection(&g_lock);
	if (port < 1 || port > 65535) port = 8767;
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) return 0;
	sockaddr_in a = {};
	a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	u_long nb = 1;
	ioctlsocket(s, FIONBIO, &nb);                       // a short connect: AIMOD is on this PC or not at all
	connect(s, (sockaddr *)&a, sizeof a);
	fd_set w, e; FD_ZERO(&w); FD_SET(s, &w); FD_ZERO(&e); FD_SET(s, &e);
	timeval tv = { 0, 400000 };
	if (select(0, NULL, &w, &e, &tv) <= 0 || FD_ISSET(s, &e)) { closesocket(s); return 0; }
	nb = 0;
	ioctlsocket(s, FIONBIO, &nb);
	DWORD to = 3000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
	std::string req = "GET " + target + " HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", all;
	send(s, req.c_str(), (int)req.size(), 0);
	char buf[16384];
	int n;
	while ((n = recv(s, buf, sizeof buf, 0)) > 0) all.append(buf, n);
	closesocket(s);
	size_t sp = all.find(' '), end = all.find("\r\n\r\n");
	if (sp == std::string::npos || end == std::string::npos) return 0;
	body = all.substr(end + 4);
	return atoi(all.c_str() + sp + 1);
}

static void Serve(SOCKET s, const std::string &ip) {
	DWORD to = 4000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
	std::string req;
	char buf[4096];
	while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
		int n = recv(s, buf, sizeof buf, 0);
		if (n <= 0) return;
		req.append(buf, n);
	}
	size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
	if (sp1 == std::string::npos || sp2 == std::string::npos) return;
	std::string method = req.substr(0, sp1), target = req.substr(sp1 + 1, sp2 - sp1 - 1);
	size_t qm = target.find('?');
	std::string path = target.substr(0, qm);
	std::map<std::string, std::string> q;
	if (qm != std::string::npos) {
		std::string qs = target.substr(qm + 1) + "&";
		size_t a = 0, e;
		while ((e = qs.find('&', a)) != std::string::npos) {
			std::string kv = qs.substr(a, e - a); a = e + 1;
			size_t eq = kv.find('=');
			if (eq != std::string::npos) q[UrlDecode(kv.substr(0, eq))] = UrlDecode(kv.substr(eq + 1));
		}
	}
	std::string key = Raw("remote_key");
	bool keyOk = !key.empty() && q["k"] == key;
	const char *JSON = "application/json; charset=utf-8";
	// A companion overlay runs in a different browser origin. Give it a read-only,
	// loopback-only copy of the same car state the RaceVision overlays use.
	if (path == "/api/overlay-state") {
		if (ip != "127.0.0.1" && ip != "::1") { Reply(s, 403, JSON, Err("local overlay endpoint only"), false, true); return; }
		if (method == "OPTIONS") { Reply(s, 200, JSON, "", false, true); return; }
		if (method != "GET") { Reply(s, 400, JSON, Err("use GET"), false, true); return; }
		Reply(s, 200, JSON, StateJson(), false, true);
		return;
	}

	if (path == "/" || path == "/index.html") {
		// the home screen icon is made from the manifest, before the page's script runs: it has to
		// carry the code already, or the icon opens RaceVision unpaired
		std::string html = PageHtml();
		size_t at = html.find("href=\"/manifest.json\"");
		if (keyOk && at != std::string::npos) html.replace(at, 21, "href=\"/manifest.json?k=" + key + "\"");
		Reply(s, 200, "text/html; charset=utf-8", html);
		return;
	}
	if (path == "/director" || path == "/director.html") {        // the director view (racevision_timeline.cpp)
		Reply(s, 200, "text/html; charset=utf-8", DirectorHtml());
		return;
	}
	if (path == "/icon.png" || path == "/icon512.png" || path == "/logo.png") {
		std::string png = LogoPng(path == "/icon.png" ? 192 : path == "/logo.png" ? 256 : 512, path != "/logo.png");
		if (png.empty()) Reply(s, 404, "text/plain", "no logo");
		else Reply(s, 200, "image/png", png, true);
		return;
	}
	if (path == "/manifest.json") {
		Reply(s, 200, "application/manifest+json", std::string("{\"name\":\"RaceVision\",\"short_name\":\"RaceVision\",\"start_url\":\"/") +
		      (keyOk ? "?k=" + key : "") + "\",\"display\":\"standalone\",\"background_color\":\"#111318\",\"theme_color\":\"#111318\","
		      "\"icons\":[{\"src\":\"/icon.png\",\"sizes\":\"192x192\",\"type\":\"image/png\",\"purpose\":\"any\"},"
		      "{\"src\":\"/icon512.png\",\"sizes\":\"512x512\",\"type\":\"image/png\",\"purpose\":\"any\"}]}");
		return;
	}
	if (path.rfind("/art/", 0) == 0) {                    // /art/num|head|logo/<n>.png (the page adds the series, so a new one isn't cached)
		if (!keyOk) { Reply(s, 403, "text/plain", "not paired"); return; }
		size_t slash = path.find('/', 5);
		std::string kind = slash == std::string::npos ? "" : path.substr(5, slash - 5);
		std::string num = slash == std::string::npos ? "" : path.substr(slash + 1);     // the painted number: "01"
		if (num.size() > 4 && num.substr(num.size() - 4) == ".png") num.resize(num.size() - 4);
		bool ok = !num.empty() && num.size() <= 4 && num.find_first_not_of("0123456789") == std::string::npos;
		std::string png = ok ? ArtPng(kind, num) : "";
		if (png.empty()) Reply(s, 404, "text/plain", "no art", true);
		else Reply(s, 200, "image/png", png, true);
		return;
	}
	if (path.rfind("/api/", 0) != 0) { Reply(s, 404, "text/plain", "not found"); return; }
	if (!keyOk) { Reply(s, 403, JSON, Err("This phone isn't paired. Scan the QR code on RaceVision's Phone page again.")); return; }

	EnterCriticalSection(&g_lock);
	if (g_phones.find(ip) == g_phones.end()) Log("-- phone connected: %s --\n", ip.c_str());
	g_phones[ip] = Now();
	LeaveCriticalSection(&g_lock);

	if (path == "/api/state") { Reply(s, 200, JSON, StateJson()); return; }
	if (path == "/api/director/live") { Reply(s, 200, JSON, TlLiveJson()); return; }
	if (path == "/api/director/map") { Reply(s, 200, JSON, TlMapJsonGet()); return; }
	if (path == "/api/director/events") { Reply(s, 200, JSON, TlEventsJson(atoi(q["from"].c_str()))); return; }
	if (path == "/api/director/at") { Reply(s, 200, JSON, TlAtJson(atof(q["t"].c_str()))); return; }
	if (path == "/api/director/files") { Reply(s, 200, JSON, TlFilesJson()); return; }
	if (path == "/api/dyn") { int code = 200; std::string j = DynApi(*g_game, "", q, code); Reply(s, code, JSON, j); return; }   // dynamic cameras (racevision_dyncams.cpp)
	if (path == "/api/camsets" || path == "/api/camsets/preview") {          // camera sets (racevision_camsets.cpp)
		int code = 200;
		EnterCriticalSection(&g_lock);
		std::string j = CsApi(*g_game, path == "/api/camsets" ? "" : "preview", q, false, code);
		LeaveCriticalSection(&g_lock);
		Reply(s, code, JSON, j); return;
	}
	if (path == "/api/director/file") {
		std::string body;
		if (!TlFileRead(q["name"], body)) Reply(s, 404, JSON, Err("No such recording."));
		else Reply(s, 200, "application/x-ndjson; charset=utf-8", body);
		return;
	}
	if (path == "/api/settings") { Reply(s, 200, JSON, SettingsJson()); return; }
	if (path == "/api/replays") { Reply(s, 200, JSON, ReplaysJson()); return; }
	if (path == "/api/tape") { Reply(s, 200, JSON, TpJson(g_game)); return; }
	if (path == "/api/camedit" && method == "GET") {
		EnterCriticalSection(&g_lock); std::string j = CamEditJson(); LeaveCriticalSection(&g_lock);
		Reply(s, 200, JSON, j); return;
	}
	if (path == "/api/crew") {                            // AIMOD's crew chief view (beta)
		std::string body;
		int code = AimodGet("/crew.json", body);
		Reply(s, 200, JSON, code == 200 ? body : std::string("{\"enabled\":false}"));
		return;
	}
	if (path.rfind("/api/crew/", 0) == 0) {               // take a car / keep it / give it back / its calls, plan, adjustments
		std::string what = path.substr(10);
		if (what != "claim" && what != "ping" && what != "release" && what != "call" && what != "plan" && what != "adjust") { Reply(s, 404, JSON, Err("not found")); return; }
		if (what != "ping" && method != "POST") { Reply(s, 400, JSON, Err("use POST")); return; }
		auto enc = [](const std::string &v) {             // the phone's values, passed on safely
			std::string o; char h[4];
			for (unsigned char ch : v) {
				if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') o += (char)ch;
				else { snprintf(h, sizeof h, "%%%02X", ch); o += h; }
			}
			return o;
		};
		std::string target = "/crew/" + what + "?";
		for (const char *k : { "car", "name", "token", "caution", "pit", "service", "fuel", "lap", "clear", "wedge", "bar", "air", "tape" })
			if (q.count(k)) target += std::string(k) + "=" + enc(q[k]) + "&";
		std::string body;
		int code = AimodGet(target, body);
		if (code == 200) Reply(s, 200, JSON, body);
		else Reply(s, 400, JSON, Err("AIMOD isn't running (or the crew chief beta is off)."));
		return;
	}
	if (path.rfind("/api/scanner", 0) == 0) {             // AIMOD's scanner calls, the sound files and "phone only"
		EnterCriticalSection(&g_lock);
		bool on = Raw("remote_scanner") != "0";
		LeaveCriticalSection(&g_lock);
		if (!on) { Reply(s, 200, JSON, "{\"enabled\":false}"); return; }
		std::string body;
		if (path == "/api/scanner") {
			int code = AimodGet(std::string("/radio.json") + (q["listen"] == "1" ? "?listen=1" : ""), body);
			Reply(s, 200, JSON, code == 200 ? "{\"enabled\":true,\"aimod\":true,\"radio\":" + body + "}"
			                                : std::string("{\"enabled\":true,\"aimod\":false}"));
		} else if (path == "/api/scanner/phoneonly") {
			if (method != "POST") { Reply(s, 400, JSON, Err("use POST")); return; }
			int code = AimodGet(std::string("/radio/phone_only?on=") + (q["on"] == "1" ? "1" : "0"), body);
			Reply(s, code == 200 ? 200 : 400, JSON, code == 200 ? body : Err("AIMOD isn't running."));
		} else {                                            // /api/scanner/<n>.wav
			int n = atoi(path.c_str() + 13);
			int code = n > 0 ? AimodGet("/radio/" + std::to_string(n) + ".wav", body) : 0;
			if (code == 200) Reply(s, 200, "audio/wav", body);
			else Reply(s, 404, JSON, Err("That call is gone."));
		}
		return;
	}
	if (method != "POST") { Reply(s, 400, JSON, Err("use POST")); return; }
	const std::string ok = "{\"ok\":true}";
	if (path == "/api/dyn/test" || path == "/api/dyn/rate" || path == "/api/dyn/forget") {
		int code = 200;
		std::string j = DynApi(*g_game, path.substr(9), q, code);
		Reply(s, code, JSON, j); return;
	}
	if (path == "/api/camsets/apply" || path == "/api/camsets/restore" || path == "/api/camsets/series") {
		int code = 200;
		EnterCriticalSection(&g_lock);
		std::string j = CsApi(*g_game, path.substr(13), q, true, code);
		LeaveCriticalSection(&g_lock);
		Reply(s, code, JSON, j); return;
	}
	if (path == "/api/focus") {
		int slot = atoi(q["slot"].c_str());
		EnterCriticalSection(&g_lock);
		bool valid = slot >= 0 && slot < NUM_SLOTS && g_car[slot].valid && g_game && g_game->h;
		LeaveCriticalSection(&g_lock);
		if (!valid) { Reply(s, 400, JSON, Err("That car isn't on track.")); return; }
		g_reqFocus = slot;
		Reply(s, 200, JSON, ok);
	} else if (path == "/api/cam") {
		int c = atoi(q["c"].c_str());
		if (c < 1 || c > CamLast() || (c > 21 && !VCamById(c))) { Reply(s, 400, JSON, Err("No such camera.")); return; }
		g_reqCam = c;
		Reply(s, 200, JSON, ok);
	} else if (path == "/api/camlist") {             // the Cameras tab: do = name (n, name; empty = the usual one) /
		                                              // order (list = every camera, in order) / hide (n, on)
		std::string d = q["do"], err;
		int n = atoi(q["n"].c_str());
		auto put = [](const std::string &k, const std::string &v) { SaveKey(k, v); };
		auto join = [](const std::vector<int> &l) { std::string o; for (int x : l) o += (o.empty() ? "" : ", ") + std::to_string(x); return o; };
		EnterCriticalSection(&g_lock);
		bool known = n >= 1 && n <= CamLast() && (n <= 21 || VCamById(n));
		if (d == "name") {
			std::string v;
			for (char ch : q["name"]) if ((unsigned char)ch >= 32 && ch != '=' && ch != ';' && v.size() < 28) v += ch;
			v = Trim(v);
			if (!known) err = "No such camera.";
			else { put("cam_label_" + std::to_string(n), v); Log("-- camera %d is called %s --\n", n, CamName(n)); }
		} else if (d == "order") {
			std::vector<int> l;
			for (int x : IntList(q["list"])) if (x >= 1 && x <= CamLast() && std::find(l.begin(), l.end(), x) == l.end()) l.push_back(x);
			put("cam_order", join(l));
		} else if (d == "hide") {
			std::vector<int> l = IntList(Raw("cam_hidden"));
			l.erase(std::remove(l.begin(), l.end(), n), l.end());
			if (!known) err = "No such camera.";
			else { if (q["on"] == "1") l.push_back(n); put("cam_hidden", join(l)); }
		} else err = "Unknown.";
		LeaveCriticalSection(&g_lock);
		if (!err.empty()) { Reply(s, 400, JSON, Err(err)); return; }
		Reply(s, 200, JSON, ok);
	} else if (path == "/api/tape/ctl") {              // the playback transport: do = play / pause / live / start / slow (n 1-16, rev) / fast (n) /
		                                              // step (d +-1) / seek (f 0-1) / lap (n) / crash (id) / jump (kind L|C|F, d +-1)
		std::string d = q["do"];
		int n = atoi(q["n"].c_str());
		if (d == "play" || d == "pause" || d == "live" || d == "start") TpQueue(d);
		else if (d == "slow" || d == "fast") TpQueue(d, n, q["rev"] == "1" ? -1 : 1);
		else if (d == "step") TpQueue(d, atoi(q["d"].c_str()) < 0 ? -1 : 1);
		else if (d == "seek") TpQueue(d, 0, atof(q["f"].c_str()));
		else if (d == "lap") TpQueue(d, n);
		else if (d == "crash") TpQueue(d, atoi(q["id"].c_str()));
		else if (d == "jump") TpQueue(d, atoi(q["d"].c_str()) < 0 ? -1 : 1, q["kind"].empty() ? 'C' : q["kind"][0]);
		else { Reply(s, 400, JSON, Err("Unknown playback command.")); return; }
		Reply(s, 200, JSON, ok);
	} else if (path == "/api/replay") {               // do=crash (id=n: that one; else the best lately, or back to live
		                                              // while one plays) / reel (the highlight reel) / live
		if (q["do"] == "live") g_reqLive = true;
		else if (q["do"] == "reel") g_reqReel = true;
		else if (!q["id"].empty()) g_reqReplayId = atoi(q["id"].c_str());
		else g_reqReplay = true;
		Reply(s, 200, JSON, ok);
	} else if (path == "/api/camedit") {             // the live camera editor: do = nudge (p, k) / move (f, s, u) / look
		                                              // (dt, dp) / save / reset / new / delete; the camera on screen
		std::string d = q["do"], err;
		EnterCriticalSection(&g_lock);
		const CamNow n = g_camNow;
		if (n.kind == ckNone) err = "This camera can't be adjusted.";
		else if (d == "nudge") CamEditNudge(atoi(q["p"].c_str()), atof(q["amt"].c_str()));   // (not "k": that is the pairing code)
		else if (d == "move") CamEditMove(atof(q["f"].c_str()), atof(q["s"].c_str()), atof(q["u"].c_str()));
		else if (d == "look") CamEditLook(atof(q["dt"].c_str()), atof(q["dp"].c_str()));
		else if (d == "drone") CamEditDrone(atof(q["u"].c_str()), atof(q["dd"].c_str()),
		                                    q["top"].empty() ? -1 : atof(q["top"].c_str()), q["hover"].empty() ? -1 : atof(q["hover"].c_str()));
		else if (d == "save") {
			std::string key = TuneKey(n.id), v = CamEditValue();
			if (FieldByKey(key)) PostMessage(g_ui, WM_APP_REMOTE, 0, (LPARAM) new RemoteReq{ rSet, key, v });
			else SaveKey(key, v);
			S.raw[key] = v;
			g_liveTune.erase(n.id);
			Log("-- camera %d (%s) adjustment saved: %s --\n", n.id, CamName(n.id), v.c_str());
			if (n.kind == ckHeli && VCamById(n.id)) {   // and the drone's follow settings
				char b[3][32];
				snprintf(b[0], 32, "%.0f", S.droneDistance); snprintf(b[1], 32, "%.0f", S.droneTopMph); snprintf(b[2], 32, "%.0f", S.droneHoverM);
				const char *keys[3] = { "drone_distance", "drone_top_mph", "drone_hover_m" };
				for (int i = 0; i < 3; i++) { S.raw[keys[i]] = b[i]; PostMessage(g_ui, WM_APP_REMOTE, 0, (LPARAM) new RemoteReq{ rSet, keys[i], b[i] }); }
				g_droneLive = false;
				Log("-- drone follow saved: trail %s %%, top %s mph, holds within %s m --\n", b[0], b[1], b[2]);
			}
		} else if (d == "reset") {                  // back to the preset (RaceVision's own) or the game's own
			std::string key = TuneKey(n.id);
			g_liveTune.erase(n.id);
			if (n.kind == ckHeli && VCamById(n.id) && g_droneLive) {   // the drone's follow settings back to the saved ones
				auto num = [](const char *k, double def) { auto it = S.raw.find(k); return it != S.raw.end() && !it->second.empty() ? atof(it->second.c_str()) : def; };
				S.droneDistance = num("drone_distance", 100); S.droneTopMph = num("drone_top_mph", 175); S.droneHoverM = num("drone_hover_m", 25);
				g_droneLive = false;
			}
			if (FieldByKey(key)) PostMessage(g_ui, WM_APP_REMOTE, 0, (LPARAM) new RemoteReq{ rSet, key, PresetTune(n.id) });
			else SaveKey(key, PresetTune(n.id));
			S.raw[key] = PresetTune(n.id);
			Log("-- camera %d (%s) back to %s --\n", n.id, CamName(n.id), *PresetTune(n.id) ? "its first set-up" : "the game's own");
		} else if (d == "new") {                    // a new camera of RaceVision's own from this one, as it is now
			int id = 29;
			while (id <= CAM_MAX && VCamById(id)) id++;
			if (id > CAM_MAX) err = "No room for more cameras (40 is the last).";
			else {
				std::string v = CamEditValue(), name = "Custom " + std::to_string(id);
				g_vcams.push_back({ id, n.gameCam, name, false });
				std::sort(g_vcams.begin(), g_vcams.end(), [](const VCam &a, const VCam &b) { return a.id < b.id; });
				SaveKey("cam_new_" + std::to_string(id), std::to_string(n.gameCam) + ", " + name);
				SaveKey(TuneKey(id), v);
				g_liveTune.erase(n.id);
				g_reqCam = id;
				Log("-- new camera %d (%s, on the game's %d): %s --\n", id, name.c_str(), n.gameCam, v.c_str());
			}
		} else if (d == "delete") {
			if (n.id <= 28 || !VCamById(n.id)) err = "Only cameras made here can be deleted.";
			else {
				int id = n.id;
				g_vcams.erase(std::remove_if(g_vcams.begin(), g_vcams.end(), [id](const VCam &v) { return v.id == id; }), g_vcams.end());
				SaveKey("cam_new_" + std::to_string(id), ""); SaveKey(TuneKey(id), "");
				g_liveTune.erase(id); g_reqCam = GameCam(id) > 0 ? n.gameCam : 19;
				Log("-- camera %d deleted --\n", id);
			}
		} else err = "Unknown.";
		std::string j = err.empty() ? CamEditJson() : "";
		LeaveCriticalSection(&g_lock);
		if (!err.empty()) { Reply(s, 400, JSON, Err(err)); return; }
		Reply(s, 200, JSON, j);
	} else if (path == "/api/auto") {
		g_reqAuto = true;
		Reply(s, 200, JSON, ok);
	} else if (path == "/api/pause") {
		bool on = q["on"] == "1";
		EnterCriticalSection(&g_lock);
		if (g_paused != on) { g_paused = on; Log(on ? "-- paused (phone) --\n" : "-- directing (phone) --\n"); }
		LeaveCriticalSection(&g_lock);
		Reply(s, 200, JSON, ok);
	} else if (path == "/api/set") {
		std::string k = q["name"], v = Trim(Recode(q["value"], CP_UTF8, CP_ACP));
		Field *f = FieldByKey(k);
		if (!f || f->page == pPhone || f->page == pArt) { Reply(s, 400, JSON, Err("No such setting.")); return; }
		if (f->kind == fNum) {
			char *end; strtod(v.c_str(), &end);
			if (v.empty() || *end) { Reply(s, 400, JSON, Err("That's not a number.")); return; }
		}
		PostMessage(g_ui, WM_APP_REMOTE, 0, (LPARAM) new RemoteReq{ rSet, k, v });
		Reply(s, 200, JSON, ok);
	} else if (path == "/api/preset") {
		std::string name = PresetName(Recode(q["name"], CP_UTF8, CP_ACP));
		if (q["do"] == "save") {
			if (name.empty()) { Reply(s, 400, JSON, Err("Give the preset a name.")); return; }
			if (_stricmp(name.c_str(), BUILTIN_PRESET) == 0) { Reply(s, 400, JSON, Err("That name belongs to the built-in preset.")); return; }
			PostMessage(g_ui, WM_APP_REMOTE, 0, (LPARAM) new RemoteReq{ rSavePreset, name, "" });
		} else PostMessage(g_ui, WM_APP_REMOTE, 0, (LPARAM) new RemoteReq{ rLoadPreset, Recode(q["name"], CP_UTF8, CP_ACP), "" });
		Reply(s, 200, JSON, ok);
	} else Reply(s, 404, JSON, Err("not found"));
}

struct Conn { SOCKET s; std::string ip; };
static DWORD WINAPI ConnThread(LPVOID p) {
	Conn *c = (Conn *)p;
	Serve(c->s, c->ip);
	shutdown(c->s, SD_SEND);
	closesocket(c->s);
	delete c;
	return 0;
}

// ---- the PC's address on the home network: an adapter that is up and has a gateway
static std::string LanIp() {
	ULONG size = 16384;
	std::vector<BYTE> buf(size);
	ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG r = GetAdaptersAddresses(AF_INET, flags, NULL, (IP_ADAPTER_ADDRESSES *)buf.data(), &size);
	if (r == ERROR_BUFFER_OVERFLOW) { buf.resize(size); r = GetAdaptersAddresses(AF_INET, flags, NULL, (IP_ADAPTER_ADDRESSES *)buf.data(), &size); }
	if (r != NO_ERROR) return "";
	std::string best;
	int bestScore = -1;
	for (auto *a = (IP_ADAPTER_ADDRESSES *)buf.data(); a; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
			sockaddr_in *sin = (sockaddr_in *)u->Address.lpSockaddr;
			unsigned b0 = sin->sin_addr.S_un.S_un_b.s_b1, b1 = sin->sin_addr.S_un.S_un_b.s_b2;
			if (b0 == 127 || (b0 == 169 && b1 == 254)) continue;
			bool home = (b0 == 192 && b1 == 168) || b0 == 10 || (b0 == 172 && b1 >= 16 && b1 < 32);
			int score = (a->FirstGatewayAddress ? 4 : 0) + (home ? 2 : 0) +
			            (a->IfType == IF_TYPE_IEEE80211 || a->IfType == IF_TYPE_ETHERNET_CSMACD ? 1 : 0);
			char ip[32]; inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
			if (score > bestScore) { bestScore = score; best = ip; }
		}
	}
	return best;
}

// the PC's Tailscale address (100.64.0.0/10) while Tailscale is up: friends somewhere else use it
static std::string TailIp() {
	ULONG size = 16384;
	std::vector<BYTE> buf(size);
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG r = GetAdaptersAddresses(AF_INET, flags, NULL, (IP_ADAPTER_ADDRESSES *)buf.data(), &size);
	if (r == ERROR_BUFFER_OVERFLOW) { buf.resize(size); r = GetAdaptersAddresses(AF_INET, flags, NULL, (IP_ADAPTER_ADDRESSES *)buf.data(), &size); }
	if (r != NO_ERROR) return "";
	for (auto *a = (IP_ADAPTER_ADDRESSES *)buf.data(); a; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp) continue;
		for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
			sockaddr_in *sin = (sockaddr_in *)u->Address.lpSockaddr;
			unsigned b0 = sin->sin_addr.S_un.S_un_b.s_b1, b1 = sin->sin_addr.S_un.S_un_b.s_b2;
			if (b0 == 100 && b1 >= 64 && b1 < 128) {
				char ip[32]; inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
				return ip;
			}
		}
	}
	return "";
}

// let phones on the local network (and Tailscale, for friends elsewhere) in (RaceVision runs as administrator, so it can add the rule)
static bool RunHidden(std::string cmd) {
	STARTUPINFOA si = { sizeof si };
	PROCESS_INFORMATION pi;
	if (!CreateProcessA(NULL, &cmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return false;
	WaitForSingleObject(pi.hProcess, 15000);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
	return code == 0;
}
static void Firewall(int port) {
	RunHidden("netsh advfirewall firewall delete rule name=\"RaceVision phone\"");
	std::string add = "netsh advfirewall firewall add rule name=\"RaceVision phone\" dir=in action=allow protocol=TCP localport=" +
	                  std::to_string(port) + " profile=any remoteip=localsubnet";
	bool tail = RunHidden(add + ",100.64.0.0/10");      // + Tailscale's addresses (friends somewhere else)
	bool ok = tail || RunHidden(add);
	EnterCriticalSection(&g_lock);
	g_remoteFirewall = tail ? "Windows Firewall lets phones on this network and on Tailscale in (rule \"RaceVision phone\")."
	                 : ok ? "Windows Firewall lets phones on this network in (rule \"RaceVision phone\")."
	                      : "Windows Firewall rule could not be added (needs administrator): if the phone can't connect, allow RaceVision when Windows asks.";
	if (!ok) Log("Could not add the firewall rule for the phone remote (not running as administrator?).\n");
	LeaveCriticalSection(&g_lock);
}

static void ShowLink(const std::string &url, const std::string &note) {
	std::vector<std::vector<bool>> qr = url.empty() ? std::vector<std::vector<bool>>() : MakeQr(url);
	EnterCriticalSection(&g_lock);
	g_remoteUrl = url; g_remoteQr = qr; g_remoteNote = note;
	LeaveCriticalSection(&g_lock);
	if (g_ui) InvalidateRect(g_ui, NULL, FALSE);
}

static DWORD WINAPI RemoteThread(LPVOID) {
	WSADATA wd;
	WSAStartup(MAKEWORD(2, 2), &wd);
	SOCKET ls = INVALID_SOCKET;
	int port = 0, fwPort = 0;
	std::string ip, tailIp, shown = "?", shownTail;
	double lastTry = -100, lastIp = -100;
	while (!g_quit) {
		EnterCriticalSection(&g_lock);
		bool on = Raw("remote") != "0";
		int want = atoi(Raw("remote_port").c_str());
		std::string key = Raw("remote_key");
		LeaveCriticalSection(&g_lock);
		if (want < 1 || want > 65535) want = 8090;
		if (ls != INVALID_SOCKET && (!on || want != port)) {
			closesocket(ls); ls = INVALID_SOCKET;
			Log(on ? "-- phone remote moving to port %d --\n" : "-- phone remote off --\n", want);
			lastTry = -100;
		}
		if (!on) {
			if (shown != "off") { shown = "off"; ShowLink("", "Phone remote is off.\nSwitch it on to show the code."); }
			if (!shownTail.empty()) { shownTail = ""; EnterCriticalSection(&g_lock); g_remoteTail = ""; LeaveCriticalSection(&g_lock); }
			Sleep(300);
			continue;
		}
		if (ls == INVALID_SOCKET && Now() - lastTry > 5) {
			lastTry = Now();
			ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			BOOL excl = TRUE;
			setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
			sockaddr_in a = {};
			a.sin_family = AF_INET; a.sin_port = htons((u_short)want); a.sin_addr.s_addr = INADDR_ANY;
			if (bind(ls, (sockaddr *)&a, sizeof a) != 0 || listen(ls, 16) != 0) {
				closesocket(ls); ls = INVALID_SOCKET;
				std::string note = "Port " + std::to_string(want) + " is taken by another program.\nPick another port on the right.";
				if (shown != note) { shown = note; ShowLink("", note); Log("Phone remote: port %d is taken by another program.\n", want); }
			} else {
				port = want;
				Log("Phone remote ready on port %d.\n", port);
				if (fwPort != port) { fwPort = port; Firewall(port); }
				lastIp = -100;
			}
		}
		if (ls == INVALID_SOCKET) { Sleep(300); continue; }
		if (Now() - lastIp > 10) { lastIp = Now(); ip = LanIp(); tailIp = TailIp(); }   // Wi-Fi / Tailscale can change under us
		std::string tailUrl = tailIp.empty() ? "" : "http://" + tailIp + ":" + std::to_string(port) + "/?k=" + key;
		if (tailUrl != shownTail) {
			shownTail = tailUrl;
			EnterCriticalSection(&g_lock); g_remoteTail = tailUrl; LeaveCriticalSection(&g_lock);
			if (!tailUrl.empty()) Log("Phone link for friends on Tailscale: %s\n", tailUrl.c_str());
			if (g_ui) InvalidateRect(g_ui, NULL, FALSE);
		}
		std::string url = ip.empty() ? "" : "http://" + ip + ":" + std::to_string(port) + "/?k=" + key;
		std::string link = url.empty() ? "This PC isn't on a network." : url;
		if (shown != link) { shown = link; ShowLink(url, url.empty() ? link : ""); if (!url.empty()) Log("Phone link: %s\n", url.c_str()); }

		fd_set set; FD_ZERO(&set); FD_SET(ls, &set);
		timeval tv = { 0, 300000 };
		if (select(0, &set, NULL, NULL, &tv) > 0) {
			sockaddr_in from; int len = sizeof from;
			SOCKET c = accept(ls, (sockaddr *)&from, &len);
			if (c != INVALID_SOCKET) {
				char addr[32]; inet_ntop(AF_INET, &from.sin_addr, addr, sizeof addr);
				HANDLE t = CreateThread(NULL, 0, ConnThread, new Conn{ c, addr }, 0, NULL);
				if (t) CloseHandle(t); else closesocket(c);
			}
		}
	}
	if (ls != INVALID_SOCKET) closesocket(ls);
	WSACleanup();
	return 0;
}

static void StartRemote() {
	EnterCriticalSection(&g_lock);                      // settings from before the remote existed
	if (Raw("remote").empty()) SaveKey("remote", "1");
	if (Raw("remote_port").empty()) SaveKey("remote_port", "8090");
	if (Raw("remote_key").empty()) SaveKey("remote_key", MakeKey());
	if (Raw("remote_scanner").empty()) SaveKey("remote_scanner", "1");
	if (Raw("aimod_port").empty()) SaveKey("aimod_port", "8767");
	LeaveCriticalSection(&g_lock);
	for (auto &f : g_fields)                             // the window made its boxes before these were set
		if (f.page == pPhone && f.h) {
			if (f.kind == fNum) { g_syncing = true; SetWindowTextA(f.h, Raw(f.key).c_str()); g_syncing = false; }
			else InvalidateRect(f.h, NULL, FALSE);
		}
	HANDLE t = CreateThread(NULL, 0, RemoteThread, NULL, 0, NULL);
	if (t) CloseHandle(t);
}
