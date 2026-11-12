// RaceVision highlights besides crashes (included by tv_director.cpp, after the replays). Few and far between (user,
// 2026-11-11):
//   multi      one car by multi_pass_cars cars or more in ONE corner or ONE straight (superspeedways: last lap only)
//   pass       a last-lap pass for a podium position
//   bump       a bump and run: the nose of the car behind on the tail of the one ahead (the game's own front / rear damage
//              zones), then by it within bump_run_window s, no crash
//   finish     the winner crossing the line, and the runner-up when it is close
// Passes, multi-passes and bump and runs: at most one every highlight_gap game s (the better one stays), the last lap
// excepted. They live in g_highs as Crash records (kind != HL_CRASH) so the replay machinery, the Replays list, the
// phone, the tape marks and the highlight reel take them as they take crashes. Nothing here goes on air by itself
// except the finish (replay_finish). Passes use the LIVE order on track between cars on the same lap (the user's
// rule: live order only for passes and battles); the position they are for is the official one.

static const double HL_PI = 3.14159265358979323846;
struct HlPair { signed char side = 0, cand = 0; double candAt = -1, seen = -1; };
static HlPair g_hlPair[NUM_SLOTS][NUM_SLOTS];      // [i][j], i < j: side +1 = i ahead of j on track, -1 behind, 0 not known
struct Overtake { double t, real; int by, of, pos, lap, seg; bool lastLap; };
struct HlGroup { std::vector<Overtake> passes; int seg = -1; double last = -1; };
static HlGroup g_hlGrp[NUM_SLOTS];                 // each car's passes in the corner / straight it is in
static int g_finWinner = -1, g_finSecond = -1, g_finId = -1, g_finPrevLaps = -1;
static double g_finAt = -1, g_finReal = -1;        // game s the winner crossed; real time the finish was settled

// ---- corners and straights: the direction of travel against lap distance, learnt from the cars
static const int HB = 240;                         // bins a lap
static double g_hbX[HB], g_hbY[HB];                // summed direction of travel (unit vectors)
static int g_hbN[HB], g_hbSeg[HB];                 // samples; segment number (corners and straights alternate)
static bool g_hbOk = false;
static double g_hbBuiltAt = -100;

static void HlReset() {
	for (auto &row : g_hlPair) for (auto &p : row) p = HlPair();
	for (auto &r : g_hlGrp) r = HlGroup();
	g_finWinner = g_finSecond = g_finId = g_finPrevLaps = -1;
	g_finAt = g_finReal = -1;
	for (int k = 0; k < HB; k++) { g_hbX[k] = g_hbY[k] = 0; g_hbN[k] = 0; g_hbSeg[k] = -1; }   // (the turns: learnt again, a few laps)
	g_hbOk = false; g_hbBuiltAt = -100;
}
static void HbClear() { HlReset(); }
static int HbBin(float dist) {
	double L = std::max(g_trackLen, 400.0f);
	int k = (int)floor(dist / L * HB);
	return ((k % HB) + HB) % HB;
}
// corner = turning faster than 1 radian per 700 m over about 60 m of track; runs shorter than 5 % of a lap join their
// neighbours (a kink isn't a corner, a short chute between two turns isn't a straight of its own)
static void HbBuild() {
	int have = 0;
	for (int k = 0; k < HB; k++) if (g_hbN[k] >= 3) have++;
	if (have < HB * 9 / 10) { g_hbOk = false; return; }
	double L = std::max(g_trackLen, 400.0f), bin = L / HB, ang[HB];
	double last = 0; bool any = false;
	for (int k = 0; k < HB; k++) {
		if (g_hbN[k] >= 3) { last = atan2(g_hbY[k], g_hbX[k]); any = true; }
		ang[k] = last;
	}
	if (!any) return;
	int w = std::max(1, (int)lround(30.0 / bin));
	std::vector<int> corner(HB);
	for (int k = 0; k < HB; k++) {
		double d = ang[(k + w) % HB] - ang[(k - w + HB) % HB];
		while (d > HL_PI) d -= 2 * HL_PI;
		while (d < -HL_PI) d += 2 * HL_PI;
		corner[k] = fabs(d) / (2 * w * bin) > 1.0 / 700 ? 1 : 0;
	}
	int minLen = std::max(2, HB / 20);
	for (int pass = 0; pass < 20; pass++) {          // short runs take their neighbours' kind
		int start = 0;
		while (start < HB && corner[start] == corner[(start + HB - 1) % HB]) start++;
		if (start >= HB) break;                    // one kind all round
		bool changed = false;
		for (int k = 0, n = 0; k < HB; k += n) {
			int a = (start + k) % HB; n = 1;
			while (k + n < HB && corner[(start + k + n) % HB] == corner[a]) n++;
			if (n < minLen) { for (int m = 0; m < n; m++) corner[(start + k + m) % HB] ^= 1; changed = true; break; }
		}
		if (!changed) break;
	}
	int start = 0;
	while (start < HB && corner[start] == corner[(start + HB - 1) % HB]) start++;
	int seg = 0;
	for (int k = 0; k < HB; k++) {
		int a = (start + k) % HB;
		if (k > 0 && corner[a] != corner[(a + HB - 1) % HB]) seg++;
		g_hbSeg[a] = start >= HB ? 0 : seg;
	}
	g_hbOk = true;
}
static void HbSample() {
	for (int i : g_order) {
		const Car &c = g_car[i];
		if (c.pit || c.speed < 20) continue;
		int k = HbBin(c.dist);
		if (g_hbN[k] >= 400) continue;
		g_hbX[k] += cos(c.head); g_hbY[k] += sin(c.head); g_hbN[k]++;
	}
	if (Now() - g_hbBuiltAt > 10) { g_hbBuiltAt = Now(); HbBuild(); }
}
// the corner / straight at lap distance d (no map yet: eighths of the lap)
static int SegAt(double d) {
	double L = std::max(g_trackLen, 400.0f);
	if (g_hbOk) return g_hbSeg[HbBin((float)d)];
	int k = (int)floor(fmod(fmod(d, L) + L, L) / (L / 8));
	return 1000 + k;
}

static bool Superspeedway() {
	std::string t = CoLower(g_trackName), l = CoLower(S.superspeedways) + ";";
	size_t a = 0, b;
	while ((b = l.find(';', a)) != std::string::npos) {
		std::string tok = Trim(l.substr(a, b - a)); a = b + 1;
		if (!tok.empty() && t.find(tok) != std::string::npos) return true;
	}
	return false;
}
static bool LastLap(int car) { return g_lapsTotal > 0 && g_leaderLaps >= g_lapsTotal - 1 && g_car[car].laps == g_lapsTotal - 1; }
static std::string ForPos(const char *what, int pos) {
	if (pos == 1) return std::string(what) + " FOR THE LEAD";
	return pos > 0 ? std::string(what) + " FOR P" + std::to_string(pos) : std::string(what);
}
static double PosBonus(int pos) { return pos <= 0 ? 0 : pos == 1 ? 4 : pos <= 3 ? 3 : pos <= 5 ? 2 : pos <= 10 ? 1 : 0; }

// a new highlight. Passes, multi-passes and bump and runs share highlight_gap: within it the better one stays (one
// already replayed stays too); the last lap is free. Returns its id, or -1 when it didn't stay.
static int HlAdd(int kind, int car, int other, double at, double end, double score, const std::string &what, int lap, bool lastLap = false) {
	bool passKind = kind == HL_PASS || kind == HL_MULTI || kind == HL_BUMP;
	if (passKind && !lastLap && S.hlGap > 0)
		for (size_t k = 0; k < g_highs.size(); k++) {
			Crash &h = g_highs[k];
			if (!(h.kind == HL_PASS || h.kind == HL_MULTI || h.kind == HL_BUMP) || fabs(h.first - at) >= S.hlGap || h.lastLap) continue;
			if (h.replayed || h.score >= score) return -1;
			SYSTEMTIME st; GetLocalTime(&st);
			Log("[%02d:%02d:%02d] HIGHLIGHT #%d dropped: %s is better\n", st.wHour, st.wMinute, st.wSecond, h.id, what.c_str());
			g_highs.erase(g_highs.begin() + k);
			break;
		}
	Crash h;
	h.kind = kind; h.id = ++g_hlSeq; h.lap = lap; h.first = at; h.lastAt = end; h.realAt = h.worthyAt = Now();
	h.impactAt = at; h.impactCar = car; h.other = other; h.score = score; h.what = what; h.lastLap = lastLap;
	h.cars.push_back(car); h.firsts.push_back(at);
	if (other >= 0 && other != car) { h.cars.push_back(other); h.firsts.push_back(at); }
	CarVecs(h);
	if (g_highs.size() >= 60) g_highs.erase(g_highs.begin());
	g_highs.push_back(h);
	SYSTEMTIME st; GetLocalTime(&st);
	Log("[%02d:%02d:%02d] HIGHLIGHT #%d %s, lap %d: %s - %s, worth %.1f\n", st.wHour, st.wMinute, st.wSecond, h.id, HlKey(kind),
	    lap, what.c_str(), CrashWho(h, 3).c_str(), h.score);
	return h.id;
}

static bool InCrash(int car, double t) {
	for (auto &cr : g_crashes)
		if (cr.first < t + 12 && cr.lastAt > t - 12 && std::find(cr.cars.begin(), cr.cars.end(), car) != cr.cars.end()) return true;
	return false;
}
// still a real pass: the car passed didn't pit, crash or stop right after (that's a car in trouble or slowing for pit
// road, not a pass)
static bool PassHolds(const Overtake &o) {
	const Car &v = g_car[o.of];
	if (!v.valid || v.out || v.pit || v.pitPhase != 0 || v.pitExitAt > o.real) return false;
	if (v.incidentAt >= 0 && v.incidentAt > o.real - 10) return false;
	return !InCrash(o.of, o.t) && !InCrash(o.by, o.t);
}
// the hit that makes it a bump and run: the nose of the car that passed on the tail of the one it passed (either car's
// front / rear damage zone), within bump_run_window s before the pass, too small to be a crash (or part of one)
static const Contact *BumpBefore(const Overtake &o) {
	const Contact *best = nullptr;
	for (auto &ct : g_contacts) {
		if (ct.used || ct.gain >= S.crashDamage || ct.end < o.t - S.hlBumpWindow || ct.start > o.t + 0.5) continue;
		bool nose = ct.car == o.by && ct.nearCar == o.of && ct.front > 0.0005f;
		bool tail = ct.car == o.of && ct.nearCar == o.by && ct.rear > 0.0005f;
		if ((nose || tail) && (!best || ct.peak > best->peak)) best = &ct;
	}
	return best;
}

// a car is out of the corner / straight where it made its passes: a multi-pass, else a bump and run or a last-lap
// podium pass, else nothing
static void GroupClose(int car) {
	HlGroup gr = g_hlGrp[car];
	g_hlGrp[car] = HlGroup();
	std::vector<Overtake> ok;
	for (auto &o : gr.passes) if (PassHolds(o)) ok.push_back(o);
	if (ok.empty()) return;
	std::vector<int> victims;                       // still behind it now
	for (auto &o : ok)
		if (std::find(victims.begin(), victims.end(), o.of) == victims.end() && g_car[o.of].valid && SameLap(car, o.of) && Ahead(car, o.of) > 0)
			victims.push_back(o.of);
	bool last = false;
	for (auto &o : ok) last |= o.lastLap;
	if ((int)victims.size() >= S.hlMultiCars && (!Superspeedway() || last)) {
		int best = 0;
		for (auto &o : ok) if (o.pos > 0 && (!best || o.pos < best)) best = o.pos;
		char w[64];
		snprintf(w, sizeof w, "%d CARS IN ONE MOVE", (int)victims.size());
		HlAdd(HL_MULTI, car, -1, ok.front().t, ok.back().t, 2.0 * victims.size() + PosBonus(best), w, ok.front().lap, last);
		return;
	}
	for (auto &o : ok) {
		if (std::find(victims.begin(), victims.end(), o.of) == victims.end()) continue;   // it got back by
		const Contact *b = S.hlBump && o.pos > 0 && o.pos <= S.hlBumpDepth ? BumpBefore(o) : nullptr;
		if (b) HlAdd(HL_BUMP, o.by, o.of, std::min(o.t, b->peakAt >= 0 ? b->peakAt : b->start), o.t, 3 + PosBonus(o.pos), ForPos("BUMP AND RUN", o.pos), o.lap, o.lastLap);
		else if (o.lastLap && o.pos > 0 && o.pos <= 3) HlAdd(HL_PASS, o.by, o.of, o.t, o.t, 5 + PosBonus(o.pos), "LAST LAP: " + ForPos("PASS", o.pos), o.lap, true);
	}
}

// car `by` got ahead of car `of` on track at game second t (held 1.5 s since)
static void OvertakeSeen(int by, int of, double t) {
	int pa = g_car[by].spos, pb = g_car[of].spos;
	double where = g_car[by].dist - g_car[by].speed * (g_simT - t);   // where it got ahead
	Overtake o = { t, Now() - (g_simT - t), by, of, pa > 0 && pb > 0 ? std::min(pa, pb) : std::max(pa, pb), g_leaderLaps, SegAt(where), LastLap(by) };
	if (o.pos > 0 && o.pos <= 5) {
		SYSTEMTIME st; GetLocalTime(&st);
		Log("[%02d:%02d:%02d] PASS     %s gets by %s for P%d%s\n", st.wHour, st.wMinute, st.wSecond, Name(by).c_str(), Name(of).c_str(), o.pos,
		    o.lastLap ? " on the last lap" : "");
	}
	HlGroup &gr = g_hlGrp[by];
	if (!gr.passes.empty() && (o.seg != gr.seg || t - gr.passes.front().t > 15)) GroupClose(by);
	HlGroup &g2 = g_hlGrp[by];
	g2.passes.push_back(o); g2.seg = o.seg; g2.last = t;
}

// the finish: the winner's line crossing (seen as it happens, not when RaceVision joined a race already over), then the
// runner-up's if it comes within 10 s; the replay (FinishAuto) waits for both
static void FinishWatch() {
	if (g_lapsTotal <= 0 || g_score.empty()) { g_finPrevLaps = -1; return; }
	auto crossedAt = [](int i) {                    // just over the line: back by the distance driven since
		const Car &c = g_car[i];
		return g_simT - std::min(1.0, (double)c.dist / std::max(c.speed, 10.0f));
	};
	if (g_finWinner < 0) {
		if (g_finPrevLaps >= 0 && g_finPrevLaps < g_lapsTotal && g_leaderLaps >= g_lapsTotal) {
			int w = g_score[0];
			if (g_car[w].laps < g_lapsTotal) { g_finPrevLaps = g_leaderLaps; return; }
			g_finWinner = w; g_finAt = crossedAt(w);
			int second = g_score.size() > 1 ? g_score[1] : -1;
			g_finId = HlAdd(HL_FINISH, w, second, g_finAt, g_finAt + 3, 6, "THE WIN", g_lapsTotal);
		}
		g_finPrevLaps = g_leaderLaps;
		return;
	}
	if (g_finSecond != -1 || g_finReal >= 0) return;
	Crash *h = CrashById(g_finId);
	if (!h) { g_finReal = Now(); return; }
	int second = -1;
	for (int i : g_score) if (i != g_finWinner && g_car[i].laps >= g_lapsTotal) { second = i; break; }
	if (second < 0) {
		if (g_simT - g_finAt > 10) { g_finSecond = -2; g_finReal = Now(); h->what = "THE WIN"; }   // nobody close: just the winner
		return;
	}
	g_finSecond = second;
	double m = std::max(0.0, crossedAt(second) - g_finAt);
	char w[64];
	if (m < 1) snprintf(w, sizeof w, "WINS BY %.3f S", m); else snprintf(w, sizeof w, "WINS BY %.1f S", m);
	h->what = w;
	h->other = second;
	if (h->cars.size() > 1) h->cars[1] = second; else { h->cars.push_back(second); h->firsts.push_back(g_finAt); }
	CarVecs(*h);
	h->score = 6 + (m < 0.1 ? 4 : m < 0.5 ? 2.5 : m < 1.5 ? 1 : 0);
	if (m < 4) h->lastAt = std::max(h->lastAt, g_finAt + m + 1.5);   // close: the runner-up crossing is in the replay too
	g_finReal = Now();
	SYSTEMTIME st; GetLocalTime(&st);
	Log("[%02d:%02d:%02d] HIGHLIGHT #%d finish: %s - %s, %s second\n", st.wHour, st.wMinute, st.wSecond, h->id, w,
	    Name(g_finWinner).c_str(), Name(second).c_str());
}
// a photo finish (under half a second): the finish replay gets its slow-motion look at the line
static bool FinishClose(const Crash &c) { return c.kind == HL_FINISH && c.score >= 8.5; }

// each poll (Poll, race sessions): who got by whom on track, passes by corner / straight, the finish
static void HighlightWatch() {
	if (g_session != SESSION_RACE) return;
	HbSample();
	if (!S.highlights) return;
	bool green = g_flag == FLAG_GREEN;
	for (size_t x = 0; x < g_order.size(); x++)
		for (size_t y = x + 1; y < g_order.size(); y++) {
			int i = std::min(g_order[x], g_order[y]), j = std::max(g_order[x], g_order[y]);
			HlPair &p = g_hlPair[i][j];
			if (p.seen >= 0 && (g_simT - p.seen > 2 || g_simT < p.seen)) p = HlPair();   // not watched for a while: start over
			const Car &a = g_car[i], &b = g_car[j];
			bool ok = green && !a.pit && !b.pit && a.speed > 20 && b.speed > 20 && SameLap(i, j);
			double d = ok ? Ahead(i, j) : 0;
			if (!ok || fabs(d) > 80) { p = HlPair(); continue; }
			p.seen = g_simT;
			if (fabs(d) > 40) { p.cand = 0; continue; }
			signed char s = d > 1 ? 1 : d < -1 ? -1 : 0;   // (side by side: neither yet)
			if (s == 0 || s == p.side) { p.cand = 0; continue; }
			if (p.cand != s) { p.cand = s; p.candAt = g_simT; continue; }
			if (g_simT - p.candAt < 1.5) continue;
			signed char was = p.side;
			p.side = s; p.cand = 0;
			if (!was) continue;                    // the first look at these two: no pass, just where they are
			int by = s > 0 ? i : j, of = s > 0 ? j : i;
			if (g_car[of].speed < 0.85f * g_car[by].speed) continue;   // the one passed is slowing (trouble, pit road): not a pass
			OvertakeSeen(by, of, p.candAt);
		}
	for (int c = 0; c < NUM_SLOTS; c++) {           // out of that corner / straight (the last pass in it has had time to show)
		HlGroup &gr = g_hlGrp[c];
		if (gr.passes.empty()) continue;
		bool moved = !g_car[c].valid || SegAt(g_car[c].dist) != gr.seg;
		if ((moved && g_simT - gr.last > 2) || g_simT - gr.passes.front().t > 15 || g_simT < gr.last) GroupClose(c);
	}
	FinishWatch();
}

// the takes of a highlight: its car from the replay cameras, highlight_preroll before the moment (the finish: at least
// 8 s, the run to the line), through its end. Real speed (a photo finish gets its slow look in ReplayStart).
static std::vector<Take> HighlightTakes(Crash &c) {
	Take t;
	t.car = c.impactCar >= 0 ? c.impactCar : (c.cars.empty() ? -1 : c.cars[0]);
	t.cam = NextCam("replay");
	t.guide = c.impactAt >= 0 ? c.impactAt : c.first;
	t.pre = c.kind == HL_FINISH ? std::max(S.hlPreroll, 8.0) : S.hlPreroll;
	double after = std::max(3.0, c.lastAt - t.guide + 2.5);
	t.len = t.pre + std::min(after, S.replayMax);
	return { t };
}

// replay_finish: once the finish is settled (runner-up across, or nobody within 10 s), after replay_finish_after s
static void FinishAuto(Game &g) {
	if (!S.replayFinish || !S.highlights || g_finId < 0 || g_finReal < 0 || Now() - g_finReal < S.replayFinishAfter) return;
	Crash *c = CrashById(g_finId);
	if (!c || c->replayed || g_rp.phase || !g_reel.empty() || g_youDrive || g_session != SESSION_RACE) return;
	if (g_shot.kind == kManual && Now() - g_shot.since < S.manualHold) return;   // you picked a shot: it waits
	SYSTEMTIME st; GetLocalTime(&st);
	Log("[%02d:%02d:%02d] REPLAY   the finish: %s\n", st.wHour, st.wMinute, st.wSecond, c->what.c_str());
	ReplayStart(g, c->id, false);
}

// ---- self-test (--selftest): cars driven round a made-up 800 m oval (two 250 m straights, two 150 m turns), no game
static double OvalHead(double d) {
	d = fmod(fmod(d, 800.0) + 800.0, 800.0);
	if (d < 250) return 0;
	if (d < 400) return HL_PI * (d - 250) / 150;
	if (d < 650) return HL_PI;
	return HL_PI + HL_PI * (d - 650) / 150;
}
static void HlSelfTest(FILE *out) {
	int ok = 0, bad = 0;
	auto check = [&](bool c, const char *what) { if (c) ok++; else { bad++; fprintf(out, "FAIL  %s\n", what); } };
	Settings keep = S;
	std::string keepTrack = g_trackName;
	S.highlights = true; S.hlBump = true; S.hlMultiCars = 2; S.hlGap = 180; S.hlBumpWindow = 3; S.hlBumpDepth = 10;
	S.superspeedways = "daytona; talladega"; S.crashDamage = 0.5;
	const float L = 800;
	struct Sim { int slot; double at; float v; int spos; };
	auto setup = [&](int leaderLaps, const char *track) {
		ResetRace();
		g_session = SESSION_RACE; g_flag = FLAG_GREEN; g_trackLen = L; g_lapsTotal = 100; g_leaderLaps = leaderLaps; g_simT = 100;
		g_trackName = track;
		HbClear();
		for (int k = 0; k < HB; k++) { double h = OvalHead((k + 0.5) * L / HB); g_hbX[k] = 5 * cos(h); g_hbY[k] = 5 * sin(h); g_hbN[k] = 5; }
		HbBuild(); g_hbBuiltAt = Now() + 1e6;      // (the made-up map stays)
		for (int i = 0; i < NUM_SLOTS; i++) g_car[i] = Car();
	};
	auto run = [&](std::vector<Sim> cars, double secs, int leaderLaps, std::function<void(double, std::vector<Sim> &)> each, bool fresh = true, const char *track = "bristol") {
		if (fresh) setup(leaderLaps, track);
		for (auto &s : cars) {
			Car &c = g_car[s.slot];
			c.valid = true; c.number = s.slot + 1; c.last = "Car" + std::to_string(s.slot); c.spos = s.spos;
		}
		double t0 = g_simT;
		for (double t = 0; t < secs; t += 0.05) {
			g_simT = t0 + t;
			if (each) each(t, cars);
			g_order.clear();
			for (auto &s : cars) {
				s.at += s.v * 0.05;
				Car &c = g_car[s.slot];
				c.speed = s.v; c.progress = s.at; c.laps = (int)floor(s.at / L); c.dist = (float)(s.at - c.laps * L); c.head = (float)OvalHead(c.dist);
				g_order.push_back(s.slot);
			}
			std::sort(g_order.begin(), g_order.end(), [](int a, int b) { return g_car[a].progress > g_car[b].progress; });
			g_score = g_order;
			if (leaderLaps < 0) { int ll = 0; for (int i : g_order) ll = std::max(ll, g_car[i].laps); g_leaderLaps = ll; }
			HighlightWatch();
		}
	};
	auto count = [](int kind) { int n = 0; for (auto &h : g_highs) if (h.kind == kind) n++; return n; };
	auto at = [&](int lap, double d) { return lap * (double)L + d; };

	setup(40, "bristol");
	check(g_hbOk && g_hbSeg[HbBin(100)] != g_hbSeg[HbBin(300)] && g_hbSeg[HbBin(300)] == g_hbSeg[HbBin(390)] && g_hbSeg[HbBin(300)] != g_hbSeg[HbBin(500)],
	      "track map: straights and turns apart");

	// one car by two in the same turn (gets ahead at about 280 m and 347 m): a multi-pass
	run({ { 4, at(40, 150), 52, 9 }, { 5, at(40, 166.5), 45, 8 }, { 6, at(40, 175.5), 45, 7 } }, 25, 40, nullptr);
	check(count(HL_MULTI) == 1 && g_highs.size() == 1, "two cars in one turn: one multi-pass");
	check(!g_highs.empty() && g_highs[0].impactCar == 4 && g_highs[0].what.find("2 CARS") == 0, "multi-pass: car and count");

	// one on the straight (about 200 m), the next in the turn (about 300 m): nothing
	run({ { 4, at(40, 100), 52, 9 }, { 5, at(40, 112.5), 45, 8 }, { 6, at(40, 126), 45, 7 } }, 25, 40, nullptr);
	check(g_highs.empty(), "straight then turn: no multi-pass");

	// a single pass for the lead mid-race: nothing (only last-lap podium passes count alone)
	run({ { 2, at(40, 110), 50, 1 }, { 3, at(40, 100), 52, 2 } }, 40, 40, nullptr);
	check(g_highs.empty(), "single pass for the lead mid-race: none");

	// last lap, pass for P2: one
	run({ { 2, at(99, 110), 50, 2 }, { 3, at(99, 100), 52, 3 }, { 7, at(99, 500), 50, 1 } }, 20, 99, nullptr);
	check(count(HL_PASS) == 1, "last lap pass for P2: one");
	check(!g_highs.empty() && g_highs[0].what == "LAST LAP: PASS FOR P2", "last lap pass: says so");

	// last lap, pass for P5: none
	run({ { 2, at(99, 110), 50, 5 }, { 3, at(99, 100), 52, 6 }, { 7, at(99, 500), 50, 1 } }, 20, 99, nullptr);
	check(g_highs.empty(), "last lap pass for P5: none");

	// superspeedway: the same two-car move mid-race doesn't count, on the last lap it does
	run({ { 4, at(40, 150), 52, 9 }, { 5, at(40, 166.5), 45, 8 }, { 6, at(40, 175.5), 45, 7 } }, 25, 40, nullptr, true, "daytona");
	check(g_highs.empty(), "superspeedway mid-race: no multi-pass");
	run({ { 4, at(99, 150), 52, 9 }, { 5, at(99, 166.5), 45, 8 }, { 6, at(99, 175.5), 45, 7 }, { 7, at(99, 700), 50, 1 } }, 25, 99, nullptr, true, "daytona");
	check(count(HL_MULTI) == 1, "superspeedway last lap: multi-pass");

	// passing a car that is slowing (40 vs 52), and one that goes down pit road: nothing
	run({ { 4, at(40, 150), 52, 9 }, { 5, at(40, 166.5), 40, 8 }, { 6, at(40, 175.5), 40, 7 } }, 25, 40, nullptr);
	check(g_highs.empty(), "cars slowing: no pass");
	run({ { 4, at(40, 150), 52, 9 }, { 5, at(40, 166.5), 45, 8 }, { 6, at(40, 175.5), 45, 7 } }, 25, 40,
	    [](double t, std::vector<Sim> &) { if (t > 4) g_car[5].pit = true, g_car[5].pitPhase = 1; });
	check(g_highs.empty(), "a car pitting: no multi-pass");

	// a bump and run for P4: the nose of the car behind on the tail of the one ahead, by it 1 s later
	auto bumpRun = [&](double hitAt) {
		run({ { 8, at(40, 106), 50, 4 }, { 9, at(40, 100), 52, 5 } }, 25, 40, [hitAt](double t, std::vector<Sim> &) {
			static bool done = false;
			if (t < 0.01) done = false;
			if (!done && t > hitAt) {
				done = true;
				Contact ct = { 9, g_simT - 0.2, g_simT, g_simT - 0.1, 0.02f, 0.015f, 0, 8, false };
				ct.front = 0.02f;
				g_contacts.push_back(ct);
			}
		});
	};
	bumpRun(2.5);                                   // (it gets ahead at 3.5 s)
	check(count(HL_BUMP) == 1 && g_highs.size() == 1, "bump and run: one");
	check(!g_highs.empty() && g_highs[0].what == "BUMP AND RUN FOR P4", "bump and run: says for P4");
	bumpRun(0.0);                                   // the hit 3.5 s before: too long ago
	check(g_highs.empty(), "hit 3.5 s before the pass: no bump and run");

	// two multi-passes 60 s apart: one stays (the better: for higher up)
	run({ { 4, at(40, 150), 52, 19 }, { 5, at(40, 166.5), 45, 18 }, { 6, at(40, 175.5), 45, 17 } }, 25, 40, nullptr);
	run({ { 10, at(40, 150 + 400), 50, 1 } }, 35, 40, nullptr, false);
	run({ { 11, at(40, 150), 52, 4 }, { 12, at(40, 166.5), 45, 3 }, { 13, at(40, 175.5), 45, 2 } }, 25, 40, nullptr, false);
	check(count(HL_MULTI) == 1 && !g_highs.empty() && g_highs[0].impactCar == 11, "two in highlight_gap: the better one stays");

	// the finish: the winner, then the runner-up 0.3 s behind
	run({ { 10, at(99, 790), 50, 1 }, { 11, at(99, 775), 50, 2 } }, 25, -1, nullptr);
	check(count(HL_FINISH) == 1, "finish: one finish highlight");
	if (Crash *h = CrashById(g_finId)) {
		check(h->impactCar == 10 && h->other == 11, "finish: winner and runner-up");
		check(h->what == "WINS BY 0.300 S", "finish: the margin");
		check(FinishClose(*h) && g_finReal >= 0, "finish: a photo finish, settled");
		check(fabs(h->impactAt - (100 + 0.2)) < 0.1, "finish: the moment the winner crossed");
	} else check(false, "finish: highlight found");
	run({ { 10, at(100, 300), 50, 1 }, { 11, at(100, 285), 50, 2 } }, 10, -1, nullptr);
	check(count(HL_FINISH) == 0, "joined after the finish: none");

	ResetRace();
	HbClear();
	for (int i = 0; i < NUM_SLOTS; i++) g_car[i] = Car();
	g_order.clear(); g_score.clear();
	S = keep; g_trackName = keepTrack;
	fprintf(out, "highlights self-test: %d ok, %d failed\n", ok, bad);
}
