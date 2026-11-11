// ---------------------------------------------------------------- the follow rig (2026-11-03)
// How RaceVision's Dynamic Cameras follow a car smoothly. The first version re-aimed from the main loop (~15-30 ms, irregular) at the
// car's position as the game keeps it - and that position moves in PHYSICS TICKS (about 36 a second), so the camera stepped with the
// ticks and juddered against a 60-144 fps picture ("very jumpy", user 2026-11-03). The rig:
//  1. runs on its own thread on a 2 ms high resolution clock (timeBeginPeriod(1) + a high resolution waitable timer, absolute
//     schedule, a short spin for the end), writing the camera ~500 times a second;
//  2. reconstructs the target instead of reading it: the last 4 distinct positions ("steps") are kept, a clock is phase-locked to when
//     they arrive (period starts at 1/36 s, learnt: gain 0.002 for 40 steps, then 0.0005; phase gain 0.03; a resync past 0.25 s;
//     a jump larger than 1.5 m + 500 m/s over 2 periods is rejected, 3 in a row start over) and the target is PLAYED BACK one
//     step period late, interpolated between steps - a continuous path instead of stairs;
//  3. critically damps yaw / pitch (a SmoothDamp spring: 45 ms / 55 ms) and rate limits them (18 / 12 rad/s);
//  4. takes the car's position from the car record's object, [REC + 0x5AE] + 24 / 28 / 32 (x, y, z).
// On top: lead - the aim point runs ahead by the playback delay plus the smoothing lag, so the car stays in the middle of the
// frame through a fast pan instead of drifting to its edge; auto zoom - the lens widens as the car comes close and narrows as it
// goes, so it keeps about the same size (TV operators do it; the game's own TV cameras do it with zoom_in / zoom / zoom_out).
// RigCore is a pure function of time and target positions (so it can be measured offline: --selftest); RigThread wraps it.
struct RigStepTrack {
	float st[4][3]; int count = 0; long long index = 0; double base = 0, period = 1.0 / 36; bool init = false; int rejected = 0;
};
static void RigStepUpdate(RigStepTrack &t, double now, float x, float y, float z) {      // = UpdateLockOnStepClock
	if (!t.init) { t.st[0][0] = x; t.st[0][1] = y; t.st[0][2] = z; t.count = 1; t.index = 0; t.base = now; t.rejected = 0; t.init = true; return; }
	const float *last = t.st[t.count - 1];
	if (last[0] == x && last[1] == y && last[2] == z) return;                           // the same sample: not a new step
	float dx = x - last[0], dy = y - last[1], dz = z - last[2], dist = sqrtf(dx * dx + dy * dy + dz * dz);
	if (dist > 1.5f + 500.f * (float)(t.period * 2)) {                                  // a jump no car makes: a respawn, a camera change
		if (++t.rejected >= 3) { t.init = false; RigStepUpdate(t, now, x, y, z); }
		return;
	}
	t.rejected = 0;
	if (t.count == 4) { memmove(t.st[0], t.st[1], 3 * 3 * sizeof(float)); } else t.count++;
	t.st[t.count - 1][0] = x; t.st[t.count - 1][1] = y; t.st[t.count - 1][2] = z;
	t.index++;
	double err = now - (t.base + t.index * t.period);
	if (fabs(err) > 0.25) { t.base = now - t.index * t.period; return; }
	t.base += err * 0.03;                                                               // follow the phase
	double gain = t.index >= 40 ? 0.0005 : 0.002;
	t.period = std::max(0.002, std::min(0.05, t.period + err * gain));                 // learn the tick period
}
static void RigStepPlayback(const RigStepTrack &t, double now, float *p, float *v) {     // = LockOnPlaybackTarget, + the segment's velocity
	const float *last = t.st[t.count - 1];
	v[0] = v[1] = v[2] = 0;
	if (t.count < 2) { p[0] = last[0]; p[1] = last[1]; p[2] = last[2]; return; }
	long long oldest = t.index - (t.count - 1);
	double pos = (now - t.base) / t.period - 1.0;                                       // one step behind the newest
	pos = std::max((double)oldest, std::min((double)t.index, pos));
	long long k = std::min((long long)floor(pos), t.index - 1);
	float f = (float)(pos - (double)k);
	const float *a = t.st[k - oldest], *b = t.st[k - oldest + 1];
	for (int i = 0; i < 3; i++) { p[i] = a[i] + (b[i] - a[i]) * f; v[i] = (float)((b[i] - a[i]) / t.period); }
}
static float RigSmoothDamp(float cur, float target, float &vel, float smoothTime, float dt) {   // Unity's critically damped spring
	smoothTime = std::max(0.001f, smoothTime);
	float omega = 2.f / smoothTime, x = omega * dt, ex = 1.f / (1.f + x + 0.48f * x * x + 0.235f * x * x * x);
	float change = cur - target, temp = (vel + omega * change) * dt;
	vel = (vel - omega * temp) * ex;
	return target + (change + temp) * ex;
}
static float RigWrap(float a) { const float TP = 6.2831853f; a = fmodf(a, TP); if (a > 3.1415927f) a -= TP; else if (a < -3.1415927f) a += TP; return a; }
static float RigSmoothDampAngle(float cur, float target, float &vel, float smoothTime, float dt) { return cur + RigWrap(RigSmoothDamp(0.f, RigWrap(target - cur), vel, smoothTime, dt)); }
static float RigApproach(float cur, float target, float step) { float d = target - cur; return fabsf(d) > step ? cur + copysignf(step, d) : target; }
static float RigApproachAngle(float cur, float target, float step) { float d = RigWrap(target - cur); return fabsf(d) > step ? cur + copysignf(step, d) : target; }

struct RigParams {
	float cx = 0, cy = 0, cz = 0;               // where the camera stands
	float fov0 = 0.5f, width0 = 40;             // the lens it starts with (radians), and the view width (m) that lens gives at the start
	float fovMin = 0.1f, fovMax = 1.0f;         // the lens' range (radians)
	int mode = 0;                               // targets: 0 the first only, 1 the midpoint of the first two when they are close, 2 the middle of all
	float smooth = 1, lead = 1;                 // smoothing times x, lead x (0 = aim at where the car was)
	bool autoZoom = true;
	float aimHeight = 0.8f;                     // aim this far above the car's reference point
};
struct RigCore {
	RigStepTrack trk[6];
	bool init = false, zoomInit = false;
	float yaw = 0, pitch = 0, yawVel = 0, pitchVel = 0, fov = 0.5f, fovVel = 0, yawRate = 0, dist = 0;
	double lastNow = 0;
	void Reset() { *this = RigCore(); }
	// positions of the targets as the game gives them (stepped); returns false when there is nothing to aim at yet
	bool Step(double now, const float (*tp)[3], const bool *tok, int n, const RigParams &p, float &outYaw, float &outPitch, float &outFov) {
		float P[6][3], V[6][3]; int have = 0;
		for (int i = 0; i < n && i < 6; i++) {
			if (!tok[i]) continue;
			RigStepUpdate(trk[i], now, tp[i][0], tp[i][1], tp[i][2]);
			if (trk[i].init) { RigStepPlayback(trk[i], now, P[have], V[have]); have++; }
		}
		if (!have) return false;
		float ax = P[0][0], ay = P[0][1], az = P[0][2], vx = V[0][0], vy = V[0][1], vz = V[0][2];
		if (p.mode == 1 && have >= 2 && hypotf(P[1][0] - ax, P[1][1] - ay) < 28) { ax = (ax + P[1][0]) / 2; ay = (ay + P[1][1]) / 2; az = (az + P[1][2]) / 2; vx = (vx + V[1][0]) / 2; vy = (vy + V[1][1]) / 2; vz = (vz + V[1][2]) / 2; }
		else if (p.mode == 2 && have >= 2) {
			float sx = 0, sy = 0, sz = 0, wx = 0, wy = 0, wz = 0; int m = 0;
			for (int i = 0; i < have; i++) if (hypotf(P[i][0] - ax, P[i][1] - ay) < 70) { sx += P[i][0]; sy += P[i][1]; sz += P[i][2]; wx += V[i][0]; wy += V[i][1]; wz += V[i][2]; m++; }
			ax = sx / m; ay = sy / m; az = sz / m; vx = wx / m; vy = wy / m; vz = wz / m;
		}
		double period = trk[0].period;
		float dt = (float)(now - lastNow); lastNow = now;
		if (!(dt > 0) || dt > 0.25f) dt = 0.016f;
		float leadT = p.lead * (float)(period + 0.045 * p.smooth);                       // the playback delay and the smoothing lag
		ax += vx * leadT; ay += vy * leadT; az += vz * leadT + p.aimHeight;
		float dx = ax - p.cx, dy = ay - p.cy, dz = az - p.cz, hz = hypotf(dx, dy);
		if (hz < 0.001f) return false;
		float wantYaw = atan2f(dy, dx), wantPitch = -atan2f(dz, hz);
		if (!init) { yaw = wantYaw; pitch = wantPitch; yawVel = pitchVel = 0; init = true; }
		else {
			float ys = RigSmoothDampAngle(yaw, wantYaw, yawVel, 0.045f * p.smooth, dt), ps = RigSmoothDamp(pitch, wantPitch, pitchVel, 0.055f * p.smooth, dt);
			float oy = yaw;
			yaw = RigApproachAngle(yaw, ys, 18.f * dt); pitch = RigApproach(pitch, ps, 12.f * dt);
			yawRate += (fabsf(RigWrap(yaw - oy)) / dt - yawRate) * std::min(1.f, dt * 4);   // (a short average, for the shot's score)
		}
		dist = sqrtf(dx * dx + dy * dy + dz * dz);
		float wantFov = p.fov0;
		if (p.autoZoom) wantFov = std::max(p.fovMin, std::min(p.fovMax, 2.f * atanf(p.width0 / 2.f / std::max(1.f, dist))));
		if (!zoomInit) { fov = wantFov; fovVel = 0; zoomInit = true; } else fov = RigSmoothDamp(fov, wantFov, fovVel, 0.25f * p.smooth, dt);
		outYaw = yaw; outPitch = std::max(-1.5f, std::min(1.5f, pitch)); outFov = fov;
		return true;
	}
};

// ---- the thread
struct RigCmd {
	bool on = false;
	DWORD block = 0;                            // the pit camera block being driven (PIT1_BLOCK / PIT2_BLOCK)
	int n = 0, slots[6] = {};
	int posMode = 0;                            // 1: the car object [record + 0x5AE] + 24, 0: the car block's x, y, z
	RigParams p;
	float yawFix = 0, pitchSign = 1;            // radians added to the turn / -1 flips the tilt (dyn_yaw_fix, dyn_pitch_sign)
};
static CRITICAL_SECTION g_rigCs;
static bool g_rigCsInit = false;
static RigCmd g_rigCmd;
static volatile LONG g_rigGen = 0, g_rigAck = 0, g_rigQuit = 0, g_rigAlive = 0;
static volatile LONG g_rigTicks = 0, g_rigMaxGapUs = 0, g_rigValid = 0;
static volatile float g_rigYaw = 0, g_rigPitch = 0, g_rigFov = 0, g_rigDist = 0, g_rigYawRate = 0;
static HANDLE g_rigThread = NULL, g_rigH = NULL;
static volatile DWORD g_rigOwnBlock = 0;                // what CamWork must leave alone

static bool RigOk(float v) { return v == v && fabsf(v) < 100000.f; }
static DWORD WINAPI RigThread(LPVOID) {
	typedef UINT (WINAPI *TBP)(UINT);
	HMODULE mm = LoadLibraryA("winmm.dll");
	TBP begin = mm ? (TBP)GetProcAddress(mm, "timeBeginPeriod") : nullptr, end = mm ? (TBP)GetProcAddress(mm, "timeEndPeriod") : nullptr;
	if (begin) begin(1);
	HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, 0x1F0003);
	LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
	auto nowS = [&]() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)fq.QuadPart; };
	auto waitUntil = [&](double t) {                       // a sleep on the timer, the last stretch spun
		double left = t - nowS();
		if (left > 0.0008 && timer) {
			LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((left - 0.0004) * 1e7);
			if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 50); else Sleep(1);
		}
		while (nowS() < t) { if (t - nowS() > 0.0002) SwitchToThread(); else YieldProcessor(); }
	};
	RigCore core; RigCmd c; LONG seen = -1; double next = nowS();
	float lastWritten[4] = { 1e9f, 1e9f, 1e9f, 1e9f };
	DWORD ptr[6] = {}; double ptrAt[6] = {};
	double lastTick = next;
	while (!g_rigQuit) {
		next += 0.002;
		LONG gen = g_rigGen;
		if (gen != seen) { EnterCriticalSection(&g_rigCs); c = g_rigCmd; LeaveCriticalSection(&g_rigCs); seen = gen; core.Reset(); lastWritten[0] = 1e9f; for (int i = 0; i < 6; i++) ptr[i] = 0; }
		double t = nowS();
		if (t - next > 0.05) next = t;                    // fell far behind (the machine stalled): don't try to catch up
		if (!c.on || !g_rigH) { waitUntil(next + 0.004); g_rigAck = gen; g_rigAlive = 1; next = nowS(); continue; }
		float tp[6][3]; bool tok[6] = {};
		for (int i = 0; i < c.n && i < 6; i++) {
			unsigned char b[12]; SIZE_T got = 0;
			if (c.posMode == 1) {
				if (!ptr[i] || t - ptrAt[i] > 0.5) {
					DWORD pv = 0;
					ptr[i] = ReadProcessMemory(g_rigH, (LPCVOID)(UINT_PTR)(REC0 + c.slots[i] * REC_SIZE + 0x5AE), &pv, 4, &got) && got == 4 && pv >= 0x400000 && pv < 0x7FFFFFFF ? pv : 0;
					ptrAt[i] = t;
				}
				if (!ptr[i] || !ReadProcessMemory(g_rigH, (LPCVOID)(UINT_PTR)(ptr[i] + 24), b, 12, &got) || got != 12) continue;
			} else if (!ReadProcessMemory(g_rigH, (LPCVOID)(UINT_PTR)(BLOCK0 + c.slots[i] * BLOCK_SIZE + 6622), b, 12, &got) || got != 12) continue;
			memcpy(tp[i], b, 12);
			tok[i] = RigOk(tp[i][0]) && RigOk(tp[i][1]) && RigOk(tp[i][2]);
		}
		float y, pt, fv;
		if (core.Step(t, tp, tok, c.n, c.p, y, pt, fv)) {
			float w[4] = { y + c.yawFix, pt * c.pitchSign, 0.f, fv };
			if (!(w[0] == lastWritten[0] && w[1] == lastWritten[1] && w[3] == lastWritten[3])) {
				float cur[4]; SIZE_T got = 0;                  // keep the roll the block has (floats 3..6: yaw, pitch, roll, lens)
				if (ReadProcessMemory(g_rigH, (LPCVOID)(UINT_PTR)(c.block + 12), cur, 16, &got) && got == 16) w[2] = cur[2];
				SIZE_T wr = 0; WriteProcessMemory(g_rigH, (LPVOID)(UINT_PTR)(c.block + 12), w, 16, &wr);
				memcpy(lastWritten, w, sizeof w);
			}
			g_rigYaw = y; g_rigPitch = pt; g_rigFov = fv; g_rigDist = core.dist; g_rigYawRate = core.yawRate; g_rigValid = 1;
		}
		double gap = t - lastTick; lastTick = t;
		LONG us = (LONG)(gap * 1e6); if (us > g_rigMaxGapUs) g_rigMaxGapUs = us;
		g_rigTicks++; g_rigAck = gen; g_rigAlive = 1;
		waitUntil(next);
	}
	if (timer) CloseHandle(timer);
	if (end) end(1);
	g_rigAlive = 0;
	return 0;
}
static void RigEnsureThread() {
	if (!g_rigCsInit) { InitializeCriticalSection(&g_rigCs); g_rigCsInit = true; }
	if (g_rigThread && WaitForSingleObject(g_rigThread, 0) != WAIT_TIMEOUT) { CloseHandle(g_rigThread); g_rigThread = NULL; }
	if (!g_rigThread) {
		g_rigQuit = 0; g_rigGen = g_rigAck = 0;
		g_rigThread = CreateThread(NULL, 0, RigThread, NULL, 0, NULL);
	}
}
static bool RigPost(Game &g, const RigCmd &c) {          // start / change what the rig drives; false = it can't (no thread / handle)
	if (!c.on && !g_rigThread) { g_rigOwnBlock = 0; return true; }   // nothing running, nothing to stop
	RigEnsureThread();
	if (!g_rigThread) return false;
	if (c.on) {
		if (!g_rigH) { if (!DuplicateHandle(GetCurrentProcess(), g.h, GetCurrentProcess(), &g_rigH, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false; }
		g_rigOwnBlock = c.block;
	} else g_rigOwnBlock = 0;
	g_rigValid = 0;
	EnterCriticalSection(&g_rigCs); g_rigCmd = c; LeaveCriticalSection(&g_rigCs);
	LONG gen = InterlockedIncrement(&g_rigGen);
	for (int i = 0; i < 40 && g_rigAck != gen; i++) Sleep(1);   // the rig has taken it (it writes nothing for a stopped shot after this)
	return true;
}
static void RigShutdown() {
	if (g_rigThread) {
		g_rigQuit = 1; WaitForSingleObject(g_rigThread, 500); CloseHandle(g_rigThread); g_rigThread = NULL;
	}
	if (g_rigH) { CloseHandle(g_rigH); g_rigH = NULL; }
	g_rigOwnBlock = 0;
}
static bool DynRigOwns(DWORD block) { return block && g_rigOwnBlock == block; }

// ---- self-test: a car on a straight at 60 m/s, positions in 1/36 s physics steps, a camera 25 m off the line; what the picture shows
// (sampled at 144 fps) with the old way - aim at the latest step from a loop that runs every 12-30 ms, an easing of 71 ms - against the rig.
// Measured: how far the camera is off the car (degrees) and how much the camera's own speed jerks (the change of angular speed
// between frames, degrees per second squared).
static void RigSelfTest(FILE *out) {
	int ok = 0, bad = 0;
	auto check = [&](bool c, const char *what) { if (c) ok++; else { bad++; fprintf(out, "FAIL  %s\n", what); } };
	std::mt19937 rng(7);
	std::uniform_real_distribution<double> jit(0.012, 0.030), tickj(-0.0015, 0.0015);
	const double TICK = 1.0 / 36, V = 60, LAT = 25, T0 = 0.0, T1 = 6.0;
	auto truth = [&](double t) { return -180.0 + V * t; };           // car x at time t (m), along x; the camera is at (0, -LAT)
	// the physics steps: a new position every TICK (+- 1.5 ms of when we notice)
	std::vector<std::pair<double, double>> steps;                     // (time the step is visible, x)
	for (double k = 0; k * TICK < T1 + 1; k++) steps.push_back({ k * TICK + tickj(rng), truth(k * TICK) });
	auto latest = [&](double t) { double x = steps[0].second; for (auto &s : steps) if (s.first <= t) x = s.second; else break; return x; };
	auto trueYaw = [&](double t) { return atan2(LAT, truth(t)); };       // direction to the car (camera at 0, -LAT)
	// old way
	std::vector<double> oldYaw;
	{
		double yaw = atan2(LAT, latest(0)), t = 0, nextLoop = 0;
		for (double f = T0; f < T1; f += 1.0 / 144) {
			while (nextLoop <= f) { double want = atan2(LAT, latest(nextLoop)); yaw += (want - yaw) * (1 - exp(-0.015 * 14)); nextLoop += jit(rng); }
			oldYaw.push_back(yaw); (void)t;
		}
	}
	// the rig, ticked every 2 ms
	std::vector<double> rigYaw; RigCore core; RigParams p; p.cx = 0; p.cy = -(float)LAT; p.cz = 5; p.autoZoom = false; p.aimHeight = 0;
	{
		double next = 0, cur = atan2(LAT, latest(0));
		for (double t = 0; t < T1; t += 0.002) {
			float tp[1][3] = { { (float)latest(t), 0.f, 5.f } }; bool tk[1] = { true }; float y, pt, fv;
			// (the camera is at y = -LAT looking to +y: yaw measured from +x is atan2(dy, dx) with dy = LAT)
			if (core.Step(t, tp, tk, 1, p, y, pt, fv)) cur = y;
			while (next <= t) { rigYaw.push_back(cur); next += 1.0 / 144; }
		}
	}
	auto metrics = [&](const std::vector<double> &y, double &maxErrDeg, double &jerkRms, double skipS) {
		maxErrDeg = 0; double sum = 0; int n = 0;
		for (size_t i = 2; i < y.size(); i++) {
			double t = i / 144.0; if (t < skipS) continue;
			double err = fabs(y[i] - trueYaw(t)) * 180 / CS_PI; maxErrDeg = std::max(maxErrDeg, err);
			double a = (y[i] - 2 * y[i - 1] + y[i - 2]) * 144 * 144 * 180 / CS_PI; sum += a * a; n++;     // degrees / s^2
		}
		jerkRms = sqrt(sum / std::max(1, n));
	};
	double eo, jo, er, jr;
	metrics(oldYaw, eo, jo, 1.0); metrics(rigYaw, er, jr, 1.0);
	fprintf(out, "follow rig vs the old aim (a car at 60 m/s, 25 m from the camera, 36 Hz physics):\n  old: off the car by up to %.2f deg, camera jerk (rms) %.0f deg/s^2\n  rig: off the car by up to %.2f deg, camera jerk (rms) %.0f deg/s^2\n", eo, jo, er, jr);
	check(jr < jo * 0.5, "the rig's camera is at least twice as smooth as the old aim");
	check(er < 2.5, "the rig keeps the car within 2.5 degrees through the pass (lead compensates the lag)");
	// the step clock learns the real tick period (here 1/30 s, not the 1/36 it starts with)
	{
		RigStepTrack t; double tk = 1.0 / 30, now = 0;
		for (int i = 0; i < 200; i++) { now = i * tk + tickj(rng) * 0.5; RigStepUpdate(t, now, (float)(i * 2), 0, 0); }
		fprintf(out, "  step clock learnt a period of %.4f s (real 0.0333)\n", t.period);
		check(fabs(t.period - tk) < 0.0015, "the step clock learns the physics tick period");
	}
	// a respawn (a jump) is rejected, then accepted once it repeats
	{
		RigStepTrack t; for (int i = 0; i < 20; i++) RigStepUpdate(t, i * 0.0278, (float)(i * 1.6), 0, 0);
		long long idx = t.index; RigStepUpdate(t, 20 * 0.0278, 5000, 0, 0);
		check(t.index == idx, "one impossible jump is ignored");
		RigStepUpdate(t, 21 * 0.0278, 5001, 0, 0); RigStepUpdate(t, 22 * 0.0278, 5002, 0, 0);
		RigStepUpdate(t, 23 * 0.0278, 5003, 0, 0);
		float p3[3], v3[3]; RigStepPlayback(t, 23 * 0.0278 + 0.1, p3, v3);
		check(p3[0] > 4000, "three in a row: the rig starts over at the new place");
	}
	// SmoothDamp reaches its target and does not overshoot a step
	{
		float v = 0, x = 0; float mx = 0;
		for (int i = 0; i < 500; i++) { x = RigSmoothDamp(x, 1.f, v, 0.045f, 0.002f); mx = std::max(mx, x); }
		check(fabsf(x - 1.f) < 1e-3f && mx < 1.001f, "SmoothDamp settles on the target without overshoot");
	}
	// auto zoom keeps the car about the same size
	{
		RigCore c; RigParams q; q.cx = 0; q.cy = -25; q.cz = 5; q.autoZoom = true; q.width0 = 40; q.fov0 = 0.16f; q.fovMin = 0.1f; q.fovMax = 0.9f; q.aimHeight = 0;
		float fv = 0, y, pt; double minW = 1e9, maxW = 0;
		for (double t = 0; t < 4; t += 0.002) {
			float tp[1][3] = { { (float)(-120 + 60 * t), 0.f, 5.f } }; bool tk[1] = { true };
			if (c.Step(t, tp, tk, 1, q, y, pt, fv) && t > 1) { double w = 2 * c.dist * tan(fv / 2); minW = std::min(minW, w); maxW = std::max(maxW, w); }
		}
		fprintf(out, "  auto zoom: the view width stays within %.0f - %.0f m while the car passes at 25 m (width 40 asked)\n", minW, maxW);
		check(maxW / minW < 2.6, "auto zoom keeps the car's apparent size within a factor of 2.6 (without it, 20+)");
	}
	fprintf(out, "follow rig self-test: %d ok, %d failed\n", ok, bad);
}
