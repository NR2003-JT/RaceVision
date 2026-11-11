// ---------------------------------------------------------------- the track's TV cameras (and the shared track map)
// (2026-10-29) Once a session is loaded NR2003 keeps the track's TV camera sets in memory, verbatim from the .cam file:
//   TV1 0xC548A0, TV2 +3652, spectator +7304: a count, then up to 64 records of 57 bytes {sf (m past the line), cl (m
//   from the centre line, + = left of travel / infield), height, take-over (m from sf, < 0 = before the camera), lens
//   and aim floats ..., type byte (0 turning, 1 fixed)}. (A second copy 0x3D00 further on is the same.)
// A camera shows the car from its take-over point (sf + take-over) to the next camera's. Checked live at Bristol
// against the game's own camera index 0xC06B40 (TV2) and the camera position CAMOBJ+0x10 (TV1): 100 % of samples.
// The lap distance for this is the car's dlong (car block +6250), the one the camera file is measured in.
// Uses:
//  - cuts: no cut within tv_cut_gap seconds of a TV camera change on screen - the game's own change from one TV camera
//    to the next is a cut too; two cuts in a row look like a mistake. The director waits (tv_cut_wait at most).
//  - the camera map: a picture of the track with each TV camera and the stretch it covers (Cameras page button), from
//    the track map RaceControl records (%PROGRAMDATA%\NR2003 Companions\tracks\<track>\map.json).
static const DWORD TVCAM_TABLE = 0xC548A0, TVCAM_GROUP = 4 + 64 * 57, TRACK_LEN_ADDR = 0xDACF0C;
static const int OFF_DLONG = 6250;
static const char *TV_GROUP_NAME[3] = { "TV1", "TV2", "spectator" };
struct TvCam { float sf = 0, cl = 0, h = 0, take = 0, from = 0, zoom = 0; int type = 0, n = 0; };   // zoom: the lens (radians) as the car passes
static std::vector<TvCam> g_tv[3];               // per group, ordered by take-over point (n = its place in the file)
static double g_tvLen = 0;                       // the lap (m), the game's own track length
static std::string g_tvTrack;
static double g_tvWaitFrom = -1;                 // real time the director started waiting for a camera change
static volatile bool g_reqCamMap = false;

static double TvMod(double a, double b) { double r = fmod(a, b); return r < 0 ? r + b : r; }

static void TvLoad(Game &g) {
	double len = g.Get<double>(TRACK_LEN_ADDR, 0);
	if (!(len > 100 && len < 100000)) return;
	std::vector<TvCam> got[3];
	for (int k = 0; k < 3; k++) {
		DWORD base = TVCAM_TABLE + k * TVCAM_GROUP;
		int n = g.Get<int>(base, 0);
		if (n < 0 || n > 64) return;
		for (int i = 0; i < n; i++) {
			unsigned char rec[57];
			if (!g.Read(base + 4 + i * 57, rec, 57)) return;
			TvCam c; memcpy(&c.sf, rec, 4); memcpy(&c.cl, rec + 4, 4); memcpy(&c.h, rec + 8, 4); memcpy(&c.take, rec + 12, 4); memcpy(&c.zoom, rec + 24, 4);
			c.type = rec[56]; c.n = i + 1;
			if (!(c.sf == c.sf) || fabs(c.sf) > 100000 || !(c.take == c.take)) return;
			c.from = (float)TvMod(c.sf + c.take, len);
			got[k].push_back(c);
		}
		std::sort(got[k].begin(), got[k].end(), [](const TvCam &a, const TvCam &b) { return a.from < b.from; });
	}
	bool same = fabs(len - g_tvLen) < 0.5 && g_tvTrack == g_trackName;
	for (int k = 0; k < 3 && same; k++) {
		same = got[k].size() == g_tv[k].size();
		for (size_t i = 0; same && i < got[k].size(); i++) same = got[k][i].sf == g_tv[k][i].sf && got[k][i].take == g_tv[k][i].take && got[k][i].zoom == g_tv[k][i].zoom && got[k][i].cl == g_tv[k][i].cl;
	}
	if (same) return;
	for (int k = 0; k < 3; k++) g_tv[k] = got[k];
	g_tvLen = len; g_tvTrack = g_trackName;
	Log("TV cameras (%s, %.0f m lap): TV1 %d, TV2 %d, spectator %d\n", g_trackName.c_str(), len,
	    (int)g_tv[0].size(), (int)g_tv[1].size(), (int)g_tv[2].size());
}

// the TV group a camera number shows (19 TV1, 20 TV2, 21 spectator; RaceVision's own cameras by their base), -1 = none
static int TvGroupOf(int cam) {
	if (cam < 0) return -1;
	int b = GameCam(cam);
	return b >= 19 && b <= 21 && !g_tv[b - 19].empty() ? b - 19 : -1;
}

static float CarDlong(Game &g, int i) {
	float d = -1;
	if (!g.Read(BLOCK0 + i * BLOCK_SIZE + OFF_DLONG, &d, 4) || !(d == d) || d < 0 || d >= g_tvLen) return -1;
	return d;
}

// which camera of the group shows a car at lap distance d: its index in g_tv[grp]; since = metres since that camera
// took over, toNext = metres to the next camera's take-over (a group with one camera never changes: huge)
static int TvZone(int grp, double d, double &since, double &toNext) {
	const std::vector<TvCam> &v = g_tv[grp];
	int k = (int)v.size() - 1;
	for (int i = 0; i < (int)v.size(); i++) if (v[i].from <= d) k = i;
	since = TvMod(d - v[k].from, g_tvLen);
	toNext = v.size() < 2 ? 1e9 : TvMod(v[(k + 1) % v.size()].from - d, g_tvLen);
	return k;
}

// Is now a bad moment to change the picture to camera `cam` on `car`? Yes when the TV camera now on screen has just
// changed by itself (the game's cut), or when the new shot's TV camera is about to. why says which.
static bool TvTooClose(Game &g, int cam, int car, std::string &why) {
	if (S.tvCutGap <= 0 || g_tvLen <= 0) return false;
	char b[120];
	int now = g_lastCam, sc = g_shot.car;
	if (cam < 0) cam = now;                       // the new shot keeps the camera on screen
	int gNow = TvGroupOf(now);
	if (gNow >= 0 && sc >= 0 && g_car[sc].valid && g_car[sc].speed > 5) {
		float d = CarDlong(g, sc);
		double since, toNext;
		if (d >= 0) {
			int k = TvZone(gNow, d, since, toNext);
			double s = since / g_car[sc].speed;
			if (s < S.tvCutGap) { snprintf(b, sizeof b, "%s camera %d took over %.1f s ago", TV_GROUP_NAME[gNow], g_tv[gNow][k].n, s); why = b; return true; }
		}
	}
	int gNew = TvGroupOf(cam);
	if (gNew >= 0 && car >= 0 && g_car[car].valid && g_car[car].speed > 5) {
		float d = CarDlong(g, car);
		double since, toNext;
		if (d >= 0) {
			int k = TvZone(gNew, d, since, toNext);
			double s = toNext / g_car[car].speed;
			if (s < S.tvCutGap) {
				snprintf(b, sizeof b, "%s camera %d takes over from camera %d in %.1f s", TV_GROUP_NAME[gNew],
				         g_tv[gNew][(k + 1) % g_tv[gNew].size()].n, g_tv[gNew][k].n, s);
				why = b; return true;
			}
		}
	}
	return false;
}

// The director asks before a cut / camera change: true = wait (not longer than tv_cut_wait in all).
static bool TvHold(Game &g, int cam, int car) {
	std::string why;
	double now = Now();
	if (!TvTooClose(g, cam, car, why)) {
		if (g_tvWaitFrom >= 0) {
			Log("           waited %.1f s for a TV camera change\n", now - g_tvWaitFrom);
			char b[64]; snprintf(b, sizeof b, "waited %.1f s for a TV camera change", now - g_tvWaitFrom); TlNote(b);
		}
		g_tvWaitFrom = -1;
		return false;
	}
	if (g_tvWaitFrom < 0) g_tvWaitFrom = now;
	if (now - g_tvWaitFrom < S.tvCutWait) return true;
	Log("           cut anyway after %.1f s (%s)\n", now - g_tvWaitFrom, why.c_str());
	{ char b[200]; snprintf(b, sizeof b, "cut anyway after %.1f s (%s)", now - g_tvWaitFrom, why.c_str()); TlNote(b); }
	g_tvWaitFrom = -1;
	return false;
}

// ---- the camera map picture ----
struct TrackMapPts { std::vector<float> x, y; double len = 0; };
static std::string g_mapFile;                   // --cammap's third argument: this map file instead

static bool LoadTrackMap(const std::string &track, TrackMapPts &m) {
	char pd[MAX_PATH];
	if (!GetEnvironmentVariableA("PROGRAMDATA", pd, MAX_PATH)) return false;
	std::string text = !g_mapFile.empty() ? ReadAll(g_mapFile) : ReadAll(std::string(pd) + "\\NR2003 Companions\\tracks\\" + track + "\\map.json");
	size_t at = text.find("\"length_m\"");
	size_t pts = text.find("\"points\"");
	if (at == std::string::npos || pts == std::string::npos) return false;
	m.len = atof(text.c_str() + text.find(':', at) + 1);
	const char *p = text.c_str() + pts;
	while ((p = strchr(p, '[')) != nullptr) {
		p++;
		char *e; double x = strtod(p, &e); if (e == p) continue;   // the outer list's bracket
		p = e;
		while (*p == ',' || *p == ' ') p++;
		double y = strtod(p, &e); if (e == p) break; p = e;
		m.x.push_back((float)x); m.y.push_back((float)y);
	}
	if (!(m.len > 100 && m.x.size() >= 100)) return false;
	// RaceControl's smoothing (TrackMap.Smooth: a quadratic through the points within 12 m either side), for maps saved
	// before it came in - on a short track the points are close together and their wobble showed
	int n = (int)m.x.size(), h = std::max(2, std::min(40, (int)floor(12.0 / (m.len / n) + 0.5)));
	double norm = (2.0 * h + 1) * (4.0 * h * h + 4 * h - 3);
	std::vector<float> x0 = m.x, y0 = m.y;
	for (int i = 0; i < n; i++) {
		double sx = 0, sy = 0;
		for (int j = -h; j <= h; j++) {
			double w = (3 * (3.0 * h * h + 3 * h - 1) - 15.0 * j * j) / norm;
			int k = ((i + j) % n + n) % n;
			sx += w * x0[k]; sy += w * y0[k];
		}
		m.x[i] = (float)sx; m.y[i] = (float)sy;
	}
	return true;
}

static void MapAt(const TrackMapPts &m, double d, double lat, float &x, float &y) {
	int n = (int)m.x.size();
	auto at = [&](double dd, double &ox, double &oy) {
		double f = TvMod(dd, m.len) / m.len * n;
		int i = (int)f % n, j = (i + 1) % n; double t = f - floor(f);
		ox = m.x[i] + (m.x[j] - m.x[i]) * t; oy = m.y[i] + (m.y[j] - m.y[i]) * t;
	};
	double cx, cy, ax, ay, bx, by;
	at(d, cx, cy); at(d - 3 * m.len / n, ax, ay); at(d + 3 * m.len / n, bx, by);   // the direction over a few points
	double tx = bx - ax, ty = by - ay, l = sqrt(tx * tx + ty * ty);
	if (l < 1e-6) { tx = 1; ty = 0; } else { tx /= l; ty /= l; }
	x = (float)(cx - ty * lat); y = (float)(cy + tx * lat);
}

// Draws the camera map to a PNG: the track, each group's cameras (dot + number) and the stretch each one covers, as a
// coloured band beside the track (TV1 outside, TV2 next in, spectator innermost), plus the two pit cameras.
static bool CamMapPng(Game &g, const std::string &out, std::string &err) {
	TrackMapPts m;
	if (g_tvLen <= 0) { err = "no session loaded in NR2003 (the TV cameras are read from the game)"; return false; }
	if (!LoadTrackMap(g_tvTrack, m)) { err = "no track map for " + g_tvTrack + " yet - RaceControl draws it from the cars in the first lap or so of a session"; return false; }
	double scaleL = g_tvLen / m.len;                  // camera distances are in the game's lap; the map's may differ a hair
	float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
	for (size_t i = 0; i < m.x.size(); i++) { minX = std::min(minX, m.x[i]); maxX = std::max(maxX, m.x[i]); minY = std::min(minY, m.y[i]); maxY = std::max(maxY, m.y[i]); }
	// the cameras can sit well off the track: take them into the frame
	std::vector<PointF> camPos[3];
	for (int k = 0; k < 3; k++)
		for (auto &c : g_tv[k]) {
			float x, y; MapAt(m, c.sf / scaleL, c.cl, x, y);
			camPos[k].push_back(PointF(x, y));
			minX = std::min(minX, x); maxX = std::max(maxX, x); minY = std::min(minY, y); maxY = std::max(maxY, y);
		}
	const int W = 1400, H = 1000, PAD = 70, LEG = 70;
	float sc = std::min((W - 2 * PAD) / std::max(1.f, maxX - minX), (H - 2 * PAD - LEG) / std::max(1.f, maxY - minY));
	float ox = PAD + ((W - 2 * PAD) - (maxX - minX) * sc) / 2 - minX * sc, oy = PAD + LEG + ((H - 2 * PAD - LEG) - (maxY - minY) * sc) / 2 + maxY * sc;
	auto P = [&](float x, float y) { return PointF(ox + x * sc, oy - y * sc); };

	Bitmap bmp(W, H, PixelFormat32bppARGB);
	Graphics gr(&bmp);
	gr.SetSmoothingMode(SmoothingModeAntiAlias);
	gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
	gr.Clear(C(255, 24, 26, 30));
	int n = (int)m.x.size();
	std::vector<PointF> track;
	for (int i = 0; i <= n; i++) track.push_back(P(m.x[i % n], m.y[i % n]));
	float tw = std::max(5.f, std::min(18.f, 15 * sc));
	Pen edge(C(255, 10, 10, 12), tw + 4), road(C(255, 110, 114, 122), tw);
	edge.SetLineJoin(LineJoinRound); road.SetLineJoin(LineJoinRound);
	gr.DrawLines(&edge, track.data(), (INT)track.size());
	gr.DrawLines(&road, track.data(), (INT)track.size());

	static const Color pal[6] = { C(255, 255, 140, 30), C(255, 60, 170, 255), C(255, 120, 220, 90), C(255, 240, 80, 160),
	                              C(255, 250, 220, 60), C(255, 170, 120, 255) };
	FontFamily fam(L"Segoe UI");
	Font fSmall(&fam, 11, FontStyleBold, UnitPixel), fBig(&fam, 18, FontStyleBold, UnitPixel), fMid(&fam, 13, FontStyleRegular, UnitPixel);
	SolidBrush white(C(255, 235, 238, 245)), grey(C(255, 150, 156, 168));
	for (int k = 0; k < 3; k++) {
		const std::vector<TvCam> &v = g_tv[k];
		float band = -(tw / 2 + 6 + k * 9) / sc;          // to the right of travel = outside on an oval
		for (size_t i = 0; i < v.size(); i++) {
			Color col = pal[i % 6];
			double a = v[i].from, b = v.size() < 2 ? a + g_tvLen : v[(i + 1) % v.size()].from;
			double span = TvMod(b - a, g_tvLen); if (span < 1) span = g_tvLen;
			std::vector<PointF> seg;
			for (double d = 0; d <= span; d += std::max(2.0, span / 200)) { float x, y; MapAt(m, (a + d) / scaleL, band, x, y); seg.push_back(P(x, y)); }
			Pen pen(col, 6); pen.SetStartCap(LineCapFlat); pen.SetEndCap(LineCapFlat);
			if (seg.size() > 1) gr.DrawLines(&pen, seg.data(), (INT)seg.size());
			// the camera: a dot where it stands, a thin line to where it takes over
			PointF c = P(camPos[k][i].X, camPos[k][i].Y);
			float fx, fy; MapAt(m, a / scaleL, band, fx, fy);
			Pen thin(Color(70, col.GetR(), col.GetG(), col.GetB()), 1);
			gr.DrawLine(&thin, c, P(fx, fy));
			SolidBrush cb(col);
			float r = 6;
			if (v[i].type == 1) gr.FillRectangle(&cb, c.X - r, c.Y - r, 2 * r, 2 * r);   // fixed
			else gr.FillEllipse(&cb, c.X - r, c.Y - r, 2 * r, 2 * r);                     // turning
			wchar_t lab[24]; swprintf(lab, 24, L"%s%d", k == 0 ? L"TV1-" : k == 1 ? L"TV2-" : L"S-", v[i].n);
			gr.DrawString(lab, -1, &fSmall, PointF(c.X + 8, c.Y - 8), &white);
		}
	}
	// start / finish and the pit cameras
	{
		float ax, ay, bx, by; MapAt(m, 0, tw / 2 / sc + 4 / sc, ax, ay); MapAt(m, 0, -(tw / 2 + 32) / sc, bx, by);
		Pen sf(C(255, 255, 255, 255), 3); gr.DrawLine(&sf, P(ax, ay), P(bx, by));
		gr.DrawString(L"S/F", -1, &fSmall, PointF(P(bx, by).X + 4, P(bx, by).Y - 6), &white);
		for (int p = 0; p < 2; p++) {
			float px, py;
			if (!g.Read(PIT1_BLOCK + p * 0x24, &px, 4) || !g.Read(PIT1_BLOCK + p * 0x24 + 4, &py, 4) || !(px == px)) continue;
			PointF c = P(px, py);
			SolidBrush pb(C(255, 255, 255, 255));
			gr.FillRectangle(&pb, c.X - 4, c.Y - 4, 8.f, 8.f);
			gr.DrawString(p == 0 ? L"pit 1 (17)" : L"pit 2 (18)", -1, &fSmall, PointF(c.X + 7, c.Y - 7), &grey);
		}
	}
	std::wstring title(g_tvTrack.begin(), g_tvTrack.end());
	title += L" - TV cameras";
	gr.DrawString(title.c_str(), -1, &fBig, PointF(20, 16), &white);
	gr.DrawString(L"Bands beside the track, outside in: TV1, TV2, spectator. Each colour = the stretch one camera shows "
	              L"(from where it takes over). Dot = turning camera, square = fixed.", -1, &fMid, PointF(20, 44), &grey);

	CLSID png = {}; UINT cnt = 0, size = 0;
	GetImageEncodersSize(&cnt, &size);
	std::vector<BYTE> buf(size);
	ImageCodecInfo *enc = (ImageCodecInfo *)buf.data();
	GetImageEncoders(cnt, size, enc);
	for (UINT k = 0; k < cnt; k++) if (wcscmp(enc[k].MimeType, L"image/png") == 0) png = enc[k].Clsid;
	std::wstring w(out.begin(), out.end());
	if (bmp.Save(w.c_str(), &png, NULL) != Ok) { err = "could not write " + out; return false; }
	return true;
}

static void CamMapRequest(Game &g) {
	if (!g_reqCamMap) return;
	g_reqCamMap = false;
	if (g.h) TvLoad(g);
	char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
	std::string out = std::string(tmp) + "racevision_cameras_" + (g_tvTrack.empty() ? "track" : g_tvTrack) + ".png", err;
	if (!CamMapPng(g, out, err)) { Log("Camera map: %s\n", err.c_str()); return; }
	Log("Camera map: %s\n", out.c_str());
	ShellExecuteA(NULL, "open", out.c_str(), NULL, NULL, SW_SHOWNORMAL);
}
