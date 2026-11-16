// RaceVision window (included by tv_director.cpp, one build unit)
//
// One window for everything: what the director is doing (the shot on screen, the field, the
// log) and every setting, grouped by page. A change is written to racevision.ini and used at
// once. The window runs on its own thread, so dragging it or a dialog never stalls the
// director. It touches race data and settings only under g_lock, and asks the main thread for
// anything done to the overlay windows (they belong to that thread): g_reqEdit, g_reqFocus. Layout numbers are 96-dpi pixels; Px() scales them for the screen.
// The phone remote (racevision_remote.cpp) asks the main thread for g_reqFocus, g_reqCam and
// g_reqAuto, and this window's thread for settings and presets (WM_APP_REMOTE).

static volatile bool g_paused = false, g_quit = false, g_reqEdit = false, g_reqAuto = false;
static volatile bool g_reqReplay = false, g_reqLive = false;   // replay the last crash / back to live
static volatile int g_reqFocus = -1, g_reqCam = -1;
static const UINT WM_APP_REMOTE = WM_APP + 2;
enum RemoteWhat { rSet, rLoadPreset, rSavePreset };
struct RemoteReq { RemoteWhat what; std::string a, b; };
static void NewRemoteKey();
static Bitmap *LogoAt(int size);
// the phone link, kept up to date by the remote's thread (under g_lock)
static std::string g_remoteUrl, g_remoteNote, g_remoteFirewall, g_remoteTail;   // tail: the link for friends on Tailscale
static double g_tailCopiedAt = -100;
static std::vector<std::vector<bool>> g_remoteQr;
static std::map<std::string, double> g_phones;           // phone address -> last request (Now())

static int g_dpi = 96;
static int Px(double v) { return (int)lround(v * g_dpi / 96.0); }

static const COLORREF U_BG = RGB(17, 19, 24), U_SIDE = RGB(11, 12, 16), U_CARD = RGB(26, 29, 36),
	U_INPUT = RGB(12, 14, 18), U_LINE = RGB(50, 55, 66), U_TEXT = RGB(236, 239, 244), U_MUTED = RGB(144, 153, 170),
	U_GOLD = RGB(255, 196, 0), U_GREEN = RGB(70, 200, 120), U_RED = RGB(235, 85, 75), U_TEAL = RGB(60, 200, 200);
static const int UW = 1100, UH = 780, UH_MIN = 640, UHEAD = 64, USIDE = 200, UPAD = 28, UCX = USIDE + UPAD, UCW = UW - USIDE - 2 * UPAD,
	UCY = 150, UCARD_H = 68;
// The window can be made wider than UW. g_uw = its width now and g_cw = the width of the content now (96-dpi). The
// settings pages (Director, Battles, Cameras, Replays, Overlays) are laid out from g_cw; Live, Art, Presets and Phone
// keep their own fixed layout (UCW wide).
static int g_uw = UW, g_cw = UCW;

enum Page { pLive, pDirector, pBattles, pCameras, pReplays, pOverlays, pArt, pPresets, pPhone, PAGES };
static const char *PageName[] = { "Live", "Director", "Battles", "Cameras", "Replays", "Overlays", "Art", "Presets", "Phone" };
// the side bar groups its pages: a heading before the page that starts a group
static const char *PageGroup[PAGES] = { "MONITOR", "DIRECTING", NULL, NULL, NULL, "BROADCAST", NULL, "SETUP", NULL };
static const char *PageSub[] = {
	"What the director is doing right now. Double-click a car to put it on TV.",
	"How long shots last, when the director cuts to something better, and what happens when you take over.",
	"What counts as a battle, and how starts and restarts are handled.",
	"Which camera each kind of shot uses, and how the special cameras behave.",
	"Crashes and highlights from this race, and how replays are found and shot.",
	"Broadcast graphics drawn over the game (NR2003 in windowed or borderless mode).",
	"Number cards, headshots and driver colours, from series art folders.",
	"Save your settings as a preset of your own, and switch between presets in one click.",
	"Run RaceVision from your phone: pick cars and cameras, and change settings, in its browser.",
};
static int g_page = pLive;
// Cameras and Replays are long, so they have tabs
static int TabCount(int p) { return p == pCameras ? 3 : p == pReplays ? 5 : 0; }
static const char *TabName(int p, int t) {
	static const char *cam[] = { "Shots", "Rear and drone", "Dynamic cameras" };
	static const char *rep[] = { "This race", "Automatic replays", "Finding crashes", "Highlights", "Look and slow motion" };
	return p == pCameras ? cam[t] : rep[t];
}
static int g_tab[PAGES] = {};
// Pages taller than the window scroll (mouse wheel, or drag the bar on the right). The window can also be made
// taller or shorter (it opens to fit the screen). g_oy = the scroll of the page being painted: the page drawing
// helpers take it off every y, so page code keeps its plain layout numbers.
static int g_uh = UH;                       // window height now (96-dpi)
static int g_scroll[PAGES] = {}, g_oy = 0;
// how far each page's own drawing goes down (from UCY). The settings pages' numbers are worked out by FlowLayout();
// the others are fixed.
static int g_pageEnd[PAGES] = { 604, 0, 0, 0, 0, 0, 558, 552, 640 };

static HFONT fTitle, fH1, fNav, fLabel, fBody, fSmall, fSection, fBig, fMono, fInput;
static HBRUSH g_inputBrush, g_bgBrush, g_cardBrush;

// ---- settings on the pages
// A setting is a row on its page: the label and one sentence of help on the left, the control on the right. Rows are
// not placed by hand: AddFields() lists them in order (with section titles) and Layout() puts them in two columns
// from the width of the window.
enum FieldKind { fNum, fToggle, fText, fColor, fChoice, fAction, fCams };
struct Field {
	int page = 0, tab = -1;      // tab: which tab of the page (-1 = all)
	std::string key, label, help; FieldKind kind = fNum;
	std::string unit;            // shown inside the box: s, m, %, ...
	int x = 0, y = 0, w = 0, ht = UCARD_H;   // card (cell for camera chips), from the content's top left; set by Layout()
	int cw = 0;                  // width of the control; set by Layout()
	int want = 0;                // control width asked for (0 = the kind's own)
	std::string choices;         // fChoice "value|Label;..."; fAction the button text
	HWND h = NULL;
	int ctl = -1;                // its place in g_ctls
	bool bad = false;            // not a number
	bool pick = false, single = false;   // camera chips (always for fCams): single = exactly one camera
	bool full = false;           // a card across the whole width
	bool hdr = false, bar = false, cell = false, fixed = false;   // hdr: button beside the page title; bar: button in a bar; cell: in the camera table; fixed: placed by hand
};
static std::vector<Field> g_fields;
// the page, in order: section titles, setting rows, and a few blocks that are drawn by hand
enum ItemType { iSection, iField, iCamTable, iBar, iPreview, iReplayList, iReplayLog };
struct Item { int page = 0, tab = 0, type = iField, field = -1; std::string text, sub; std::vector<int> fields; int y = 0, h = 0; };
static std::vector<Item> g_items;
static const int RL_ROW = 34, RL_ROWS = 10;       // the crash list on the Replays page
static int g_rlY = 0, g_rlogY = 0;                // where the crash list and the log start (from UCY)
static int g_camX1 = 190, g_camX2 = 400;          // the camera table's two columns (from UCX)
static std::vector<int> g_camRowY, g_camRowH;     // its rows (from UCY)

struct CamRow { const char *label, *key; bool follow; };
static const CamRow CAM_ROWS[] = {
	{ "Crash", "incident", true }, { "Pass: the car passing", "pass", true }, { "Battle", "battle", true }, { "Leader", "leader", true },
	{ "Running order tour", "tour", true }, { "Restart", "restart", true }, { "Pit road in", "pit", false },
	{ "In the pit stall", "pit_stall", false }, { "Pit road out", "pit_exit", false },
	{ "Qualifying: rolling off", "qual_exit", false }, { "Qualifying run", "qual", true }, { "Crash replay", "replay", false }, { "Pass: the car passed", "pass_passed", false },
};
static const char *CamName(int c) {
	auto own = S.raw.find("cam_label_" + std::to_string(c));        // a name given on the phone's Cameras tab
	if (own != S.raw.end() && !own->second.empty()) return own->second.c_str();
	static const char *n[] = { "", "In-car 1", "In-car 2", "In-car 3", "Chase", "Far chase", "Chase 2", "Far chase 2",
		"Rear chase", "(unused)", "Front", "Rear", "Roof forward", "Roof", "Right side", "Left side", "Helicopter",
		"Pit lane 1", "Pit lane 2", "TV1", "TV2", "Spectator" };
	if (c > 21) {                                                      // RaceVision's own cameras carry its name
		const VCam *v = VCamById(c);
		static std::string ring[6]; static int at = 0;
		std::string &o = ring[at++ % 6];
		o = !v ? "?" : c >= 41 ? "Dynamic Camera - Static" : "Dynamic Camera - Flowing: " + v->name;
		return o.c_str();
	}
	return c >= 1 && c <= 21 ? n[c] : "?";
}

static void AddFields() {
	int pg = 0, tb = 0;
	auto At = [&](int p, int t = 0) { pg = p; tb = t; };
	auto Sec = [&](const char *t, const char *sub = "") {
		Item i; i.page = pg; i.tab = tb; i.type = iSection; i.text = t; i.sub = sub; g_items.push_back(i);
	};
	auto Mk = [&](const char *key, const char *label, const char *help, FieldKind k) {
		Field f; f.page = pg; f.tab = tb; f.key = key; f.label = label; f.help = help; f.kind = k; return f;
	};
	auto Put = [&](const Field &f) {
		g_fields.push_back(f);
		Item i; i.page = pg; i.tab = tb; i.type = iField; i.field = (int)g_fields.size() - 1; g_items.push_back(i);
	};
	auto Num = [&](const char *key, const char *label, const char *help, const char *unit = "") {
		Field f = Mk(key, label, help, fNum); f.unit = unit; Put(f);
	};
	auto Tog = [&](const char *key, const char *label, const char *help) { Put(Mk(key, label, help, fToggle)); };
	auto Txt = [&](const char *key, const char *label, const char *help) { Put(Mk(key, label, help, fText)); };
	auto Col = [&](const char *key, const char *label, const char *help) { Put(Mk(key, label, help, fColor)); };
	auto Cho = [&](const char *key, const char *label, const char *help, const char *choices) {
		Field f = Mk(key, label, help, fChoice); f.choices = choices; Put(f);
	};
	auto Act = [&](const char *key, const char *label, const char *help, const char *button) {
		Field f = Mk(key, label, help, fAction); f.choices = button; Put(f);
	};
	auto Cams = [&](const char *key, const char *label, const char *help, bool single = false) {
		Field f = Mk(key, label, help, single ? fNum : fCams); f.pick = true; f.single = single; f.full = true; Put(f);
	};
	auto Hdr = [&](const char *key, const char *button) {          // a button beside the page title
		Field f = Mk(key, "", "", fAction); f.tab = -1; f.hdr = true; f.choices = button; g_fields.push_back(f);
	};
	auto Fix = [&](const char *key, const char *label, const char *help, FieldKind k, int x, int y, int w, const char *button = "") {
		Field f = Mk(key, label, help, k); f.fixed = true; f.x = x; f.y = y; f.w = w; f.choices = button; g_fields.push_back(f);
	};
	size_t bar = 0;
	auto Bar = [&](const char *label) { Item i; i.page = pg; i.tab = tb; i.type = iBar; i.text = label; g_items.push_back(i); bar = g_items.size() - 1; };
	auto BarBtn = [&](const char *key, const char *button) {
		Field f = Mk(key, "", "", fAction); f.bar = true; f.choices = button; g_fields.push_back(f);
		g_items[bar].fields.push_back((int)g_fields.size() - 1);
	};

	// ---- Director
	At(pDirector);
	Hdr("director_view", "Open director view in browser");
	Sec("SHOT LENGTH", "How long the picture stays on one thing.");
	Num("min_hold", "Shortest shot", "A shot always stays up at least this long, unless a crash happens.", "s");
	Num("max_hold", "When a shot goes stale", "After this long a shot's score is halved, so something fresher can take over.", "s");
	Num("onboard_hold", "On-board shot length", "In-car and roof shots stay up at least this long.", "s");
	Num("switch_margin", "Cut threshold", "A new shot must score this many times higher than the current one before the director cuts to it.", "\xD7");
	Num("revisit_after", "Revisit a car after", "A car that was just shown scores half for this long, so the whole field gets seen.", "s");
	Num("tour_depth", "Running order tour", "With nothing else going on, the tour visits the top this many cars.", "cars");
	Sec("WHEN YOU TAKE OVER", "You pick a car with the V key or on the Live page.");
	Num("manual_hold", "Director waits after your pick", "No automatic cuts for this long after you choose a car.", "s");
	Tog("live_after_climb_out", "Skip the game's replay when you climb out", "Getting out of your car starts NR2003's own replay. When this is on, RaceVision skips it so the broadcast carries on.");
	Sec("QUALIFYING", "Group and Indy qualifying, with many cars on track.");
	Num("qual_handoff_m", "Hand off before the stripe", "Switch to the next runner this far before the line. Race Control's timing box leads when it runs.", "m");
	Num("qual_close_ms", "Close to the pace", "A car on a timed lap this close to the time to beat (or faster) goes on screen as it nears the line. Crashes still come first.", "ms");
	Num("qual_finish_pct", "End of the lap", "How much of the lap, counted back from the line, counts as the end of the lap.", "%");
	Num("qual_break_hold", "Shot length in a break", "Between rounds the top cars are shown, one shot this long each.", "s");

	// ---- Battles
	At(pBattles);
	Sec("BATTLES", "When two cars count as fighting each other.");
	Num("battle_gap", "Battle gap", "Two cars this close together are a battle.", "s");
	Num("battle_min_age", "Battle must last", "A battle is worth a cut once the cars have been this close for this long.", "s");
	Num("short_track_m", "Short track below", "On tracks shorter than this, a battle means cars at the bumper or alongside, not a gap in seconds.", "m");
	Num("battle_short_m", "Battle distance, short tracks", "How close two cars must be to count as a battle on a short track.", "m");
	Sec("AFTER A START OR RESTART", "The field is bunched up, so almost everything looks like a battle.");
	Num("settle_laps", "Bunched-up period", "After the green, the field is treated as bunched up for this many laps.", "laps");
	Num("settle_positions", "Only the top positions count", "While the field is bunched up, only battles for this position or better count.");
	Num("settle_hold", "Shot length while bunched up", "Shots stay up at least this long while the field settles.", "s");
	Sec("LEAD BATTLES", "A fight for the front.");
	Num("lead_battle_depth", "Lead battle: top positions", "A battle for this position or better doesn't fade, and holds the camera. 0 turns lead battles off.");
	Num("lead_battle_hold", "Lead battle: stay up for", "Only a crash, a restart or something far bigger cuts away before this.", "s");
	Num("lead_battle_pack", "Pack size", "If this many cars run within 1.5 s of the leader (a superspeedway draft), lead battles are treated like any other battle.", "cars");

	// ---- Cameras
	At(pCameras, 0);
	Hdr("cam_map", "Show camera map");
	Sec("CAMERA PER SHOT", "A shot starts on one of the cameras in the first column, then switches to the second. Empty keeps the current camera.");
	{
		Item t; t.page = pCameras; t.tab = 0; t.type = iCamTable;
		for (auto &r : CAM_ROWS) {
			Field a = Mk((std::string("cam_") + r.key).c_str(), "", "", fCams); a.pick = true; a.cell = true; g_fields.push_back(a);
			t.fields.push_back((int)g_fields.size() - 1);
			if (r.follow) {
				Field b = Mk((std::string("cam_") + r.key + "_follow").c_str(), "", "", fCams); b.pick = true; b.cell = true; g_fields.push_back(b);
				t.fields.push_back((int)g_fields.size() - 1);
			}
		}
		g_items.push_back(t);
	}
	Sec("ON-BOARD AND SWITCHING");
	Cams("onboard_cams", "On-board cameras", "Cameras that count as on-board. They get the in-car dash.");
	Num("follow_after", "Switch after", "A shot leaves its starting camera for its second set after this long.", "s");
	Num("camera_every", "Then change every", "After that the camera changes this often. 0 = never.", "s");
	Tog("camera_call", "Use the game's camera switch", "Changes cameras the way the C key does, which keeps the camera sounds right.");
	At(pCameras, 1);
	Sec("REAR CAMERA (11)");
	Tog("rear_pan", "Follow the car behind", "The rear camera turns to the nearest car behind or alongside it, with a wide lens.");
	Num("rear_pan_span", "Turns up to", "Either way from straight back (90 = looking out the side).", "\xB0");
	Num("rear_pan_fov", "Lens width", "Wider is more fish-eye. The game's own is 78.", "\xB0");
	Tog("rear_mount", "Shoot from the car ahead", "Shows the TV car from the car just ahead of it. The overlays stay on the TV car.");
	Num("rear_mount_m", "Car ahead must be within", "If nobody is this close ahead of the TV car, the camera below is used instead.", "m");
	Cams("rear_alone", "Camera when nobody is ahead", "One camera: 11 is the TV car's own rear camera looking back, 4 is the chase camera.", true);
	Sec("DRONE (23)");
	Tog("drone_quiet", "Drone without sound", "No helicopter sound on the Drone. The Helicopter (16) keeps it.");
	Num("drone_distance", "Trail distance", "How far behind the car the Drone follows, as a percentage of the game's helicopter (50 = about half).", "%");
	Num("drone_top_mph", "Top speed", "The game's own is 175. Faster cars leave it behind until it jumps back beside them.", "mph");
	Num("drone_hover_m", "Holds still within", "Closer than this to the car, the Drone stops chasing. The game's own is 25.", "m");
	Sec("TV CAMERAS");
	Num("tv_cut_gap", "Gap around a TV camera change", "No cut this close to a TV camera's own change. 0 = off.", "s");
	Num("tv_cut_wait", "Wait for it at most", "If the TV camera change takes longer than this, the cut goes ahead anyway.", "s");
	At(pCameras, 2);
	Sec("DYNAMIC CAMERAS", "RaceVision's own static cameras, placed on the pit lane camera blocks.");
	Tog("dyn_cams", "Dynamic cameras", "RaceVision places its own static cameras (pit road, crashes, restarts, battles, the finish) and learns the track as it goes. Off = only the game's cameras.");
	Num("dyn_chance", "How often", "Of the shots that could have one, this many get a dynamic camera. The rest use the game's cameras.", "%");
	Sec("WHERE THEY ARE USED");
	Tog("dyn_pit", "Pit road", "Who dives in, the stall, and the race off pit road.");
	Tog("dyn_crash", "Crashes", "A new angle on the wreck each time.");
	Tog("dyn_restart", "Restarts", "Low and close at the green.");
	Tog("dyn_battle", "Battles and passes", "Trackside, ahead of the cars.");
	Tog("dyn_finish", "The finish", "At the line for the last lap.");
	Sec("TRACK LEARNING AND AIM");
	Tog("dyn_learn", "Learn the track", "Pit road, track width, where crashes happen and which angles worked, saved per track.");
	Num("dyn_half_width", "Track half width", "0 = learn it from where the cars drive.", "m");
	Num("dyn_yaw_fix", "Aim correction", "Added to the turn if a camera looks off to one side of the car.", "\xB0");
	Num("dyn_pitch_sign", "Tilt direction", "1 or -1. Use -1 if a camera looks up when it should look down.");
	Sec("MOTION");
	Tog("dyn_rig", "Smooth following", "A fast thread of its own follows the car, rebuilt between the game's physics ticks. Off = the old jumpy way.");
	Tog("dyn_autozoom", "Zoom with the car", "The lens widens as the car comes close and narrows as it goes, so the car keeps its size.");
	Num("dyn_smooth", "Pan smoothing", "1 = standard. Lower is quicker and tighter, higher is softer.");
	Sec("YOUR OWN CAMERA ANGLES");
	Tog("custom_cams", "Own angles on the spare cameras", "Chase 2 (6), Far chase 2 (7) and 22-40 use your own angles. Set them on the phone (Cameras > Adjust).");

	// ---- Replays
	At(pReplays, 0);
	Bar("REPLAY");
	BarBtn("replay_now", "Replay best crash"); BarBtn("replay_reel", "Highlight reel"); BarBtn("replay_live", "Back to live");
	Bar("TAPE");
	BarBtn("tp_start", "To the start"); BarBtn("tp_back", "Frame back"); BarBtn("tp_play", "Play / pause"); BarBtn("tp_fwd", "Frame forward");
	BarBtn("tp_slower", "Slower"); BarBtn("tp_faster", "Faster");
	Sec("CRASHES AND HIGHLIGHTS", "Best first. Click a row to play it.");
	{ Item i; i.page = pReplays; i.tab = 0; i.type = iReplayList; g_items.push_back(i); }
	Sec("REPLAY LOG");
	{ Item i; i.page = pReplays; i.tab = 0; i.type = iReplayLog; g_items.push_back(i); }
	At(pReplays, 1);
	Sec("AUTOMATIC REPLAYS", "Replays that start by themselves once you are out of your car.");
	Tog("replay_auto", "Replay crashes by themselves", "Rolls the replay of the crash most worth it once it is over.");
	Tog("replay_caution_only", "Only under caution", "Automatic replays wait for the yellow. The buttons on the This race tab work any time.");
	Num("replay_countdown", "Countdown before it rolls", "The crash alert counts down this long first.", "s");
	Num("replay_worthy", "Minimum crash score", "Score = the crash's damage, plus 0.5 for every car involved after the first.");
	Num("replay_after", "Wait after the crash", "The crash must have been over this long before its replay rolls.", "s");
	Num("replay_within", "Too old after", "A crash older than this (in game seconds) isn't replayed by itself.", "s");
	Sec("THE FINISH");
	Tog("replay_finish", "Replay the finish", "Once the winner and the runner-up are across the line, the finish plays by itself (not while you drive).");
	Num("replay_finish_after", "Wait after the runner-up", "How long after the runner-up crosses the line the replay starts.", "s");
	At(pReplays, 2);
	Sec("FINDING CRASHES", "What counts as a crash.");
	Num("crash_damage", "Crash from damage", "One contact adding this much damage is a crash. Rubs add 0.001 to 0.03.");
	Num("contact_join", "Joins a crash from damage", "A contact this big near a crash in progress puts that car in it.");
	Num("crash_drop", "Crash from speed loss", "When damage can't be read, losing this much speed quickly counts as a crash or spin (31 m/s = 70 mph).", "m/s");
	Num("crash_window", "Speed loss counted within", "The speed loss must happen within this much game time.", "s");
	Num("incident_hold", "Crash stays news", "A crash stays a candidate for the TV shot this long.", "s");
	Sec("HOW A REPLAY IS SHOT");
	Num("replay_preroll", "Start before the hit", "Each angle starts this long before its car's hardest hit.", "s");
	Num("replay_max", "Longest angle", "No angle runs longer than this.", "s");
	Num("replay_angles", "Angles for a multi-car crash", "One angle per car hit hardest. The next one starts once the car on screen has stopped.");
	Num("replay_take", "Each later angle at most", "The longest a later angle runs after its own hit (sooner if its car stops).", "s");
	Num("reel_angles", "Angles per crash in the reel", "How many angles each crash gets in the highlight reel.");
	Cams("cam_replay_angles", "Cameras for later angles", "Used after the Crash replay camera (Cameras page). They must show the car: TV, chase or helicopter.");
	At(pReplays, 3);
	Sec("OTHER HIGHLIGHTS", "Passes, bump and runs and the finish.");
	Tog("highlights", "Find other highlights", "Passes for the front, one car by several in a row, bump and runs and the finish go on the list, the phone and the reel.");
	Num("highlight_gap", "At most one pass every", "Of the passes, multi-passes and bump and runs inside this much game time, the better one stays. The last lap is free.", "s");
	Num("multi_pass_cars", "Multi-pass: cars", "One car by this many cars in one corner or one straight. Last-lap passes for the podium count too.", "cars");
	Txt("superspeedways", "Superspeedways", "Track folder names, or part of one, with ; between several. Multi-passes there count only on the last lap.");
	Num("highlight_preroll", "Start before the moment", "Highlight replays start this long before the pass or the bump.", "s");
	Sec("BUMP AND RUNS");
	Tog("highlight_bump", "Bump and runs", "The nose of the car behind hits the tail of the one ahead (no crash) and it gets by.");
	Num("bump_run_window", "Counts if it gets by within", "It must get by within this long after the hit.", "s");
	Num("bump_run_depth", "Only for position", "Only for this position or better.");
	Sec("HIGHLIGHT REEL");
	Tog("reel_highlights", "Highlights in the reel", "The highlight reel plays these with the crashes. Off = crashes only.");
	At(pReplays, 4);
	Sec("SLOW MOTION");
	Tog("replay_smart", "Smart slow motion", "A big crash gets one slow-motion look at its hardest hit (NR2003's own slow motion), then back to full speed. Passes never do.");
	Num("replay_slow_impact", "Needs a hit of at least", "Only a crash whose hardest single hit reaches this gets one slow-motion look, after the normal-speed angles.");
	Num("replay_slow_max", "Slowest speed", "1 to 16: 1 = 1/2 speed, 16 = 1/17. Smaller hits stay gentler.");
	Sec("TV WIPE");
	Tog("replay_transition", "Replay wipe", "A full-screen TV wipe into a replay, between its angles and back to live.");
	Num("replay_transition_ms", "Wipe length", "400 to 5000. The default is 1500. Higher is slower.", "ms");
	Col("replay_wipe_color", "Wipe colour", "The wipe's own colour. It is separate from the dash accent, so the dash boxes keep each driver's art colour.");
	Sec("OTHER PROGRAMS AND DRIVING");
	Tog("hide_companion_overlays", "Hide other programs' overlays in replays", "While a replay or its wipe is on, other programs' overlay windows (tickers, track maps...) stay out of sight until you are back to live. RaceVision's own overlays stay.");
	Txt("companion_apps", "Only these programs", "Blank = every other program. Otherwise part of an exe name or path, with ; between several.");
	Txt("companion_keep", "Never hide these", "Programs whose windows always stay (part of an exe name or path, with ; between several).");
	Tog("replay_drive", "Watch while driving: pause", "Ctrl+F7 or a replay button while in your car pauses the race, plays the replay, then resumes.");

	// ---- Overlays
	At(pOverlays);
	{ Item i; i.page = pOverlays; i.tab = 0; i.type = iPreview; g_items.push_back(i); }
	Sec("SHOW", "What is drawn, and in which look.");
	Tog("overlays", "Overlays", "Show the broadcast graphics. Ctrl+F9 switches them on and off.");
	Cho("overlay_style", "Style", "Dash: TV-style in-car dash. Classic: plain tag and gauges.", "dash|Dash;classic|Classic");
	Tog("replay_banner", "Replay banner", "REPLAY, the car, the angle and the time back to live, all through a replay (even with overlays off).");
	Tog("replay_overlays", "Overlays in replays", "The driver tag on the replay's car, and the dash when the angle is in-car.");
	Sec("WHEN THEY APPEAR", "The tag and battle box are kept sparing on purpose.");
	Num("overlay_min_run_laps", "Green laps into a run", "The driver tag and battle box wait this many green laps.", "laps");
	Num("overlay_gap", "Time between tags", "The tag or battle box shows at most this often. The dash shows on every in-car shot.", "s");
	Tog("tag_story", "Race story line", "Start vs now, restarts, stages, last stop and laps led, under the tag.");
	Num("tag_rotate", "Story changes every", "How long each race story fact stays up.", "s");
	Txt("stage_laps", "Stage end laps", "Laps the stages end on, for example 60, 120. Empty = none.");
	Sec("DASH LOOK");
	Col("dash_accent", "Accent colour", "Border and position colour of the dash look.");
	Col("dash_accent2", "Plate fade colour", "The driver plate fades from the accent into this. Pick the accent again for a solid plate.");
	Tog("dash_art_colors", "Driver colours from the art", "Each car's dash in its own colour (the art's colors.json). Cars without one keep the accent.");
	Num("dash_speed_max", "Speedo top", "Speed at the end of the dash's speed arc.", "mph");
	Sec("TOOLS");
	Act("move", "Place on screen", "Drag the panels to move them, mouse wheel to resize.", "Move overlays");
	Act("layout_editor", "Layout editor", "Build your own overlays and tickers in the browser: move every element, choose the data that feeds it, use PNGs from GIMP.", "Open layout editor");

	// ---- Phone (placed by hand beside the QR code)
	At(pPhone);
	Fix("remote", "Phone remote", "Lets phones on this network run RaceVision from their browser.", fToggle, 320, 26, 524);
	Fix("remote_port", "Port", "The network port the phone page uses (8090 unless something else has it).", fNum, 320, 102, 524);
	Fix("remote_newkey", "Pairing code", "A new code locks out every phone that scanned the old one.", fAction, 320, 178, 524, "New code");
	Fix("remote_scanner", "Scanner on the phone", "Phones can play AIMOD's team radio scanner (AIMOD running on this PC with its scanner on).", fToggle, 320, 254, 524);
}

static std::string Raw(const std::string &key) {
	EnterCriticalSection(&g_lock);
	auto it = S.raw.find(key);
	std::string v = it == S.raw.end() ? "" : it->second;
	LeaveCriticalSection(&g_lock);
	return v;
}

// settings changed in the window: the parts that live outside S (none of it touches a window)
static std::string g_artWas;                // art folders + series as last used (set when the window opens)
static void ApplySettings() {
	if (g_game) g_game->useCall = S.cameraCall;
	g_overlaysOn = S.overlays;
	{   // a new style is the whole look: the per-panel layout picks go
		static std::string styleWas;
		if (!styleWas.empty() && styleWas != S.overlayStyle) LayClearPanelKeys();
		styleWas = S.overlayStyle;
	}
	PanelSizes();
	std::string art = S.artFolders + "|" + S.artSeries;
	if (art != g_artWas) { g_artWas = art; ArtRoots(); g_artSeries = "?"; PickSeries(); }
}

// ---- the live picture, copied under the lock for painting
struct FieldRow { int slot, pos, number; std::string name; bool pit, tv; COLORREF col; bool hasCol; };
struct Snap {
	bool attached = false, camOk = false, paused = false, overlays = true, edit = false;
	int session = 0, flag = 0, lap = 0, laps = 0, cam = -1, cars = 0;
	float trackLen = 0;
	std::string track, onName, onKind, why;
	std::vector<FieldRow> rows;
};
static Snap g_snap;
static std::vector<std::string> g_logLines;
static std::string g_logPath;

static void TakeSnap() {
	Snap s;
	EnterCriticalSection(&g_lock);
	s.attached = g_game && g_game->h; s.camOk = g_game && g_game->camOk; s.paused = g_paused;
	s.overlays = g_overlaysOn; s.edit = g_edit; s.session = g_session; s.flag = g_flag; s.lap = g_leaderLaps;
	s.laps = g_lapsTotal; s.cam = g_lastCam; s.cars = (int)g_order.size(); s.trackLen = g_trackLen; s.track = g_trackName;
	if (s.attached && g_shot.car >= 0 && g_car[g_shot.car].valid) {
		s.onName = Name(g_shot.car); s.onKind = KindName[g_shot.kind]; s.why = g_shot.why;
	}
	if (s.attached)
		for (int i : g_score) {
			if (!g_car[i].valid) continue;
			FieldRow r; r.slot = i; r.pos = g_car[i].spos; r.number = g_car[i].number;
			r.name = g_car[i].first + " " + g_car[i].last; r.pit = g_car[i].pit; r.tv = i == g_shot.car;
			auto it = g_colors.find(NumKey(r.number));
			r.hasCol = it != g_colors.end();
			r.col = r.hasCol ? RGB(it->second.GetR(), it->second.GetG(), it->second.GetB()) : U_LINE;
			s.rows.push_back(r);
		}
	LeaveCriticalSection(&g_lock);
	g_snap = s;
}

// ---- drawing helpers (96-dpi coordinates)
static Color Gp(COLORREF c, int a = 255) { return Color((BYTE)a, GetRValue(c), GetGValue(c), GetBValue(c)); }
static void URound(HDC dc, double x, double y, double w, double h, double r, COLORREF fill, COLORREF line = CLR_INVALID) {
	y -= g_oy;
	Graphics gr(dc);
	gr.SetSmoothingMode(SmoothingModeAntiAlias);
	float k = g_dpi / 96.f;
	gr.ScaleTransform(k, k);
	SolidBrush b(Gp(fill));
	FillRound(gr, b, (float)x, (float)y, (float)w, (float)h, (float)r);
	if (line != CLR_INVALID) {
		GraphicsPath p;
		float X = (float)x + .5f, Y = (float)y + .5f, W = (float)w - 1, H = (float)h - 1, R = (float)r;
		p.AddArc(X, Y, R * 2, R * 2, 180, 90); p.AddArc(X + W - R * 2, Y, R * 2, R * 2, 270, 90);
		p.AddArc(X + W - R * 2, Y + H - R * 2, R * 2, R * 2, 0, 90); p.AddArc(X, Y + H - R * 2, R * 2, R * 2, 90, 90);
		p.CloseFigure();
		Pen pen(Gp(line), 1.2f);
		gr.DrawPath(&pen, &p);
	}
}
static void UDot(HDC dc, double x, double y, double d, COLORREF c) { URound(dc, x, y, d, d, d / 2, c); }
static void UText(HDC dc, const std::string &s, double x, double y, double w, double h, HFONT f, COLORREF c,
                  UINT fmt = DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS) {
	RECT r = { Px(x), Px(y - g_oy), Px(x + w), Px(y - g_oy + h) };
	SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
	DrawTextA(dc, s.c_str(), -1, &r, fmt | DT_NOPREFIX);
}
static int UTextW(HDC dc, const std::string &s, HFONT f) {
	SelectObject(dc, f); SIZE z; GetTextExtentPoint32A(dc, s.c_str(), (int)s.size(), &z); return (int)(z.cx * 96 / g_dpi);
}
static void UFill(HDC dc, double x, double y, double w, double h, COLORREF c) {
	RECT r = { Px(x), Px(y - g_oy), Px(x + w), Px(y - g_oy + h) };
	HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b);
}

// ---- control ids
enum {
	ID_PAUSE = 100, ID_OVERLAYS, ID_MOVE, ID_COPY, ID_OPENLOG, ID_FIELDLIST, ID_LOG, ID_FOLDERS, ID_SERIES, ID_ADDFOLDER,
	ID_REMOVEFOLDER, ID_PRESETS, ID_DIFFS, ID_PRESETNAME, ID_SAVEPRESET, ID_LOADPRESET, ID_DELPRESET, ID_PRESETFOLDER,
	ID_FIELD0 = 1000
};
static HWND g_btnPause, g_btnOverlays, g_btnMove, g_btnCopy, g_btnOpenLog, g_lbField, g_lbLog, g_lbFolders, g_lbSeries,
	g_btnAdd, g_btnRemove, g_lbPresets, g_lbDiffs, g_edPresetName;
struct Ctl { HWND h; int page, x, y, w, hgt; bool clipped = false; int tab = -1; };   // where it sits on its page (96-dpi, unscrolled)
static std::vector<Ctl> g_ctls;

static HWND MakeCtl(const char *cls, DWORD style, int id, int page, int x, int y, int w, int h, const char *text = "") {
	HWND c = CreateWindowExA(0, cls, text, WS_CHILD | style, Px(x), Px(y), Px(w), Px(h), g_ui, (HMENU)(INT_PTR)id,
	                         GetModuleHandle(NULL), NULL);
	SendMessage(c, WM_SETFONT, (WPARAM)(strcmp(cls, "LISTBOX") == 0 && id == ID_LOG ? fMono : fInput), TRUE);
	Ctl k; k.h = c; k.page = page; k.x = x; k.y = y; k.w = w; k.hgt = h;
	g_ctls.push_back(k);
	return c;
}
// Buttons are RaceVision's own "RVButton" (see BtnProc): Windows' BUTTON paints through its parent's DC and kept
// smudging the header and its neighbours while the page scrolled (live 2026-10-27, even with the clipping fix)
static HWND MakeButton(int id, int page, int x, int y, int w, int h, const char *text) {
	return MakeCtl("RVButton", WS_TABSTOP, id, page, x, y, w, h, text);
}
static HWND MakeList(int id, int page, int x, int y, int w, int h) {
	HWND l = MakeCtl("LISTBOX", LBS_OWNERDRAWFIXED | LBS_NODATA | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP,
	                 id, page, x, y, w, h);
	SetWindowTheme(l, L"DarkMode_Explorer", NULL);
	return l;
}

static std::string ChoicesOf(const Field &f);
static void Commit(Field &f, const std::string &value);
static std::string KeyLabel(const Field &f);
static void PlaceCtls();
static void Relayout();
static std::string CamList(const std::string &v);
static bool IsPick(const Field &f) { return f.kind == fCams || f.pick; }   // camera chips instead of a box

// where a field's control sits (96-dpi window coordinates)
static RECT FieldBox(const Field &f) {
	int X = UCX + f.x, Y = UCY + f.y;
	if (f.hdr || f.bar) return { X, Y, X + f.cw, Y + 34 };
	int cw = f.cw, ch = f.kind == fToggle ? 24 : 32;
	int bx = X + f.w - 16 - cw, by = Y + (f.ht - ch) / 2;
	return { bx, by, bx + cw, by + ch };
}

// ---- layout: the settings pages are sections and rows, placed from their content, so nothing can overlap
static HDC g_mdc = NULL;                                       // a DC just to measure text
static int MeasureW(const std::string &s, HFONT f) { return UTextW(g_mdc, s, f); }
static int HelpH(const std::string &s, int w) {                // height of a help text word-wrapped to w (96-dpi)
	if (s.empty() || w < 20) return 0;
	RECT r = { 0, 0, Px(w), 0 };
	SelectObject(g_mdc, fSmall);
	DrawTextA(g_mdc, s.c_str(), -1, &r, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
	return (int)ceil(r.bottom * 96.0 / g_dpi);
}
// a choice with more than 3 options (the Style box once presets are added) is a dropdown, not side-by-side segments
static std::vector<std::pair<std::string, std::string>> ChoiceOpts(const Field &f) {
	std::vector<std::pair<std::string, std::string>> opts;
	std::string list = ChoicesOf(f) + ";";
	size_t a = 0, b;
	while ((b = list.find(';', a)) != std::string::npos) {
		std::string one = list.substr(a, b - a); a = b + 1;
		size_t bar = one.find('|');
		if (bar != std::string::npos) opts.push_back({ one.substr(0, bar), one.substr(bar + 1) });
	}
	return opts;
}
static bool IsDropdown(const Field &f) { return f.kind == fChoice && ChoiceOpts(f).size() > 3; }
static int ControlW(const Field &f) {
	if (f.want) return f.want;
	if (IsDropdown(f)) {
		int w = 0;
		for (auto &o : ChoiceOpts(f)) w = std::max(w, MeasureW(o.second, fLabel));
		return std::max(190, std::min(w + 64, 340));
	}
	switch (f.kind) {
	case fToggle: return 46;
	case fNum: return 104;
	case fText: return 190;
	case fColor: return 130;
	case fChoice: {                                            // as wide as its options need (the Style box may list presets)
		int w = 0;
		std::string list = ChoicesOf(f) + ";";
		size_t a = 0, b;
		while ((b = list.find(';', a)) != std::string::npos) {
			std::string one = list.substr(a, b - a); a = b + 1;
			size_t bar = one.find('|');
			if (bar != std::string::npos) w += MeasureW(one.substr(bar + 1), fLabel) + 30;
		}
		return std::max(170, std::min(w, 360));
	}
	default: return 150;
	}
}

// camera chips: one chip per camera (number + name), then a + chip. Same ini values as before ("16, 19").
struct ChipBox { int cam, kind; double x, y, w; };            // kind 0 a camera, 2 the + chip, 3 the "keeps the camera" note
static const int CH_H = 26, CH_GAP = 6;
static double ChipW(int cam, bool single) {
	double bw = std::max(22, MeasureW(std::to_string(cam), fSection) + 10);
	return 4 + bw + 8 + MeasureW(CamName(cam), fSmall) + (single ? 12 : 26);
}
static void ChipsLayout(const std::string &value, bool single, double W, std::vector<ChipBox> &out, int &H) {
	out.clear();
	double x = 0, y = 0;
	std::vector<int> cams = IntList(value);
	if (single && cams.size() > 1) cams.resize(1);
	auto put = [&](int cam, int kind, double w) {
		if (x > 0 && x + w > W) { x = 0; y += CH_H + CH_GAP; }
		out.push_back({ cam, kind, x, y, w });
		x += w + CH_GAP;
	};
	for (int c : cams) put(c, 0, ChipW(c, single));
	if (!single || cams.empty()) put(0, 2, 30);
	if (cams.empty() && !single) put(0, 3, MeasureW("keeps the current camera", fSmall) + 4);
	H = (int)(y + CH_H);
}
static void ChipCell(const Field &f, int &cx, int &cy, int &cwid) {      // where a field's chips are drawn
	cx = UCX + f.x + (f.cell ? 0 : 310); cy = UCY + f.y + (f.cell ? 0 : 12); cwid = f.cell ? f.w : f.w - 326;
}

static void FlowLayout() {
	if (!g_mdc) return;
	const int gap = 16, colW = (g_cw - gap) / 2;
	for (int p = 0; p < PAGES; p++) if (p == pDirector || p == pBattles || p == pCameras || p == pReplays || p == pOverlays) g_pageEnd[p] = 0;
	for (auto &f : g_fields) if (!f.hdr && !f.bar && !IsPick(f)) f.cw = ControlW(f);
	int cp = -1, ct = -1, y = 0, left = -1;
	auto flush = [&]() { if (left >= 0) { y += g_fields[left].ht + 10; left = -1; } };
	auto endTab = [&]() { flush(); if (cp >= 0 && y > g_pageEnd[cp]) g_pageEnd[cp] = y; };
	for (auto &it : g_items) {
		if (it.page != cp || it.tab != ct) { endTab(); cp = it.page; ct = it.tab; y = TabCount(cp) ? 46 : 0; }
		switch (it.type) {
		case iSection:
			flush();
			if (y > (TabCount(cp) ? 46 : 0)) y += 12;
			it.y = y; y += 28;
			break;
		case iField: {
			Field &f = g_fields[it.field];
			if (f.fixed) break;
			if (f.full) { flush(); f.x = 0; f.w = g_cw; }
			else if (left < 0) { f.x = 0; f.w = colW; }
			else { f.x = colW + gap; f.w = colW; }
			int textW = IsPick(f) ? 280 : f.w - f.cw - 44;
			f.ht = std::max(UCARD_H, 36 + HelpH(f.help, textW) + 12);
			if (IsPick(f)) {
				std::vector<ChipBox> b; int H;
				ChipsLayout(Raw(f.key), f.single, f.w - 326, b, H);
				f.ht = std::max(f.ht, H + 24);
			}
			f.y = y;
			if (f.full) y += f.ht + 10;
			else if (left < 0) left = it.field;
			else {
				Field &l = g_fields[left];
				int hh = std::max(l.ht, f.ht);
				l.ht = f.ht = hh; f.y = l.y;
				y = l.y + hh + 10; left = -1;
			}
			break;
		}
		case iBar: {
			flush();
			it.y = y;
			int x = 100, line = 0;
			for (int fi : it.fields) {
				Field &f = g_fields[fi];
				f.cw = MeasureW(f.choices, fLabel) + 32;
				if (x + f.cw > g_cw - 14) { x = 100; line++; }
				f.x = x; f.y = y + 11 + line * 44; x += f.cw + 8;
			}
			it.h = 56 + line * 44;
			y += it.h + 10;
			break;
		}
		case iCamTable: {
			flush();
			it.y = y;
			int c = (g_cw - 190 - 14 - 16) / 2;
			g_camX1 = 190; g_camX2 = 190 + c + 16;
			g_camRowY.clear(); g_camRowH.clear();
			int ry = y + 34;
			size_t k = 0;
			for (auto &r : CAM_ROWS) {
				Field &a = g_fields[it.fields[k++]];
				Field *b = r.follow ? &g_fields[it.fields[k++]] : nullptr;
				std::vector<ChipBox> bx; int Ha = 0, Hb = 0;
				ChipsLayout(Raw(a.key), false, c, bx, Ha);
				if (b) ChipsLayout(Raw(b->key), false, c, bx, Hb);
				int rowH = std::max({ 38, Ha + 14, Hb + 14 });
				a.x = g_camX1; a.w = c; a.ht = Ha; a.y = ry + (rowH - Ha) / 2;
				if (b) { b->x = g_camX2; b->w = c; b->ht = Hb; b->y = ry + (rowH - Hb) / 2; }
				g_camRowY.push_back(ry); g_camRowH.push_back(rowH);
				ry += rowH;
			}
			it.h = ry - y + 30;                                // room for the caption under the rows
			y += it.h + 10;
			break;
		}
		case iPreview: flush(); it.y = y; it.h = 166; y += it.h + 10; break;
		case iReplayList: flush(); it.y = y; g_rlY = y; it.h = 44 + RL_ROWS * RL_ROW; y += it.h + 10; break;
		case iReplayLog: flush(); it.y = y; g_rlogY = y; it.h = 248; y += it.h; break;
		}
	}
	endTab();
	for (auto &f : g_fields)                                   // buttons beside the page title
		if (f.hdr) { f.cw = MeasureW(f.choices, fLabel) + 36; f.x = g_cw - f.cw; f.y = UHEAD + 22 - UCY; }
}
// after the width, a tab, or a camera list changed: place everything again
static void Relayout() {
	if (!g_ui) return;
	FlowLayout();
	for (auto &f : g_fields) {
		if (!f.h || f.ctl < 0) continue;
		RECT b = FieldBox(f);
		Ctl &k = g_ctls[f.ctl];
		if (f.kind == fNum || f.kind == fText) {
			k.x = b.left + 9; k.y = b.top + 7; k.w = b.right - b.left - 18 - (f.unit.empty() ? 0 : 34); k.hgt = 19;
		} else { k.x = b.left; k.y = b.top; k.w = b.right - b.left; k.hgt = b.bottom - b.top; }
	}
	PlaceCtls();
	InvalidateRect(g_ui, NULL, FALSE);
}

// ---- the camera picker: a small window under the chips, cameras grouped by kind
struct PickGroup { const char *name; int from, to; };
static const PickGroup PICK_GROUPS[] = { { "ON-BOARD", 1, 3 }, { "CHASE", 4, 8 }, { "ON THE CAR", 10, 15 }, { "TV AND TRACK", 16, 21 }, { "RACEVISION OWN", 22, 99 } };
struct PickChip { int cam; double x, y, w; };
static HWND g_pick = NULL;
static int g_pickField = -1;
static const double PICK_W = 470;
static void PickLayout(std::vector<PickChip> &out, std::vector<std::pair<std::string, double>> &heads, int &H) {
	out.clear(); heads.clear();
	double y = 46;
	int last = std::min(CamLast(), 99);
	for (auto &g : PICK_GROUPS) {
		std::vector<int> cams;
		for (int c = g.from; c <= std::min(g.to, last); c++) if (c != 9 && (c <= 21 || VCamById(c))) cams.push_back(c);
		if (cams.empty()) continue;
		heads.push_back({ g.name, y });
		y += 18;
		double x = 14;
		for (int c : cams) {
			double w = ChipW(c, true);
			if (x > 14 && x + w > PICK_W - 14) { x = 14; y += CH_H + CH_GAP; }
			out.push_back({ c, x, y, w });
			x += w + CH_GAP;
		}
		y += CH_H + 12;
	}
	H = (int)y + 30;
}
static std::string JoinInts(const std::vector<int> &v) {
	std::string s;
	for (int c : v) s += (s.empty() ? "" : ", ") + std::to_string(c);
	return s;
}
static void PickToggle(int cam) {
	if (g_pickField < 0 || g_pickField >= (int)g_fields.size()) return;
	Field &f = g_fields[g_pickField];
	std::vector<int> v = IntList(Raw(f.key));
	if (f.single) { Commit(f, std::to_string(cam)); if (g_pick) DestroyWindow(g_pick); return; }
	auto it = std::find(v.begin(), v.end(), cam);
	if (it != v.end()) v.erase(it); else v.push_back(cam);
	Commit(f, JoinInts(v));
	if (g_pick) InvalidateRect(g_pick, NULL, FALSE);
}
static void PaintPicker(HDC dc, int wpx, int hpx) {
	int oy = g_oy; g_oy = 0;
	double W = wpx * 96.0 / g_dpi, Hh = hpx * 96.0 / g_dpi;
	URound(dc, 0, 0, W, Hh, 10, RGB(32, 36, 44), RGB(90, 98, 116));
	if (g_pickField >= 0 && g_pickField < (int)g_fields.size()) {
		Field &f = g_fields[g_pickField];
		std::vector<int> cur = IntList(Raw(f.key));
		UText(dc, "Cameras for: " + KeyLabel(f), 14, 12, W - 28, 22, fLabel, U_TEXT);
		std::vector<PickChip> ch; std::vector<std::pair<std::string, double>> hd; int H;
		PickLayout(ch, hd, H);
		for (auto &h : hd) UText(dc, h.first, 14, h.second, 300, 16, fSection, U_MUTED);
		for (auto &c : ch) {
			bool sel = std::find(cur.begin(), cur.end(), c.cam) != cur.end(), ob = IsOnboard(c.cam);
			URound(dc, c.x, c.y, c.w, CH_H, 7, sel ? RGB(58, 50, 18) : U_INPUT, sel ? U_GOLD : U_LINE);
			double bw = std::max(22, MeasureW(std::to_string(c.cam), fSection) + 10);
			URound(dc, c.x + 3, c.y + 3, bw, CH_H - 6, 4, ob ? U_GOLD : RGB(42, 46, 56));
			UText(dc, std::to_string(c.cam), c.x + 3, c.y + 3, bw, CH_H - 6, fSection, ob ? U_INPUT : U_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			UText(dc, CamName(c.cam), c.x + 3 + bw + 8, c.y + 3, c.w - bw - 14, CH_H - 6, fSmall, U_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		}
		UFill(dc, 14, Hh - 30, W - 28, 1, U_LINE);
		UText(dc, std::string(f.single ? "Click a camera to choose it." : "Click a camera to add or remove it.") + "  Gold number = on-board.  Esc closes.",
		      14, Hh - 24, W - 28, 18, fSmall, U_MUTED);
	}
	g_oy = oy;
}
static LRESULT CALLBACK PickProc(HWND h, UINT m, WPARAM w, LPARAM l) {
	switch (m) {
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
		RECT rc; GetClientRect(h, &rc);
		HDC mem = CreateCompatibleDC(dc);
		HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
		HGDIOBJ old = SelectObject(mem, bmp);
		FillRect(mem, &rc, g_bgBrush);
		PaintPicker(mem, rc.right, rc.bottom);
		BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
		SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
		EndPaint(h, &ps);
		return 0;
	}
	case WM_LBUTTONDOWN: {
		double x = (short)LOWORD(l) * 96.0 / g_dpi, y = (short)HIWORD(l) * 96.0 / g_dpi;
		std::vector<PickChip> ch; std::vector<std::pair<std::string, double>> hd; int H;
		PickLayout(ch, hd, H);
		for (auto &c : ch) if (x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + CH_H) { PickToggle(c.cam); break; }
		return 0;
	}
	case WM_SETCURSOR: SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE;
	case WM_KEYDOWN: if (w == VK_ESCAPE) DestroyWindow(h); return 0;
	case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) DestroyWindow(h); return 0;
	case WM_DESTROY: if (g_pick == h) g_pick = NULL; return 0;
	}
	return DefWindowProc(h, m, w, l);
}
static void OpenPicker(int idx, POINT scr) {
	if (g_pick) DestroyWindow(g_pick);
	g_pickField = idx;
	std::vector<PickChip> ch; std::vector<std::pair<std::string, double>> hd; int H;
	PickLayout(ch, hd, H);
	int w = Px(PICK_W), h = Px(H);
	MONITORINFO mi = { sizeof mi };
	GetMonitorInfo(MonitorFromPoint(scr, MONITOR_DEFAULTTONEAREST), &mi);
	int x = std::max((int)mi.rcWork.left, std::min((int)scr.x, (int)mi.rcWork.right - w)), y = scr.y;
	if (y + h > mi.rcWork.bottom) y = std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - h);
	g_pick = CreateWindowExA(WS_EX_TOOLWINDOW, "RVPicker", "", WS_POPUP, x, y, w, h, g_ui, NULL, GetModuleHandle(NULL), NULL);
	ShowWindow(g_pick, SW_SHOW);
	SetForegroundWindow(g_pick);
	SetFocus(g_pick);
}
// a click on the page: on a camera chip? (x, py = page coordinates, py counts the scroll)
static bool ChipClick(int x, int py) {
	for (size_t i = 0; i < g_fields.size(); i++) {
		Field &f = g_fields[i];
		if (!IsPick(f) || f.page != g_page || (f.tab >= 0 && f.tab != g_tab[g_page])) continue;
		int cx, cy, cwid; ChipCell(f, cx, cy, cwid);
		std::vector<ChipBox> bx; int H;
		ChipsLayout(Raw(f.key), f.single, cwid, bx, H);
		for (auto &b : bx) {
			if (b.kind == 3 || x < cx + b.x || x >= cx + b.x + b.w || py < cy + b.y || py >= cy + b.y + CH_H) continue;
			if (b.kind == 0 && !f.single && x >= cx + b.x + b.w - 20) {          // the x on a chip
				std::vector<int> v = IntList(Raw(f.key));
				auto it = std::find(v.begin(), v.end(), b.cam);
				if (it != v.end()) v.erase(it);
				Commit(f, JoinInts(v));
				return true;
			}
			POINT p = { (LONG)((cx + b.x) * g_dpi / 96), (LONG)((cy + b.y + CH_H + 4 - g_scroll[g_page]) * g_dpi / 96) };
			ClientToScreen(g_ui, &p);
			OpenPicker((int)i, p);
			return true;
		}
	}
	return false;
}

// ---- settings: write, read back, and tell the main thread
static bool g_previewDirty = true;
static void Commit(Field &f, const std::string &value) {
	std::string v = Trim(value);
	if (f.kind == fNum) {
		char *end; strtod(v.c_str(), &end);
		bool bad = v.empty() || *end != 0;
		if (bad != f.bad) { f.bad = bad; InvalidateRect(g_ui, NULL, FALSE); }
		if (bad) return;
	}
	EnterCriticalSection(&g_lock);
	bool changed = Raw(f.key) != v;
	if (changed) { SaveKey(f.key, v); LoadSettings(); ApplySettings(); }
	LeaveCriticalSection(&g_lock);
	if (!changed) return;
	Log("-- setting %s = %s --\n", f.key.c_str(), v.c_str());
	if (f.page == pOverlays) g_previewDirty = true;
	if (IsPick(f)) Relayout();                                  // a longer or shorter list of chips changes the row height
	InvalidateRect(g_ui, NULL, FALSE);
	if (f.h && f.kind != fNum && f.kind != fText && f.kind != fCams) InvalidateRect(f.h, NULL, FALSE);
}
static void CommitEdit(int idx) {
	if (idx < 0 || idx >= (int)g_fields.size()) return;
	Field &f = g_fields[idx];
	char buf[512]; GetWindowTextA(f.h, buf, sizeof buf);
	Commit(f, buf);
}

// ---- art folders and series (read from disk, written as art_folders / art_series)
static std::vector<std::string> g_folderList;            // as written in the ini (may hold %VARS%)
struct SeriesInfo { std::string id; int cards; };
static std::vector<SeriesInfo> g_seriesList;            // [0] = automatic
static std::string Expand(const std::string &p) { char ex[MAX_PATH]; ExpandEnvironmentStringsA(p.c_str(), ex, MAX_PATH); return ex; }
static int CountPng(const std::string &dir) {
	WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((dir + "\\*.png").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return 0;
	int n = 0; do n++; while (FindNextFileA(h, &fd)); FindClose(h); return n;
}
static void ReadArt() {
	EnterCriticalSection(&g_lock);
	std::string list = S.artFolders + ";";
	LeaveCriticalSection(&g_lock);
	g_folderList.clear();
	size_t a = 0, b;
	while ((b = list.find(';', a)) != std::string::npos) {
		std::string one = Trim(list.substr(a, b - a)); a = b + 1;
		if (!one.empty()) g_folderList.push_back(one);
	}
	g_seriesList.clear();
	g_seriesList.push_back({ "auto", 0 });
	std::vector<std::string> roots;
	for (auto &root : g_folderList) roots.push_back(Expand(root));
	for (auto &root : AutoArtRoots()) roots.push_back(root);         // found without being added
	for (auto &root : roots) {
		std::string r = root + "\\series";
		WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((r + "\\*").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) continue;
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
			bool seen = false;
			for (auto &s : g_seriesList) seen |= _stricmp(s.id.c_str(), fd.cFileName) == 0;
			if (!seen) g_seriesList.push_back({ fd.cFileName, CountPng(r + "\\" + fd.cFileName + "\\numbers") });
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	SendMessage(g_lbFolders, LB_SETCOUNT, g_folderList.size(), 0);
	SendMessage(g_lbSeries, LB_SETCOUNT, g_seriesList.size(), 0);
	std::string cur = Raw("art_series");
	for (size_t i = 0; i < g_seriesList.size(); i++)
		if (_stricmp(g_seriesList[i].id.c_str(), cur.empty() ? "auto" : cur.c_str()) == 0) SendMessage(g_lbSeries, LB_SETCURSEL, i, 0);
	InvalidateRect(g_lbFolders, NULL, FALSE); InvalidateRect(g_lbSeries, NULL, FALSE);
}
static void SaveFolders() {
	std::string v;
	for (auto &f : g_folderList) v += (v.empty() ? "" : ";") + f;
	Field f; f.page = pArt; f.key = "art_folders"; f.kind = fText;
	Commit(f, v);
	ReadArt();
}
static void AddFolder() {
	IFileOpenDialog *dlg = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
	DWORD o; dlg->GetOptions(&o); dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
	dlg->SetTitle(L"Folder that holds a \"series\" folder");
	IShellItem *it = nullptr; PWSTR path = nullptr;
	if (SUCCEEDED(dlg->Show(g_ui)) && SUCCEEDED(dlg->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
		char p[MAX_PATH]; WideCharToMultiByte(CP_ACP, 0, path, -1, p, MAX_PATH, NULL, NULL);
		std::string s = p;
		// the series folder itself, or one series inside it: use the folder above
		if (s.size() > 7 && _stricmp(s.c_str() + s.size() - 7, "\\series") == 0) s.resize(s.size() - 7);
		g_folderList.push_back(s);
		CoTaskMemFree(path);
		SaveFolders();
	}
	if (it) it->Release();
	dlg->Release();
}

// ---- presets: the settings of every page but Art, saved as presets\<name>.ini next to racevision.ini.
// Art folders and overlay positions belong to the PC, so they stay as they are.
static const char *BUILTIN_PRESET = "RaceVision default";
struct PresetInfo { std::string name; bool builtin; std::map<std::string, std::string> vals; int diffs; };
struct DiffRow { std::string label, now, to; };
static std::vector<PresetInfo> g_presets;
static std::vector<DiffRow> g_diffs;
static bool g_syncing = false;                          // setting the boxes from code: not an edit

static std::string PresetDir() {
	std::string d = g_ini;
	size_t s = d.find_last_of("\\/");
	return (s == std::string::npos ? "" : d.substr(0, s + 1)) + "presets";
}
static Field *FieldByKey(const std::string &k) {
	for (auto &f : g_fields) if (f.key == k && f.kind != fAction) return &f;
	return nullptr;
}
static bool InPreset(const Field &f) { return f.kind != fAction && f.page != pPhone; }
// an empty number / switch / colour means "not set": leave the setting alone
static bool Usable(const Field &f, const std::string &v) { return !v.empty() || f.kind == fText || f.kind == fCams; }
static bool Same(const Field &f, const std::string &a, const std::string &b) {
	if (f.kind == fNum || f.kind == fToggle) return atof(a.c_str()) == atof(b.c_str());
	if (f.kind == fCams || f.key == "onboard_cams" || f.key == "stage_laps") return IntList(a) == IntList(b);
	return _stricmp(a.c_str(), b.c_str()) == 0;
}
static std::string ChoicesOf(const Field &f) { return f.key == "overlay_style" ? LayStyleChoices() : f.choices; }      // (the Style box also lists the style presets)
static std::string ShowVal(const Field &f, const std::string &v) {
	if (IsPick(f)) return CamList(v);
	if (f.kind == fToggle) return atoi(v.c_str()) ? "on" : "off";
	if (f.kind == fChoice) {
		std::string ch = ChoicesOf(f);
		size_t at = ch.find(v + "|");
		if (at != std::string::npos) { size_t a = at + v.size() + 1; return ch.substr(a, ch.find(';', a) - a); }
	}
	return v.empty() ? "(none)" : v;
}
static std::string KeyLabel(const Field &f) {
	if (!f.label.empty()) return f.unit.empty() ? f.label : f.label + " (" + f.unit + ")";
	for (auto &r : CAM_ROWS) {
		std::string k = std::string("cam_") + r.key;
		if (f.key == k) return std::string(r.label) + " opens on";
		if (f.key == k + "_follow") return std::string(r.label) + " follows with";
	}
	return f.key;
}
static void ParsePreset(const char *text, std::map<std::string, std::string> &vals) {
	std::string all = text, line;
	size_t a = 0;
	while (a < all.size()) {
		size_t b = all.find('\n', a);
		if (b == std::string::npos) b = all.size();
		std::string l = Trim(all.substr(a, b - a)); a = b + 1;
		size_t eq = l.find('=');
		if (l.empty() || l[0] == ';' || l[0] == '[' || eq == std::string::npos) continue;
		std::string k = Trim(l.substr(0, eq));
		Field *f = FieldByKey(k);
		if (f && InPreset(*f)) vals[k] = Trim(l.substr(eq + 1));
	}
}
// what loading the preset would change, against the settings now
static int CountDiffs(const PresetInfo &p, std::vector<DiffRow> *rows) {
	int n = 0;
	for (auto &f : g_fields) {
		if (!InPreset(f)) continue;
		auto it = p.vals.find(f.key);
		if (it == p.vals.end() || !Usable(f, it->second)) continue;
		std::string now = Raw(f.key);
		if (Same(f, now, it->second)) continue;
		n++;
		if (rows) rows->push_back({ KeyLabel(f), ShowVal(f, now), ShowVal(f, it->second) });
	}
	return n;
}
static int SelectedPreset() {
	int i = (int)SendMessage(g_lbPresets, LB_GETCURSEL, 0, 0);
	return i >= 0 && i < (int)g_presets.size() ? i : -1;
}
static void FillDiffs() {
	g_diffs.clear();
	int i = SelectedPreset();
	if (i >= 0) CountDiffs(g_presets[i], &g_diffs);
	SendMessage(g_lbDiffs, LB_SETCOUNT, g_diffs.size(), 0);
	InvalidateRect(g_lbDiffs, NULL, FALSE);
	InvalidateRect(g_ui, NULL, FALSE);
}
// the built-in preset and the files, with what each would change (any thread)
static std::vector<PresetInfo> PresetList() {
	std::vector<PresetInfo> list;
	PresetInfo def = { BUILTIN_PRESET, true, {}, 0 };
	ParsePreset(DEFAULT_INI, def.vals);
	list.push_back(def);
	std::vector<PresetInfo> mine;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((PresetDir() + "\\*.ini").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			std::string file = fd.cFileName;
			PresetInfo p = { file.substr(0, file.size() - 4), false, {}, 0 };
			FILE *f = fopen((PresetDir() + "\\" + file).c_str(), "r");
			if (!f) continue;
			std::string text; char line[512];
			while (fgets(line, sizeof line, f)) text += line;
			fclose(f);
			ParsePreset(text.c_str(), p.vals);
			mine.push_back(p);
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	std::sort(mine.begin(), mine.end(), [](const PresetInfo &a, const PresetInfo &b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
	list.insert(list.end(), mine.begin(), mine.end());
	for (auto &p : list) p.diffs = CountDiffs(p, nullptr);
	return list;
}
static void ReadPresets(const std::string &select = "") {
	int was = SelectedPreset();
	std::string want = !select.empty() ? select : was >= 0 ? g_presets[was].name : "";
	g_presets = PresetList();
	SendMessage(g_lbPresets, LB_SETCOUNT, g_presets.size(), 0);
	for (size_t i = 0; i < g_presets.size(); i++)
		if (_stricmp(g_presets[i].name.c_str(), want.c_str()) == 0) SendMessage(g_lbPresets, LB_SETCURSEL, i, 0);
	InvalidateRect(g_lbPresets, NULL, FALSE);
	FillDiffs();
}
// the boxes on the pages show the settings as they are now (after a preset was loaded)
static void SyncControls() {
	KillTimer(g_ui, 2);                                   // TIMER_COMMIT: a half-typed edit is replaced
	g_syncing = true;
	for (auto &f : g_fields) {
		if (!f.h) continue;
		if (f.kind == fNum || f.kind == fText || f.kind == fCams) { SetWindowTextA(f.h, Raw(f.key).c_str()); f.bad = false; }
		else InvalidateRect(f.h, NULL, FALSE);
	}
	g_syncing = false;
	Relayout();                                           // camera chips may now be longer or shorter
}
// a name that works as a file name ("" when nothing is left of it)
static std::string PresetName(const std::string &typed) {
	std::string name;
	for (char c : typed) if (!strchr("\\/:*?\"<>|", c) && (unsigned char)c >= 32) name += c;
	name = Trim(name);
	while (!name.empty() && name.back() == '.') name.pop_back();
	return name.substr(0, 60);
}
// ask: from the window (asks before replacing a preset and says what went wrong); the phone asks itself
static void SavePresetAs(const std::string &name, bool ask) {
	if (name.empty()) {
		if (ask) MessageBoxA(g_ui, "Type a name for the preset first.", "RaceVision", MB_OK | MB_ICONINFORMATION);
		return;
	}
	if (_stricmp(name.c_str(), BUILTIN_PRESET) == 0) {
		if (ask) MessageBoxA(g_ui, "That name belongs to the built-in preset. Pick another name.", "RaceVision", MB_OK | MB_ICONINFORMATION);
		return;
	}
	std::string path = PresetDir() + "\\" + name + ".ini";
	if (ask && GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
	    MessageBoxA(g_ui, ("Replace the preset \"" + name + "\" with the settings you have now?").c_str(), "RaceVision",
	                MB_YESNO | MB_ICONQUESTION) != IDYES)
		return;
	CreateDirectoryA(PresetDir().c_str(), NULL);
	FILE *f = fopen(path.c_str(), "w");
	if (!f) {
		if (ask) MessageBoxA(g_ui, ("Could not write " + path).c_str(), "RaceVision", MB_OK | MB_ICONWARNING);
		return;
	}
	fprintf(f, "; RaceVision preset \"%s\". Copy this file into another presets folder to share it.\n", name.c_str());
	EnterCriticalSection(&g_lock);
	for (auto &fl : g_fields) {
		if (!InPreset(fl)) continue;
		std::string v = Raw(fl.key);
		if (Usable(fl, v)) fprintf(f, "%s = %s\n", fl.key.c_str(), v.c_str());
	}
	LeaveCriticalSection(&g_lock);
	fclose(f);
	Log("-- preset \"%s\" saved --\n", name.c_str());
	ReadPresets(name);
}
static void SavePreset() {
	char buf[128]; GetWindowTextA(g_edPresetName, buf, sizeof buf);
	SavePresetAs(PresetName(buf), true);
}
static void LoadPresetInfo(const PresetInfo &p) {
	int n = 0;
	EnterCriticalSection(&g_lock);
	for (auto &f : g_fields) {
		if (!InPreset(f)) continue;
		auto it = p.vals.find(f.key);
		if (it == p.vals.end() || !Usable(f, it->second) || Same(f, Raw(f.key), it->second)) continue;
		SaveKey(f.key, it->second);
		n++;
	}
	if (n) { LoadSettings(); ApplySettings(); }
	LeaveCriticalSection(&g_lock);
	SyncControls();
	g_previewDirty = true;
	Log("-- preset \"%s\" loaded: %d setting%s changed --\n", p.name.c_str(), n, n == 1 ? "" : "s");
	if (!p.builtin) SetWindowTextA(g_edPresetName, p.name.c_str());
	ReadPresets();
	InvalidateRect(g_ui, NULL, FALSE);
}
static void LoadPreset() {
	int i = SelectedPreset();
	if (i < 0) return;
	PresetInfo p = g_presets[i];                          // ReadPresets replaces the list
	LoadPresetInfo(p);
}

// a request from the phone, on this window's thread like a click would be
static void RemoteOnUi(const RemoteReq &r) {
	if (r.what == rSet) {
		Field *f = FieldByKey(r.a);
		if (!f || f->page == pPhone) return;
		Commit(*f, r.b);
		if (f->h && (f->kind == fNum || f->kind == fText || f->kind == fCams)) {
			g_syncing = true; SetWindowTextA(f->h, Raw(f->key).c_str()); g_syncing = false;
		}
		if (f->key == "overlays") for (auto &o : g_fields) if (o.key == "overlays" && o.h) InvalidateRect(o.h, NULL, FALSE);
		if (g_page == pPresets) ReadPresets();
	} else if (r.what == rLoadPreset) {
		for (auto &p : PresetList()) if (_stricmp(p.name.c_str(), r.a.c_str()) == 0) { LoadPresetInfo(p); break; }
	} else if (r.what == rSavePreset) {
		SavePresetAs(r.a, false);
	}
}
static void DeletePreset() {
	int i = SelectedPreset();
	if (i < 0) return;
	std::string name = g_presets[i].name;
	if (g_presets[i].builtin) { MessageBoxA(g_ui, "The built-in preset can't be deleted.", "RaceVision", MB_OK | MB_ICONINFORMATION); return; }
	if (MessageBoxA(g_ui, ("Delete the preset \"" + name + "\"?").c_str(), "RaceVision", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
	DeleteFileA((PresetDir() + "\\" + name + ".ini").c_str());
	Log("-- preset \"%s\" deleted --\n", name.c_str());
	SendMessage(g_lbPresets, LB_SETCURSEL, (WPARAM)-1, 0);
	ReadPresets();
}

// ---- overlay previews: the real drawing code with its sample car
static Bitmap *g_prev[3] = {};
static const char *g_prevName[3] = {};
static void MakePreviews() {
	for (auto &b : g_prev) { delete b; b = nullptr; }
	EnterCriticalSection(&g_lock);
	PanelSizes();
	bool dash = DashLook();
	Panel *ps[3] = { dash ? &g_dash : &g_tag, dash ? &g_tag : &g_battle, dash ? &g_battle : &g_onboard };
	const char *names[3] = { dash ? "In-car dash" : "Driver tag", dash ? "Driver tag" : "Battle box", dash ? "Battle box" : "On-board gauges" };
	for (int k = 0; k < 3; k++) {
		Panel *p = ps[k];
		g_prev[k] = new Bitmap(p->baseW, p->baseH, PixelFormat32bppARGB);
		Graphics gr(g_prev[k]);
		gr.SetSmoothingMode(SmoothingModeAntiAlias);
		gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
		gr.Clear(C(0, 0, 0, 0));
		if (LayDrawPanel(gr, *p, true)) {}
		else if (p == &g_tag) { if (dash) DrawTagDash(gr, true); else DrawTag(gr, true); }
		else if (p == &g_battle) { if (dash) DrawBattleDash(gr, true); else DrawBattle(gr, true); }
		else if (p == &g_dash) DrawDash(gr, true); else DrawOnboard(gr, true);
		g_prevName[k] = names[k];
	}
	LeaveCriticalSection(&g_lock);
	g_previewDirty = false;
}
static void PaintPreviews(HDC dc, int top) {
	if (g_previewDirty) MakePreviews();
	float x0 = UCX, y0 = (float)top, total = 0, gap = 16, hmax = 0;
	for (auto *b : g_prev) if (b) { total += b->GetWidth(); hmax = std::max(hmax, (float)b->GetHeight()); }
	if (total <= 0) return;
	float k = std::min({ (g_cw - 2 * gap - 24) / total, 104.f / hmax, 0.6f });
	URound(dc, x0, y0, g_cw, hmax * k + 52, 10, RGB(46, 60, 74));
	Graphics gr(dc);
	gr.SetSmoothingMode(SmoothingModeAntiAlias);
	gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
	float s = g_dpi / 96.f;
	gr.ScaleTransform(s, s);
	float x = x0 + 12;
	for (int i = 0; i < 3; i++) {
		if (!g_prev[i]) continue;
		float w = g_prev[i]->GetWidth() * k, h = g_prev[i]->GetHeight() * k;
		gr.DrawImage(g_prev[i], RectF(x, y0 - g_oy + 12, w, h));
		gr.Flush();
		UText(dc, g_prevName[i], x, y0 + 18 + hmax * k, w, 20, fSmall, RGB(200, 210, 222));
		x += w + gap;
	}
}

// ---- painting
static void PaintCard(HDC dc, int x, int y, int w, int h) { URound(dc, x, y, w, h, 10, U_CARD); }

static void PaintChips(HDC dc, const Field &f) {
	int cx, cy, cwid; ChipCell(f, cx, cy, cwid);
	std::vector<ChipBox> bx; int H;
	ChipsLayout(Raw(f.key), f.single, cwid, bx, H);
	for (auto &b : bx) {
		double x = cx + b.x, y = cy + b.y;
		if (b.kind == 3) { UText(dc, "keeps the current camera", x, y + 4, b.w + 40, 18, fSmall, U_MUTED); continue; }
		if (b.kind == 2) {
			URound(dc, x, y, b.w, CH_H, 7, U_INPUT, U_LINE);
			UText(dc, "+", x, y + 2, b.w, 22, fLabel, U_MUTED, DT_CENTER | DT_TOP | DT_SINGLELINE);
			continue;
		}
		bool ob = IsOnboard(b.cam);
		URound(dc, x, y, b.w, CH_H, 7, U_INPUT, U_LINE);
		double bw = std::max(22, MeasureW(std::to_string(b.cam), fSection) + 10);
		URound(dc, x + 3, y + 3, bw, CH_H - 6, 4, ob ? U_GOLD : RGB(42, 46, 56));
		UText(dc, std::to_string(b.cam), x + 3, y + 3, bw, CH_H - 6, fSection, ob ? U_INPUT : U_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		UText(dc, CamName(b.cam), x + 3 + bw + 8, y + 3, b.w - bw - (f.single ? 18 : 34), CH_H - 6, fSmall, U_TEXT,
		      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		if (!f.single) UText(dc, "\xD7", x + b.w - 20, y + 3, 16, CH_H - 6, fLabel, U_MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
}

static void PaintField(HDC dc, const Field &f) {
	if (f.hdr || f.bar) return;
	int X = UCX + f.x, Y = UCY + f.y;
	if (IsPick(f)) {
		if (!f.cell) {
			PaintCard(dc, X, Y, f.w, f.ht);
			UText(dc, f.label, X + 16, Y + 11, 280, 20, fLabel, U_TEXT);
			UText(dc, f.help, X + 16, Y + 32, 280, f.ht - 34, fSmall, U_MUTED, DT_LEFT | DT_TOP | DT_WORDBREAK);
		}
		PaintChips(dc, f);
		return;
	}
	PaintCard(dc, X, Y, f.w, f.ht);
	RECT b = FieldBox(f);
	int textW = b.left - X - 28;
	UText(dc, f.label, X + 16, Y + 11, textW, 20, fLabel, U_TEXT);
	if (f.bad) UText(dc, "That is not a number. The last good value is still in use.", X + 16, Y + 32, textW, f.ht - 34, fSmall, U_RED, DT_LEFT | DT_TOP | DT_WORDBREAK);
	else UText(dc, f.help, X + 16, Y + 32, textW, f.ht - 34, fSmall, U_MUTED, DT_LEFT | DT_TOP | DT_WORDBREAK);
	if (f.kind == fNum || f.kind == fText) {
		URound(dc, b.left, b.top, b.right - b.left, b.bottom - b.top, 6, U_INPUT, f.bad ? U_RED : U_LINE);
		if (!f.unit.empty()) UText(dc, f.unit, b.right - 33, b.top + 6, 30, 20, fSmall, U_MUTED, DT_LEFT | DT_TOP | DT_SINGLELINE);
	}
}

static std::string CamList(const std::string &v) {
	std::string out;
	for (int c : IntList(v)) out += (out.empty() ? "" : ", ") + std::string(CamName(c));
	return out.empty() ? "keeps the camera" : out;
}

// the camera table: row names and lines; the chips are the rows' fields
static void PaintCamTable(HDC dc, const Item &it) {
	PaintCard(dc, UCX, UCY + it.y, g_cw, it.h);
	UText(dc, "SHOT", UCX + 14, UCY + it.y + 12, 150, 16, fSection, U_MUTED);
	UText(dc, "STARTS ON", UCX + g_camX1, UCY + it.y + 12, 150, 16, fSection, U_MUTED);
	UText(dc, "THEN SWITCHES TO", UCX + g_camX2, UCY + it.y + 12, 200, 16, fSection, U_MUTED);
	for (size_t r = 0; r < g_camRowY.size() && r < sizeof CAM_ROWS / sizeof CAM_ROWS[0]; r++) {
		int y = UCY + g_camRowY[r], h = g_camRowH[r];
		if (r > 0) UFill(dc, UCX + 14, y, g_cw - 28, 1, RGB(36, 40, 49));
		UText(dc, CAM_ROWS[r].label, UCX + 14, y + (h - 20) / 2, g_camX1 - 24, 20, fLabel, U_TEXT);
		if (!CAM_ROWS[r].follow) {
			const char *k = CAM_ROWS[r].key;
			UText(dc, !strcmp(k, "replay") ? "First angle. More angles: Replays page" : !strcmp(k, "pass_passed") ? "After the switch time, on the car that was passed" : "Follows the car along pit road",
			      UCX + g_camX2, y + (h - 18) / 2, g_cw - g_camX2 - 14, 18, fSmall, U_MUTED);
		}
	}
	UText(dc, "Gold number = on-board camera (gets the in-car dash).", UCX + 14, UCY + it.y + it.h - 24, g_cw - 28, 18, fSmall, U_MUTED);
}

// ---- the Replays page, This race tab: every crash this race (best first, click one to play it) and the replay log
static std::vector<int> g_rlistIds;                      // the crash under each row, as last painted
static void PaintReplayList(HDC dc) {
	const int UCW = g_cw;                                // (the list is as wide as the window now)
	struct Row { int id, lap, cars; double worth, damage, ago; bool replayed, playing, worthy; std::string who; int kind; };
	std::vector<Row> rows;
	EnterCriticalSection(&g_lock);
	for (auto &c : g_crashes)
		rows.push_back({ c.id, c.lap, (int)c.cars.size(), Worth(c), c.damage, std::max(0.0, g_simT - c.first), c.replayed,
		                 g_rp.phase && g_rp.crash == c.id, Worth(c) >= S.replayWorthy, CrashWho(c, 4), HL_CRASH });
	for (auto &c : g_highs)
		rows.push_back({ c.id, c.lap, (int)c.cars.size(), Worth(c), 0, std::max(0.0, g_simT - c.first), c.replayed,
		                 g_rp.phase && g_rp.crash == c.id, true, c.what + ":  " + CrashWho(c, 3), c.kind });
	LeaveCriticalSection(&g_lock);
	std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.worth > b.worth; });
	const int RL_Y = g_rlY;
	int cDamage = UCW - 284, cScore = UCW - 194, cPlay = UCW - 124;       // the right-hand columns follow the width
	PaintCard(dc, UCX, UCY + RL_Y, UCW, 44 + RL_ROWS * RL_ROW);
	UText(dc, "LAP", UCX + 16, UCY + RL_Y + 12, 50, 16, fSection, U_MUTED);
	UText(dc, "CARS (THE REPLAY'S FIRST CAR FIRST)", UCX + 70, UCY + RL_Y + 12, cDamage - 80, 16, fSection, U_MUTED);
	UText(dc, "DAMAGE", UCX + cDamage, UCY + RL_Y + 12, 80, 16, fSection, U_MUTED);
	UText(dc, "SCORE", UCX + cScore, UCY + RL_Y + 12, 60, 16, fSection, U_MUTED);
	UText(dc, "CLICK TO PLAY", UCX + cPlay, UCY + RL_Y + 12, 120, 16, fSection, U_MUTED);
	g_rlistIds.clear();
	if (rows.empty()) UText(dc, "No crashes or highlights logged this race yet.", UCX + 16, UCY + RL_Y + 48, 500, 20, fLabel, U_MUTED);
	for (size_t i = 0; i < rows.size() && (int)i < RL_ROWS; i++) {
		const Row &r = rows[i];
		int y = UCY + RL_Y + 38 + (int)i * RL_ROW;
		g_rlistIds.push_back(r.id);
		if (r.playing) URound(dc, UCX + 6, y, UCW - 12, RL_ROW - 4, 6, RGB(58, 50, 18));
		COLORREF tc = r.worthy ? U_TEXT : U_MUTED;
		UText(dc, std::to_string(r.lap), UCX + 16, y + 7, 50, 20, fLabel, tc);
		std::string who = r.kind ? r.who : r.who + "  (" + std::to_string(r.cars) + (r.cars == 1 ? " car)" : " cars)");
		UText(dc, who, UCX + 70, y + 7, cDamage - 80, 20, fLabel, tc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
		char b[32];
		if (r.kind) snprintf(b, sizeof b, "-"); else snprintf(b, sizeof b, "%.2f", r.damage);
		UText(dc, b, UCX + cDamage, y + 7, 80, 20, fLabel, tc);
		snprintf(b, sizeof b, "%.1f", r.worth); UText(dc, b, UCX + cScore, y + 7, 60, 20, fLabel, r.worthy ? U_GOLD : U_MUTED);
		std::string st = r.playing ? "ON NOW" : r.replayed ? "shown" : r.worthy ? "play" : "play (minor)";
		UText(dc, st, UCX + cPlay, y + 7, 110, 20, fLabel, r.playing ? U_GOLD : r.replayed ? U_MUTED : U_GREEN);
	}
}
static void PaintReplayLog(HDC dc) {
	std::vector<std::string> log;
	EnterCriticalSection(&g_logLock);
	log = g_replayLog;
	LeaveCriticalSection(&g_logLock);
	PaintCard(dc, UCX, UCY + g_rlogY, g_cw, 248);
	int n = (int)log.size(), first = std::max(0, n - 11);
	if (!n) UText(dc, "Replays and crashes worth a replay show here.", UCX + 16, UCY + g_rlogY + 14, 600, 20, fSmall, U_MUTED);
	for (int k = first; k < n; k++)
		UText(dc, log[k], UCX + 16, UCY + g_rlogY + 12 + (k - first) * 20, g_cw - 32, 18, fSmall, U_TEXT, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// the tab strip under the page title
static void TabRect(int p, int t, int &x, int &w) {
	x = UCX;
	for (int i = 0; i <= t; i++) {
		w = MeasureW(TabName(p, i), fLabel) + 32 + (p == pCameras && i == 2 ? 46 : 0);
		if (i < t) x += w;
	}
}
static void PaintTabs(HDC dc) {
	int n = TabCount(g_page);
	if (!n) return;
	UFill(dc, UCX, UCY + 35, g_cw, 1, U_LINE);
	for (int t = 0; t < n; t++) {
		int x, w; TabRect(g_page, t, x, w);
		bool sel = t == g_tab[g_page];
		UText(dc, TabName(g_page, t), x + 16, UCY + 8, w, 22, fLabel, sel ? U_TEXT : U_MUTED);
		if (g_page == pCameras && t == 2) {
			URound(dc, x + 16 + MeasureW(TabName(g_page, t), fLabel) + 8, UCY + 9, 38, 18, 9, RGB(20, 52, 54));
			UText(dc, "TEST", x + 16 + MeasureW(TabName(g_page, t), fLabel) + 8, UCY + 10, 38, 16, fSection, U_TEAL, DT_CENTER | DT_TOP | DT_SINGLELINE);
		}
		if (sel) UFill(dc, x + 6, UCY + 33, w - 12, 3, U_GOLD);
	}
}

static void PaintPageBody(HDC dc);
static void PaintPage(HDC dc) {
	g_oy = g_scroll[g_page];
	PaintPageBody(dc);
	g_oy = 0;
}
static void PaintPageBody(HDC dc) {
	UText(dc, PageName[g_page], UCX, UHEAD + 18, 600, 34, fH1, U_TEXT);
	UText(dc, PageSub[g_page], UCX, UHEAD + 52, g_page == pLive || g_page == pArt || g_page == pPresets || g_page == pPhone ? UCW : g_cw, 20, fSmall, U_MUTED);
	PaintTabs(dc);
	const Snap &n = g_snap;
	for (auto &it : g_items) {                                   // the settings pages: titles and hand-drawn blocks
		if (it.page != g_page || it.tab != g_tab[g_page]) continue;
		switch (it.type) {
		case iSection:
			UText(dc, it.text, UCX, UCY + it.y, 400, 20, fSection, U_GOLD);
			if (*it.sub.c_str()) UText(dc, it.sub, UCX + MeasureW(it.text, fSection) + 14, UCY + it.y + 1, g_cw - MeasureW(it.text, fSection) - 14, 18, fSmall, U_MUTED);
			break;
		case iBar: {
			PaintCard(dc, UCX, UCY + it.y, g_cw, it.h);
			UText(dc, it.text, UCX + 16, UCY + it.y + 20, 80, 16, fSection, U_MUTED);
			break;
		}
		case iCamTable: PaintCamTable(dc, it); break;
		case iPreview: PaintPreviews(dc, UCY + it.y); break;
		case iReplayList: PaintReplayList(dc); break;
		case iReplayLog: PaintReplayLog(dc); break;
		}
	}
	if (g_page == pLive) {
		const char *heads[4] = { "ON SCREEN", "CAMERA", "RACE", "TRACK" };
		std::string vals[4], subs[4];
		vals[0] = n.onName.empty() ? "-" : n.onName;
		subs[0] = n.onKind.empty() ? (n.attached ? "waiting for a shot" : "waiting for NR2003") : n.onKind + (n.why.empty() ? "" : "  " + n.why);
		vals[1] = n.cam >= 0 ? std::string(CamName(n.cam)) + "  (" + std::to_string(n.cam) + ")" : "-";
		subs[1] = n.cam >= 0 && IsOnboard(n.cam) ? "on board: in-car dash" : "";
		bool race = n.attached && n.session == SESSION_RACE;
		vals[2] = race ? "Lap " + std::to_string(n.lap) + (n.laps > 0 ? " / " + std::to_string(n.laps) : "") : "-";
		subs[2] = !race ? (n.attached ? "not a race session" : "") : n.flag == FLAG_GREEN ? "Green" : n.flag == 5 ? "One to go" : "Caution";
		vals[3] = n.track.empty() ? "-" : n.track;
		char t[64] = "";
		if (n.trackLen > 0) snprintf(t, sizeof t, "%.3f mi  |  %d cars", n.trackLen / 1609.344, n.cars);
		subs[3] = t;
		int cw = (UCW - 36) / 4;
		for (int i = 0; i < 4; i++) {
			int x = UCX + i * (cw + 12);
			PaintCard(dc, x, UCY, cw, 84);
			UText(dc, heads[i], x + 16, UCY + 12, cw - 32, 16, fSection, U_MUTED);
			UText(dc, vals[i], x + 16, UCY + 30, cw - 32, 28, fBig, U_TEXT);
			COLORREF sc = i == 2 && race ? (n.flag == FLAG_GREEN ? U_GREEN : U_GOLD) : U_MUTED;
			UText(dc, subs[i], x + 16, UCY + 60, cw - 32, 18, fSmall, sc);
		}
		PaintCard(dc, UCX, UCY + 100, 300, 460);
		UText(dc, "FIELD", UCX + 16, UCY + 112, 100, 16, fSection, U_MUTED);
		UText(dc, "Double-click a car: show it on TV", UCX + 70, UCY + 113, 214, 16, fSmall, U_MUTED, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
		PaintCard(dc, UCX + 316, UCY + 100, UCW - 316, 460);
		UText(dc, "LOG", UCX + 332, UCY + 112, 200, 16, fSection, U_MUTED);
		// the keys, as chips
		struct { const char *key, *what; } keys[] = { { "Ctrl+F9", "overlays on / off" }, { "Ctrl+F10", "move overlays" }, { "Ctrl+F11", "pause" }, { "Ctrl+F12", "quit" } };
		double kx = UCX;
		for (auto &k : keys) {
			int kw = MeasureW(k.key, fSection) + 16, ww = MeasureW(k.what, fSmall);
			URound(dc, kx, UCY + 574, kw, 24, 6, U_INPUT, U_LINE);
			UText(dc, k.key, kx, UCY + 578, kw, 18, fSection, U_TEXT, DT_CENTER | DT_TOP | DT_SINGLELINE);
			UText(dc, k.what, kx + kw + 8, UCY + 577, ww + 4, 18, fSmall, U_MUTED);
			kx += kw + 8 + ww + 28;
		}
	}
	if (g_page == pArt) {
		UText(dc, "ART FOLDERS", UCX, UCY, 400, 20, fSection, U_GOLD);
		UText(dc, "SERIES", UCX, UCY + 262, 400, 20, fSection, U_GOLD);
		PaintCard(dc, UCX, UCY + 26, UCW, 220);
		UText(dc, "RaceVision looks in each folder for series\\<id>\\numbers\\<n>.png, headshots\\<n>.png and colors.json.",
		      UCX + 16, UCY + 38, UCW - 32, 20, fSmall, U_MUTED);
		UText(dc, "Found without adding them: an art folder next to RaceVision.exe, AIMOD's art (while AIMOD runs) and the "
		          "companion app's art.", UCX + 284, UCY + 200, UCW - 300, 36, fSmall, U_MUTED, DT_LEFT | DT_TOP | DT_WORDBREAK);
		PaintCard(dc, UCX, UCY + 288, UCW, 270);
		std::string using_, how;
		EnterCriticalSection(&g_lock);
		using_ = g_artSeries; how = g_artHow;
		LeaveCriticalSection(&g_lock);
		UText(dc, "IN USE", UCX + 360, UCY + 304, 200, 16, fSection, U_MUTED);
		bool none = using_.empty() || using_ == "?";
		UText(dc, none ? "Not picked yet" : using_, UCX + 360, UCY + 322, 460, 28, fBig, U_TEXT);
		UText(dc, none ? "picked once cars are on track" : how, UCX + 360, UCY + 354, 460, 20, fSmall, U_MUTED);
		UText(dc, "Automatic uses the companion app's active series when it has one, otherwise the series whose "
		          "number cards cover the most cars on track. Pick a series to always use it.",
		      UCX + 360, UCY + 392, 460, 60, fSmall, U_MUTED, DT_LEFT | DT_TOP | DT_WORDBREAK);
	}
	if (g_page == pPresets) {
		UText(dc, "PRESETS", UCX, UCY, 400, 20, fSection, U_GOLD);
		PaintCard(dc, UCX, UCY + 26, 404, 470);
		UText(dc, "SAVE CURRENT SETTINGS", UCX + 420, UCY, 400, 20, fSection, U_GOLD);
		PaintCard(dc, UCX + 420, UCY + 26, 424, 124);
		UText(dc, "Save as", UCX + 436, UCY + 38, 300, 20, fLabel, U_TEXT);
		URound(dc, UCX + 436, UCY + 64, 254, 32, 6, U_INPUT, U_LINE);
		UText(dc, "Saves the settings on every page but Art.", UCX + 436, UCY + 112, 380, 20, fSmall, U_MUTED);
		UText(dc, "WHAT LOADING IT CHANGES", UCX + 420, UCY + 170, 400, 20, fSection, U_GOLD);
		PaintCard(dc, UCX + 420, UCY + 196, 424, 300);
		int i = SelectedPreset();
		std::string head = i < 0 ? "Pick a preset on the left" : g_presets[i].name;
		std::string sub = i < 0 ? "" : g_diffs.empty() ? "Same as your settings now" :
			std::to_string(g_diffs.size()) + (g_diffs.size() == 1 ? " setting changes" : " settings change") + "  (now  >  preset)";
		UText(dc, head, UCX + 436, UCY + 206, 392, 20, fLabel, U_TEXT);
		UText(dc, sub, UCX + 436, UCY + 226, 392, 18, fSmall, i >= 0 && g_diffs.empty() ? U_GREEN : U_MUTED);
		UText(dc, "Art folders and overlay positions stay as they are. Presets are .ini files in the presets folder next to "
		          "racevision.ini: copy one to share it, drop one in to use it.",
		      UCX, UCY + 512, UCW, 40, fSmall, U_MUTED, DT_LEFT | DT_TOP | DT_WORDBREAK);
	}
	if (g_page == pPhone) {
		UText(dc, "PAIR A PHONE", UCX, UCY, 400, 20, fSection, U_GOLD);
		EnterCriticalSection(&g_lock);
		std::vector<std::vector<bool>> qr = g_remoteQr;
		std::string url = g_remoteUrl, note = g_remoteNote, fw = g_remoteFirewall, tail = g_remoteTail;
		std::vector<std::pair<std::string, double>> phones(g_phones.begin(), g_phones.end());
		LeaveCriticalSection(&g_lock);
		PaintCard(dc, UCX, UCY + 26, 304, 400);
		if (!qr.empty()) {
			int n = (int)qr.size(), side = 248, mod = side / (n + 8), size = mod * (n + 8);
			int x0 = UCX + (304 - size) / 2, y0 = UCY + 26 + 20;
			URound(dc, x0, y0, size, size, 8, RGB(255, 255, 255));
			for (int y = 0; y < n; y++)
				for (int x = 0; x < n; x++)
					if (qr[y][x]) UFill(dc, x0 + (x + 4) * mod, y0 + (y + 4) * mod, mod, mod, RGB(0, 0, 0));
			UText(dc, "Scan with the phone's camera", UCX + 16, UCY + 294, 272, 20, fLabel, U_TEXT, DT_CENTER | DT_TOP | DT_SINGLELINE);
			UText(dc, url, UCX + 12, UCY + 316, 280, 18, fSmall, U_MUTED, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
			// the code on its own, for typing into the phone app (four and four, like the app shows it)
			size_t k = url.find("?k=");
			std::string code = k == std::string::npos ? "" : url.substr(k + 3);
			if (code.size() == 8) code.insert(4, " ");
			UText(dc, "PAIRING CODE", UCX + 16, UCY + 346, 272, 16, fSection, U_MUTED, DT_CENTER | DT_TOP | DT_SINGLELINE);
			UText(dc, code, UCX + 16, UCY + 364, 272, 34, fH1, U_GOLD, DT_CENTER | DT_TOP | DT_SINGLELINE);
		} else {
			UText(dc, note.empty() ? "Phone remote is off" : note, UCX + 24, UCY + 150, 256, 80, fLabel, U_MUTED,
			      DT_CENTER | DT_TOP | DT_WORDBREAK);
		}
		UText(dc, "PHONES", UCX + 320, UCY + 330, 400, 20, fSection, U_GOLD);
		PaintCard(dc, UCX + 320, UCY + 356, 524, 108);
		std::sort(phones.begin(), phones.end(), [](auto &a, auto &b) { return a.second > b.second; });
		double now = Now();
		if (phones.empty())
			UText(dc, "No phone has connected yet.", UCX + 336, UCY + 370, 480, 20, fBody, U_MUTED);
		for (size_t i = 0; i < phones.size() && i < 3; i++) {
			int y = UCY + 368 + (int)i * 30;
			double ago = now - phones[i].second;
			bool live = ago < 5;
			char when[64];
			if (live) snprintf(when, sizeof when, "connected");
			else if (ago < 120) snprintf(when, sizeof when, "%.0f s ago", ago);
			else snprintf(when, sizeof when, "%.0f min ago", ago / 60);
			UDot(dc, UCX + 338, y + 6, 9, live ? U_GREEN : U_MUTED);
			UText(dc, phones[i].first, UCX + 356, y, 260, 22, fBody, U_TEXT);
			UText(dc, when, UCX + 620, y, 208, 22, fSmall, live ? U_GREEN : U_MUTED, DT_RIGHT | DT_TOP | DT_SINGLELINE);
		}
		UText(dc, "The phone has to be on the same Wi-Fi as this PC, or on Tailscale (use the PC's 100.x address).", UCX, UCY + 476, UCW, 20, fSmall, U_MUTED);
		UText(dc, "Add the page to the phone's home screen (Share menu on iPhone, browser menu on Android) to open it like an app. It remembers the code.", UCX, UCY + 496, UCW, 20, fSmall, U_MUTED);
		UText(dc, "If the app asks for it, type the pairing code.", UCX, UCY + 516, UCW, 20, fSmall, U_MUTED);
		if (!fw.empty()) UText(dc, fw, UCX, UCY + 536, UCW, 34, fSmall, U_MUTED, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
		if (!tail.empty()) {                         // friends somewhere else can't scan this screen: send them the link
			PaintCard(dc, UCX, UCY + 576, UCW, 56);
			UText(dc, "FRIENDS ON TAILSCALE", UCX + 16, UCY + 584, 300, 16, fSection, U_GOLD, DT_LEFT | DT_TOP | DT_SINGLELINE);
			UText(dc, tail, UCX + 16, UCY + 604, UCW - 300, 22, fBody, U_TEXT, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
			bool copied = Now() - g_tailCopiedAt < 3;
			UText(dc, copied ? "Copied" : "Click to copy, then send it to them", UCX + UCW - 300, UCY + 594, 284, 22, fSmall,
			      copied ? U_GREEN : U_MUTED, DT_RIGHT | DT_TOP | DT_SINGLELINE);
		}
	}
	for (auto &f : g_fields) if (f.page == g_page && (f.tab < 0 || f.tab == g_tab[g_page])) PaintField(dc, f);
}

// the side bar: a heading before each group, one row per page
static int NavY(int p) {
	int y = UHEAD + 10;
	for (int q = 0; q <= p; q++) {
		if (PageGroup[q]) y += q ? 30 : 20;
		if (q == p) return y;
		y += 34;
	}
	return y;
}
static const int NAV_H = 32;
static int NavBottom() { return NavY(PAGES - 1) + NAV_H; }

static void PaintChrome(HDC dc, int mx, int my) {
	UFill(dc, 0, 0, g_uw, UHEAD, U_SIDE);
	UFill(dc, 0, UHEAD, USIDE, g_uh - UHEAD, U_SIDE);
	UFill(dc, 0, UHEAD - 1, g_uw, 1, RGB(30, 33, 40));
	int lx = 24;
	if (Bitmap *logo = LogoAt(Px(52))) {
		Graphics gr(dc);
		gr.DrawImage(logo, Px(14), Px(6));
		lx = 76;
	}
	int w = UTextW(dc, "RACE", fTitle);
	UText(dc, "RACE", lx, 13, 200, 40, fTitle, U_TEXT);
	UText(dc, "VISION", lx + w, 13, 200, 40, fTitle, U_GOLD);
	UText(dc, "NR2003 TV DIRECTOR", lx + w + UTextW(dc, "VISION", fTitle) + 14, 27, 200, 18, fSection, U_MUTED);
	for (int p = 0; p < PAGES; p++) {
		int y = NavY(p);
		if (PageGroup[p]) UText(dc, PageGroup[p], 20, y - (p ? 22 : 16), 150, 16, fSection, U_MUTED);
		bool sel = p == g_page, hot = mx >= 0 && mx < USIDE && my >= y && my < y + NAV_H;
		if (sel || hot) URound(dc, 10, y, USIDE - 20, NAV_H, 8, sel ? U_CARD : RGB(19, 21, 27));
		if (sel) URound(dc, 10, y + 8, 4, NAV_H - 16, 2, U_GOLD);
		UText(dc, PageName[p], 30, y + 5, 150, 22, fNav, sel ? U_TEXT : U_MUTED);
	}
	const Snap &n = g_snap;
	int y = g_uh - 120;
	UText(dc, "STATUS", 28, y, 150, 16, fSection, U_MUTED);
	struct { const char *label; std::string val; COLORREF c; } rows[] = {
		{ "NR2003", n.attached ? "connected" : "waiting", n.attached ? U_GREEN : U_MUTED },
		{ "Camera", n.camOk ? "ready" : "-", n.camOk ? U_GREEN : U_MUTED },
		{ "Director", n.paused ? "paused" : "directing", n.paused ? U_GOLD : U_GREEN },
		{ "Session", !n.attached ? "-" : n.session == SESSION_RACE ? "race" : "not racing", n.attached && n.session == SESSION_RACE ? U_GREEN : U_MUTED },
	};
	for (int i = 0; i < 4; i++) {
		int ry = y + 22 + i * 22;
		UDot(dc, 28, ry + 5, 9, rows[i].c);
		UText(dc, rows[i].label, 46, ry, 70, 20, fBody, U_MUTED);
		UText(dc, rows[i].val, 110, ry, 80, 20, fBody, U_TEXT);
	}
}

// ---- owner-drawn buttons, toggles, choices, colour swatches and list rows
static Field *FieldOf(HWND h) { for (auto &f : g_fields) if (f.h == h) return &f; return nullptr; }

static void DrawButtonBody(DRAWITEMSTRUCT *d);
// Buttons draw on their parent's DC (the button class's CS_PARENTDC), which isn't clipped to the button: a toggle half
// under the header or caught mid-scroll painted its whole pill over the header buttons and the boxes beside it
// (live 2026-10-27: yellow smudges). Clip to the button's own visible part, and draw with no page scroll applied.
static void DrawButton(DRAWITEMSTRUCT *d) {
	int saved = SaveDC(d->hDC), oy = g_oy;
	g_oy = 0;
	RECT rc = d->rcItem;
	IntersectClipRect(d->hDC, rc.left, rc.top, rc.right, rc.bottom);
	HRGN rg = CreateRectRgn(0, 0, 0, 0);
	if (GetWindowRgn(d->hwndItem, rg) != ERROR) { RECT b; GetRgnBox(rg, &b); IntersectClipRect(d->hDC, b.left, b.top, b.right, b.bottom); }
	DeleteObject(rg);
	DrawButtonBody(d);
	g_oy = oy;
	RestoreDC(d->hDC, saved);
}
static void DrawButtonBody(DRAWITEMSTRUCT *d) {
	HDC dc = d->hDC;
	RECT rc = d->rcItem;
	double W = (rc.right - rc.left) * 96.0 / g_dpi, H = (rc.bottom - rc.top) * 96.0 / g_dpi;
	Field *f = FieldOf(d->hwndItem);
	// the card colour behind (the button sits on a card or on the header)
	bool onHeader = d->CtlID == ID_PAUSE || d->CtlID == ID_OVERLAYS || d->CtlID == ID_MOVE;
	bool onCard = f || d->CtlID == ID_ADDFOLDER || d->CtlID == ID_REMOVEFOLDER || d->CtlID == ID_SAVEPRESET ||
	              d->CtlID == ID_LOADPRESET || d->CtlID == ID_DELPRESET || d->CtlID == ID_PRESETFOLDER;
	if (f && f->hdr) onCard = false;
	COLORREF behind = onHeader ? U_SIDE : onCard ? U_CARD : U_BG;
	HBRUSH bb = CreateSolidBrush(behind); FillRect(dc, &rc, bb); DeleteObject(bb);
	bool down = (d->itemState & ODS_SELECTED) != 0, focus = (d->itemState & ODS_FOCUS) != 0;
	if (f && f->kind == fToggle) {
		bool on = atoi(Raw(f->key).c_str()) != 0;
		URound(dc, 0, 0, W, H, H / 2, on ? U_GOLD : U_LINE);
		UDot(dc, on ? W - H + 3 : 3, 3, H - 6, on ? U_INPUT : RGB(170, 176, 190));
		return;
	}
	if (f && f->kind == fChoice) {
		std::vector<std::pair<std::string, std::string>> opts = ChoiceOpts(*f);
		if (opts.size() > 3) {                                  // dropdown: current choice + arrow
			std::string cur = Raw(f->key), shown = cur;
			for (auto &o : opts) if (_stricmp(cur.c_str(), o.first.c_str()) == 0) shown = o.second;
			URound(dc, 0, 0, W, H, 6, U_INPUT, down ? U_GOLD : U_LINE);
			UText(dc, shown, 12, 0, W - 40, H, fLabel, U_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
			double ax = W - 22, ay = H / 2 - 2;                 // little down arrow
			POINT tri[3] = { { Px(ax), Px(ay) }, { Px(ax + 10), Px(ay) }, { Px(ax + 5), Px(ay + 5) } };
			HBRUSH ab = CreateSolidBrush(U_GOLD); HPEN np = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN)); HBRUSH ob = (HBRUSH)SelectObject(dc, ab);
			Polygon(dc, tri, 3);
			SelectObject(dc, ob); SelectObject(dc, np); DeleteObject(ab);
			return;
		}
		URound(dc, 0, 0, W, H, 6, U_INPUT, U_LINE);
		double sw = W / std::max<size_t>(1, opts.size());
		std::string cur = Raw(f->key);
		for (size_t i = 0; i < opts.size(); i++) {
			bool sel = _stricmp(cur.c_str(), opts[i].first.c_str()) == 0;
			if (sel) URound(dc, i * sw + 3, 3, sw - 6, H - 6, 4, U_GOLD);
			UText(dc, opts[i].second, i * sw, 0, sw, H, fLabel, sel ? U_INPUT : U_MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		}
		return;
	}
	if (f && f->kind == fColor) {
		std::string v = Raw(f->key);
		unsigned rgb = 0; sscanf(v.c_str() + (!v.empty() && v[0] == '#'), "%x", &rgb);
		URound(dc, 0, 0, W, H, 6, U_INPUT, down ? U_GOLD : U_LINE);
		URound(dc, 6, 6, H - 12, H - 12, 4, RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255));
		UText(dc, v, H, 0, W - H - 6, H, fInput, U_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
		return;
	}
	// plain button: gold when it shows a state that is on (paused, moving overlays)
	char text[128]; GetWindowTextA(d->hwndItem, text, sizeof text);
	bool active = (d->CtlID == ID_PAUSE && g_snap.paused) || ((d->CtlID == ID_MOVE || (f && f->key == "move")) && g_snap.edit) ||
	              (f && f->key == "replay_now");              // the main replay button is gold
	COLORREF fill = active ? U_GOLD : down ? RGB(44, 49, 60) : RGB(34, 38, 47);
	URound(dc, 0, 0, W, H, 7, fill, focus && !active ? RGB(90, 98, 115) : CLR_INVALID);
	if (d->CtlID == ID_OVERLAYS) UDot(dc, 14, H / 2 - 4, 8, g_snap.overlays ? U_GREEN : U_RED);
	UText(dc, text, d->CtlID == ID_OVERLAYS ? 10 : 0, 0, W, H, fLabel, active ? U_INPUT : U_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void DrawListRow(DRAWITEMSTRUCT *d) {
	if (d->itemID == (UINT)-1) return;
	HDC dc = d->hDC;
	RECT rc = d->rcItem;
	bool sel = (d->itemState & ODS_SELECTED) != 0;
	HBRUSH bb = CreateSolidBrush(U_CARD); FillRect(dc, &rc, bb); DeleteObject(bb);
	double x0 = rc.left * 96.0 / g_dpi, y0 = rc.top * 96.0 / g_dpi, W = (rc.right - rc.left) * 96.0 / g_dpi,
	       H = (rc.bottom - rc.top) * 96.0 / g_dpi;
	if (d->CtlID == ID_LOG) {
		if (d->itemID >= g_logLines.size()) return;
		const std::string &l = g_logLines[d->itemID];
		COLORREF c = U_TEXT;
		if (l.find("FAIL") != std::string::npos || l.find("Cannot") == 0 || l.find("failed") != std::string::npos) c = U_RED;
		else if (l.rfind("--", 0) == 0) c = U_TEAL;
		else if (l.rfind("   ", 0) == 0) c = U_MUTED;
		if (l.size() > 11 && l[0] == '[' && l[9] == ']') {          // [hh:mm:ss] KIND ...
			UText(dc, l.substr(0, 10), x0 + 4, y0 + 1, 80, H, fMono, U_MUTED);
			size_t sp = l.find(' ', 11);
			std::string kind = l.substr(11, sp == std::string::npos ? std::string::npos : sp - 11);
			UText(dc, kind, x0 + 84, y0 + 1, 80, H, fMono, U_GOLD);
			if (sp != std::string::npos) UText(dc, Trim(l.substr(sp)), x0 + 160, y0 + 1, W - 164, H, fMono, U_TEXT);
		} else UText(dc, l, x0 + 4, y0 + 1, W - 8, H, fMono, c);
		return;
	}
	if (d->CtlID == ID_FIELDLIST) {
		if (d->itemID >= g_snap.rows.size()) return;
		const FieldRow &r = g_snap.rows[d->itemID];
		if (r.tv || sel) URound(dc, x0 + 2, y0 + 1, W - 4, H - 2, 5, r.tv ? RGB(58, 50, 18) : RGB(36, 40, 50));
		UText(dc, std::to_string(r.pos), x0 + 6, y0, 28, H, fLabel, r.pos == 1 ? U_GOLD : U_MUTED, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
		URound(dc, x0 + 42, y0 + 5, 40, H - 10, 4, r.hasCol ? r.col : U_LINE);
		int lum = (GetRValue(r.col) * 3 + GetGValue(r.col) * 6 + GetBValue(r.col)) / 10;
		UText(dc, NumLabel(r.number), x0 + 42, y0, 40, H, fLabel, r.hasCol && lum > 150 ? U_INPUT : U_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		UText(dc, r.name, x0 + 92, y0, W - 150, H, fBody, U_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		if (r.pit) UText(dc, "PIT", x0 + W - 56, y0, 30, H, fSection, U_TEAL, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
		if (r.tv) UText(dc, "TV", x0 + W - 26, y0, 22, H, fSection, U_GOLD, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
		return;
	}
	if (d->CtlID == ID_FOLDERS) {
		if (d->itemID >= g_folderList.size()) return;
		if (sel) URound(dc, x0 + 2, y0 + 1, W - 4, H - 2, 5, RGB(36, 40, 50));
		std::string p = Expand(g_folderList[d->itemID]);
		bool ok = IsDir(p + "\\series");
		UDot(dc, x0 + 10, y0 + H / 2 - 4, 8, ok ? U_GREEN : U_RED);
		UText(dc, p, x0 + 28, y0, W - 200, H, fBody, U_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
		UText(dc, ok ? "series folder found" : "no series folder", x0 + W - 166, y0, 156, H, fSmall, ok ? U_MUTED : U_RED,
		      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
		return;
	}
	if (d->CtlID == ID_PRESETS) {
		if (d->itemID >= g_presets.size()) return;
		const PresetInfo &p = g_presets[d->itemID];
		if (sel) URound(dc, x0 + 2, y0 + 1, W - 4, H - 2, 5, RGB(36, 40, 50));
		bool now = p.diffs == 0;
		UDot(dc, x0 + 10, y0 + H / 2 - 4, 8, now ? U_GOLD : U_LINE);
		UText(dc, p.name, x0 + 28, y0, W - 150, H, fLabel, U_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		std::string tag = now ? "in use" : p.builtin ? "built in" : std::to_string(p.diffs) + (p.diffs == 1 ? " change" : " changes");
		UText(dc, tag, x0 + W - 116, y0, 106, H, fSmall, now ? U_GOLD : U_MUTED, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
		return;
	}
	if (d->CtlID == ID_DIFFS) {
		if (d->itemID >= g_diffs.size()) return;
		const DiffRow &r = g_diffs[d->itemID];
		UText(dc, r.label, x0 + 8, y0, 180, H, fBody, U_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		UText(dc, r.now, x0 + 192, y0, 88, H, fBody, U_MUTED, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		UText(dc, ">", x0 + 284, y0, 16, H, fBody, U_MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		UText(dc, r.to, x0 + 304, y0, W - 308, H, fLabel, U_GOLD, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		return;
	}
	if (d->CtlID == ID_SERIES) {
		if (d->itemID >= g_seriesList.size()) return;
		if (sel) URound(dc, x0 + 2, y0 + 1, W - 4, H - 2, 5, U_GOLD);
		const SeriesInfo &s = g_seriesList[d->itemID];
		UText(dc, d->itemID == 0 ? "Automatic" : s.id, x0 + 12, y0, W - 120, H, fLabel, sel ? U_INPUT : U_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
		if (d->itemID > 0)
			UText(dc, std::to_string(s.cards) + " cards", x0 + W - 110, y0, 98, H, fSmall, sel ? U_INPUT : U_MUTED, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
	}
}

// RVButton: paints itself only (its own DC, drawn off screen then copied), with DrawButtonBody; a click (mouse up over
// it, or Space / Enter) sends the parent WM_COMMAND BN_CLICKED like a BUTTON
static LRESULT CALLBACK BtnProc(HWND h, UINT m, WPARAM w, LPARAM l) {
	switch (m) {
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
		RECT rc; GetClientRect(h, &rc);
		if (rc.right > 0 && rc.bottom > 0) {
			HDC mem = CreateCompatibleDC(dc);
			HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
			HGDIOBJ old = SelectObject(mem, bmp);
			DRAWITEMSTRUCT d = {};
			d.CtlType = ODT_BUTTON; d.CtlID = GetDlgCtrlID(h); d.hwndItem = h; d.hDC = mem; d.rcItem = rc;
			if (GetPropA(h, "rvDown")) d.itemState |= ODS_SELECTED;
			if (GetFocus() == h) d.itemState |= ODS_FOCUS;
			int oy = g_oy; g_oy = 0;
			DrawButtonBody(&d);
			g_oy = oy;
			BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
			SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
		}
		EndPaint(h, &ps);
		return 0;
	}
	case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
		SetFocus(h); SetCapture(h); SetPropA(h, "rvDown", (HANDLE)1); InvalidateRect(h, NULL, FALSE);
		return 0;
	case WM_LBUTTONUP: {
		bool was = GetPropA(h, "rvDown") != NULL;
		RemovePropA(h, "rvDown");
		if (GetCapture() == h) ReleaseCapture();
		InvalidateRect(h, NULL, FALSE);
		POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
		RECT rc; GetClientRect(h, &rc);
		if (was && PtInRect(&rc, pt)) SendMessage(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), (LPARAM)h);
		return 0;
	}
	case WM_CAPTURECHANGED:
		if (GetPropA(h, "rvDown")) { RemovePropA(h, "rvDown"); InvalidateRect(h, NULL, FALSE); }
		return 0;
	case WM_KEYDOWN:
		if (w == VK_SPACE || w == VK_RETURN) { SendMessage(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), (LPARAM)h); return 0; }
		break;
	case WM_SETFOCUS: case WM_KILLFOCUS:
		InvalidateRect(h, NULL, FALSE);
		break;
	case WM_SETTEXT: {
		LRESULT r = DefWindowProcA(h, m, w, l);
		InvalidateRect(h, NULL, FALSE);
		return r;
	}
	case WM_DESTROY: RemovePropA(h, "rvDown"); break;
	}
	return DefWindowProcA(h, m, w, l);
}

// ---- page scrolling
static int PageBottom(int p) {                   // where the page ends, with its bottom margin (96-dpi, unscrolled)
	return UCY + g_pageEnd[p] + 16;
}
static int MaxScroll(int p) { return std::max(0, PageBottom(p) - g_uh); }
static bool CtlVisible(const Ctl &c) { return c.page < 0 || (c.page == g_page && (c.tab < 0 || c.tab == g_tab[g_page])); }
// the page's controls follow the scroll: one sliding under the header is cut off there, one out of sight is hidden
static void PlaceCtls() {
	int oy = g_scroll[g_page];
	for (auto &c : g_ctls) {
		if (c.page < 0 || c.page != g_page) continue;
		if (c.tab >= 0 && c.tab != g_tab[g_page]) { ShowWindow(c.h, SW_HIDE); continue; }       // another tab's control
		int y = c.y - oy;
		bool gone = y + c.hgt <= UHEAD || y >= g_uh, clip = !gone && y < UHEAD;
		SetWindowPos(c.h, NULL, Px(c.x), Px(y), Px(c.w), Px(c.hgt),
		             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | (gone ? SWP_HIDEWINDOW : SWP_SHOWWINDOW));
		if (clip) SetWindowRgn(c.h, CreateRectRgn(0, Px(UHEAD - y), Px(c.w), Px(c.hgt)), TRUE);
		else if (c.clipped) SetWindowRgn(c.h, NULL, TRUE);
		c.clipped = clip;
		if (!gone) RedrawWindow(c.h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
	}
}
static void ScrollTo(int v) {
	v = std::max(0, std::min(MaxScroll(g_page), v));
	if (v == g_scroll[g_page]) return;
	g_scroll[g_page] = v;
	SendMessage(g_ui, WM_SETREDRAW, FALSE, 0);      // every control moved first, then one redraw of the lot
	PlaceCtls();
	SendMessage(g_ui, WM_SETREDRAW, TRUE, 0);
	RedrawWindow(g_ui, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}
// the bar on the right edge of the page (only when the page doesn't fit)
static int g_dragBar = -1;                       // dragging the thumb: where it was grabbed
static bool ScrollBar(int &top, int &len, int &thumbY, int &thumbH) {
	int mx = MaxScroll(g_page);
	if (mx <= 0) return false;
	top = UHEAD + 8; len = g_uh - UHEAD - 16;
	thumbH = std::max(40, len * (g_uh - UHEAD) / std::max(1, PageBottom(g_page) - UHEAD));
	thumbY = top + (len - thumbH) * g_scroll[g_page] / mx;
	return true;
}
static void PaintScrollBar(HDC dc) {
	int top, len, ty, th;
	if (!ScrollBar(top, len, ty, th)) return;
	URound(dc, UW - 14, top, 7, len, 3.5, RGB(28, 31, 38));
	URound(dc, UW - 14, ty, 7, th, 3.5, g_dragBar >= 0 ? U_MUTED : U_LINE);
}
static void DragBarTo(int y) {
	int top, len, ty, th;
	if (ScrollBar(top, len, ty, th) && len > th)
		ScrollTo((int)lround((double)(y - g_dragBar - top) * MaxScroll(g_page) / (len - th)));
}
// a part of the page to redraw (96-dpi page coordinates, scrolled)
static RECT PageRc(int x, int y, int x2, int y2) {
	int oy = g_scroll[g_page];
	return { Px(x), Px(std::max(UHEAD, y - oy)), Px(x2), Px(std::max(UHEAD, y2 - oy)) };
}

// ---- pages
static void ShowPage(int p) {
	g_page = p;
	for (auto &c : g_ctls) ShowWindow(c.h, CtlVisible(c) ? SW_SHOW : SW_HIDE);
	g_scroll[p] = std::min(g_scroll[p], MaxScroll(p));
	PlaceCtls();
	if (p == pArt) ReadArt();
	if (p == pPresets) ReadPresets();
	InvalidateRect(g_ui, NULL, FALSE);
}

static void DrainLog() {
	std::vector<std::string> fresh;
	EnterCriticalSection(&g_logLock);
	fresh.swap(g_logNew);
	LeaveCriticalSection(&g_logLock);
	if (fresh.empty()) return;
	int count = (int)g_logLines.size(), top = (int)SendMessage(g_lbLog, LB_GETTOPINDEX, 0, 0);
	RECT r; GetClientRect(g_lbLog, &r);
	int rowH = (int)SendMessage(g_lbLog, LB_GETITEMHEIGHT, 0, 0), visible = rowH > 0 ? r.bottom / rowH : 1;
	bool atEnd = top + visible >= count - 1;
	for (auto &l : fresh) g_logLines.push_back(l);
	if (g_logLines.size() > 5000) g_logLines.erase(g_logLines.begin(), g_logLines.begin() + 1000);
	SendMessage(g_lbLog, WM_SETREDRAW, FALSE, 0);
	SendMessage(g_lbLog, LB_SETCOUNT, g_logLines.size(), 0);
	SendMessage(g_lbLog, LB_SETTOPINDEX, atEnd ? std::max(0, (int)g_logLines.size() - visible) : top, 0);
	SendMessage(g_lbLog, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(g_lbLog, NULL, FALSE);
}

static void CopyText(const std::string &all) {
	if (!OpenClipboard(g_ui)) return;
	EmptyClipboard();
	HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, all.size() + 1);
	if (m) { memcpy(GlobalLock(m), all.c_str(), all.size() + 1); GlobalUnlock(m); SetClipboardData(CF_TEXT, m); }
	CloseClipboard();
}

static void CopyLog() {
	std::string all;
	for (auto &l : g_logLines) all += l + "\r\n";
	CopyText(all);
}

static void Refresh() {
	size_t had = g_snap.rows.size();
	std::vector<FieldRow> old = g_snap.rows;
	Snap before = g_snap;
	TakeSnap();
	bool rowsChanged = had != g_snap.rows.size();
	for (size_t i = 0; !rowsChanged && i < had; i++)
		rowsChanged = old[i].slot != g_snap.rows[i].slot || old[i].pit != g_snap.rows[i].pit || old[i].tv != g_snap.rows[i].tv ||
		              old[i].hasCol != g_snap.rows[i].hasCol;
	if (rowsChanged) {
		int top = (int)SendMessage(g_lbField, LB_GETTOPINDEX, 0, 0), sel = (int)SendMessage(g_lbField, LB_GETCURSEL, 0, 0);
		SendMessage(g_lbField, WM_SETREDRAW, FALSE, 0);
		SendMessage(g_lbField, LB_SETCOUNT, g_snap.rows.size(), 0);
		SendMessage(g_lbField, LB_SETTOPINDEX, top, 0);
		if (sel >= 0 && sel < (int)g_snap.rows.size()) SendMessage(g_lbField, LB_SETCURSEL, sel, 0);
		SendMessage(g_lbField, WM_SETREDRAW, TRUE, 0);
		InvalidateRect(g_lbField, NULL, FALSE);
	}
	bool header = before.paused != g_snap.paused || before.overlays != g_snap.overlays || before.edit != g_snap.edit;
	if (header) {
		SetWindowTextA(g_btnPause, g_snap.paused ? "Resume directing" : "Pause directing");
		SetWindowTextA(g_btnOverlays, g_snap.overlays ? "Overlays on" : "Overlays off");
		SetWindowTextA(g_btnMove, g_snap.edit ? "Done moving" : "Move overlays");
		for (auto &f : g_fields) if (f.h && (f.key == "move" || f.key == "overlays")) {
			if (f.key == "move") SetWindowTextA(f.h, g_snap.edit ? "Done moving" : "Move overlays");
			InvalidateRect(f.h, NULL, FALSE);
		}
		InvalidateRect(g_btnPause, NULL, FALSE); InvalidateRect(g_btnOverlays, NULL, FALSE); InvalidateRect(g_btnMove, NULL, FALSE);
	}
	// the status, the live cards and the art page's "in use" change by themselves
	RECT side = { 0, Px(g_uh - 125), Px(USIDE), Px(g_uh) };
	InvalidateRect(g_ui, &side, FALSE);
	if (g_page == pLive) { RECT cards = PageRc(UCX, UCY, UCX + UCW, UCY + 90); InvalidateRect(g_ui, &cards, FALSE); }
	if (g_page == pArt) { RECT a = PageRc(UCX + 350, UCY + 300, UCX + UCW, UCY + 380); InvalidateRect(g_ui, &a, FALSE); }
	if (g_page == pReplays && g_tab[pReplays] == 0) { RECT a = PageRc(UCX, UCY + g_rlY, UCX + g_cw, UCY + g_rlogY + 260); InvalidateRect(g_ui, &a, FALSE); }
	if (g_page == pPhone) {                                       // the code, the link and the phones list
		RECT a = PageRc(UCX, UCY + 26, UCX + 304, UCY + 426), b = PageRc(UCX + 320, UCY + 356, UCX + UCW, UCY + 464);
		RECT t = PageRc(UCX, UCY + 576, UCX + UCW, UCY + 632);
		InvalidateRect(g_ui, &a, FALSE); InvalidateRect(g_ui, &b, FALSE); InvalidateRect(g_ui, &t, FALSE);
	}
}

static const UINT_PTR TIMER_REFRESH = 1, TIMER_COMMIT = 2;
static int g_pendingEdit = -1, g_mouseX = -1, g_mouseY = -1;

static int NavHit(int y) {                       // the page under a y in the side bar, or -1
	for (int p = 0; p < PAGES; p++) if (y >= NavY(p) && y < NavY(p) + NAV_H) return p;
	return -1;
}

static void PlaceHeader() {                      // the three buttons on the right of the header
	int bx = g_uw - 24;
	bx -= 140; MoveWindow(g_btnMove, Px(bx), Px(14), Px(140), Px(36), TRUE);
	bx -= 150; MoveWindow(g_btnOverlays, Px(bx), Px(14), Px(140), Px(36), TRUE);
	bx -= 170; MoveWindow(g_btnPause, Px(bx), Px(14), Px(160), Px(36), TRUE);
}

static LRESULT CALLBACK UiProc(HWND h, UINT m, WPARAM w, LPARAM l) {
	switch (m) {
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
		RECT rc; GetClientRect(h, &rc);
		HDC mem = CreateCompatibleDC(dc);
		HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
		HGDIOBJ old = SelectObject(mem, bmp);
		FillRect(mem, &rc, g_bgBrush);
		PaintPage(mem);                          // scrolled: the header and the side bar are painted over it
		PaintChrome(mem, g_mouseX, g_mouseY);
		PaintScrollBar(mem);
		BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
		       mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
		SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
		EndPaint(h, &ps);
		return 0;
	}
	case WM_MOUSEMOVE: {
		int x = (int)(short)LOWORD(l) * 96 / g_dpi, y = (int)(short)HIWORD(l) * 96 / g_dpi;
		if (g_dragBar >= 0) { DragBarTo(y); return 0; }
		bool was = g_mouseX >= 0 && g_mouseX < USIDE, now = x < USIDE;
		g_mouseX = x; g_mouseY = y;
		if (was || now) { RECT r = { 0, Px(UHEAD), Px(USIDE), Px(NavBottom()) }; InvalidateRect(h, &r, FALSE); }
		TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
		bool hand = now && NavHit(y) >= 0;
		if (!now) {                                                  // tabs and camera chips are clickable too
			int py = y + g_scroll[g_page], n = TabCount(g_page);
			if (n && py >= UCY && py < UCY + 36) for (int t = 0; t < n; t++) { int tx, tw; TabRect(g_page, t, tx, tw); if (x >= tx && x < tx + tw) hand = true; }
			for (auto &f : g_fields) {
				if (hand || !IsPick(f) || f.page != g_page || (f.tab >= 0 && f.tab != g_tab[g_page])) continue;
				int cx, cy, cwid; ChipCell(f, cx, cy, cwid);
				std::vector<ChipBox> bx; int H;
				ChipsLayout(Raw(f.key), f.single, cwid, bx, H);
				for (auto &b : bx) if (b.kind != 3 && x >= cx + b.x && x < cx + b.x + b.w && py >= cy + b.y && py < cy + b.y + CH_H) hand = true;
			}
		}
		SetCursor(LoadCursor(NULL, hand ? IDC_HAND : IDC_ARROW));
		return 0;
	}
	case WM_MOUSELEAVE: {
		g_mouseX = g_mouseY = -1;
		RECT r = { 0, Px(UHEAD), Px(USIDE), Px(NavBottom()) }; InvalidateRect(h, &r, FALSE);
		return 0;
	}
	case WM_SETCURSOR:
		if (LOWORD(l) == HTCLIENT && (HWND)w == h) return TRUE;   // WM_MOUSEMOVE sets it
		break;
	case WM_LBUTTONDOWN: {
		int x = (int)(short)LOWORD(l) * 96 / g_dpi, y = (int)(short)HIWORD(l) * 96 / g_dpi;
		SetFocus(h);
		int top, len, ty, th;
		if (x >= g_uw - 24 && y > UHEAD && ScrollBar(top, len, ty, th)) {
			g_dragBar = y >= ty && y < ty + th ? y - ty : th / 2;     // a click on the track jumps there
			SetCapture(h);
			DragBarTo(y);
			InvalidateRect(h, NULL, FALSE);
			return 0;
		}
		int py = y + g_scroll[g_page];                              // on the (scrolled) page
		if (x < USIDE) { int p = NavHit(y); if (p >= 0) ShowPage(p); }
		else if (y > UHEAD && TabCount(g_page) && py >= UCY && py < UCY + 36) {
			for (int t = 0; t < TabCount(g_page); t++) {
				int tx, tw; TabRect(g_page, t, tx, tw);
				if (x >= tx && x < tx + tw && t != g_tab[g_page]) { g_tab[g_page] = t; g_scroll[g_page] = 0; ShowPage(g_page); }
			}
		}
		else if (y > UHEAD && ChipClick(x, py)) return 0;
		if (g_page == pReplays && g_tab[pReplays] == 0 && x >= UCX && x < UCX + g_cw && y > UHEAD && py >= UCY + g_rlY + 38) {
			int r = (py - UCY - g_rlY - 38) / RL_ROW;
			if (r >= 0 && r < (int)g_rlistIds.size()) { g_reqReplayId = g_rlistIds[r]; Log("-- replay crash #%d (Replays page) --\n", g_rlistIds[r]); }
		}
		if (g_page == pPhone && x >= UCX && x < UCX + UCW && y > UHEAD && py >= UCY + 576 && py < UCY + 632) {
			EnterCriticalSection(&g_lock);
			std::string tail = g_remoteTail;
			LeaveCriticalSection(&g_lock);
			if (!tail.empty()) { CopyText(tail); g_tailCopiedAt = Now(); InvalidateRect(h, NULL, FALSE); }
		}
		return 0;
	}
	case WM_LBUTTONUP:
		if (g_dragBar >= 0) { g_dragBar = -1; ReleaseCapture(); InvalidateRect(h, NULL, FALSE); }
		return 0;
	case WM_CAPTURECHANGED:
		if (g_dragBar >= 0) { g_dragBar = -1; InvalidateRect(h, NULL, FALSE); }
		return 0;
	case WM_MOUSEWHEEL:
		ScrollTo(g_scroll[g_page] - GET_WHEEL_DELTA_WPARAM(w) * 60 / WHEEL_DELTA);
		return 0;
	case WM_SIZE:
		if (w != SIZE_MINIMIZED && HIWORD(l) > 0 && LOWORD(l) > 0) {
			g_uh = HIWORD(l) * 96 / g_dpi;
			g_uw = std::max(UW, (int)LOWORD(l) * 96 / g_dpi);
			g_cw = std::max(UCW, g_uw - USIDE - 2 * UPAD);
			Relayout();
			g_scroll[g_page] = std::min(g_scroll[g_page], MaxScroll(g_page));
			PlaceCtls();
			if (g_btnMove) PlaceHeader();
			InvalidateRect(h, NULL, FALSE);
		}
		return 0;
	case WM_GETMINMAXINFO: {                  // only the height changes: from UH_MIN to the tallest page
		MINMAXINFO *mm = (MINMAXINFO *)l;
		int tallest = UH;
		for (int p = 0; p < PAGES; p++) tallest = std::max(tallest, PageBottom(p));
		RECT a = { 0, 0, Px(UW), Px(UH_MIN) }, b = { 0, 0, Px(UW), Px(tallest) };
		DWORD st = (DWORD)GetWindowLong(h, GWL_STYLE);
		AdjustWindowRect(&a, st, FALSE); AdjustWindowRect(&b, st, FALSE);
		mm->ptMinTrackSize.x = a.right - a.left;
		mm->ptMaxTrackSize.x = Px(3000) + (a.right - a.left - Px(UW));
		mm->ptMinTrackSize.y = a.bottom - a.top;
		mm->ptMaxTrackSize.y = b.bottom - b.top;
		return 0;
	}
	case WM_CTLCOLOREDIT:
		SetTextColor((HDC)w, U_TEXT); SetBkColor((HDC)w, U_INPUT);
		return (LRESULT)g_inputBrush;
	case WM_CTLCOLORLISTBOX:
		return (LRESULT)g_cardBrush;
	case WM_MEASUREITEM: {
		MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)l;
		mi->itemHeight = Px(mi->CtlID == ID_LOG ? 19 : mi->CtlID == ID_FIELDLIST || mi->CtlID == ID_DIFFS ? 28 : 34);
		return TRUE;
	}
	case WM_DRAWITEM: {
		DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)l;
		if (d->CtlType == ODT_BUTTON) DrawButton(d); else DrawListRow(d);
		return TRUE;
	}
	case WM_APP_LOG: DrainLog(); return 0;
	case WM_APP_REMOTE: { RemoteReq *r = (RemoteReq *)l; RemoteOnUi(*r); delete r; return 0; }
	case WM_TIMER:
		if (w == TIMER_REFRESH) Refresh();
		if (w == TIMER_COMMIT) { KillTimer(h, TIMER_COMMIT); CommitEdit(g_pendingEdit); g_pendingEdit = -1; }
		return 0;
	case WM_COMMAND: {
		int id = LOWORD(w), code = HIWORD(w);
		if (id == IDOK) {                                       // Enter in a box: use it now
			HWND f = GetFocus();
			if (f == g_edPresetName) SavePreset();
			for (size_t i = 0; i < g_fields.size(); i++) if (g_fields[i].h == f) CommitEdit((int)i);
			return 0;
		}
		if (id >= ID_FIELD0 && id < ID_FIELD0 + (int)g_fields.size()) {
			int idx = id - ID_FIELD0;
			Field &f = g_fields[idx];
			if (f.kind == fNum || f.kind == fText || f.kind == fCams) {
				if (g_syncing) return 0;
				if (code == EN_CHANGE) { g_pendingEdit = idx; SetTimer(h, TIMER_COMMIT, 900, NULL); }
				if (code == EN_KILLFOCUS) { KillTimer(h, TIMER_COMMIT); CommitEdit(idx); }
				return 0;
			}
			if (code != BN_CLICKED) return 0;
			if (f.kind == fToggle) Commit(f, atoi(Raw(f.key).c_str()) ? "0" : "1");
			else if (f.kind == fChoice) {
				if (IsDropdown(f)) {                                // list the options in a drop-down menu under the box
					auto opts = ChoiceOpts(f);
					std::string cur = Raw(f.key);
					HMENU m = CreatePopupMenu();
					for (size_t i = 0; i < opts.size(); i++)
						AppendMenuA(m, MF_STRING | (_stricmp(cur.c_str(), opts[i].first.c_str()) == 0 ? MF_CHECKED : 0), 1 + i, opts[i].second.c_str());
					RECT wr; GetWindowRect(f.h, &wr);
					int pick = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, wr.left, wr.bottom + 2, 0, h, NULL);
					DestroyMenu(m);
					if (pick > 0 && pick <= (int)opts.size()) Commit(f, opts[pick - 1].first);
					return 0;
				}
				POINT pt; GetCursorPos(&pt); ScreenToClient(f.h, &pt);
				RECT r; GetClientRect(f.h, &r);
				std::vector<std::string> vals;
				std::string list = ChoicesOf(f) + ";";
				size_t a = 0, b;
				while ((b = list.find(';', a)) != std::string::npos) { std::string one = list.substr(a, b - a); a = b + 1; vals.push_back(one.substr(0, one.find('|'))); }
				int k = vals.empty() ? 0 : std::min((int)vals.size() - 1, std::max(0, (int)(pt.x * (int)vals.size() / std::max(1L, r.right))));
				if (!vals.empty()) Commit(f, vals[k]);
			} else if (f.kind == fColor) {
				static COLORREF custom[16];
				std::string v = Raw(f.key);
				unsigned rgb = 0; sscanf(v.c_str() + (!v.empty() && v[0] == '#'), "%x", &rgb);
				CHOOSECOLORA cc = { sizeof cc };
				cc.hwndOwner = h; cc.rgbResult = RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); cc.lpCustColors = custom;
				cc.Flags = CC_FULLOPEN | CC_RGBINIT;
				if (ChooseColorA(&cc)) {
					char hex[16]; snprintf(hex, sizeof hex, "#%02X%02X%02X", GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult));
					Commit(f, hex);
				}
			} else if (f.kind == fAction && f.key == "move") g_reqEdit = true;
			else if (f.kind == fAction && f.key == "replay_now") g_reqReplay = true;
			else if (f.kind == fAction && f.key == "replay_live") g_reqLive = true;
			else if (f.kind == fAction && f.key == "replay_reel") g_reqReel = true;
			else if (f.kind == fAction && f.key == "tp_start") TpQueue("start");
			else if (f.kind == fAction && f.key == "tp_back") TpQueue("step", -1);
			else if (f.kind == fAction && f.key == "tp_fwd") TpQueue("step", 1);
			else if (f.kind == fAction && f.key == "tp_play") TpQueue("toggle");
			else if (f.kind == fAction && f.key == "tp_slower") TpQueue("slower");
			else if (f.kind == fAction && f.key == "tp_faster") TpQueue("faster");
			else if (f.kind == fAction && f.key == "cam_map") g_reqCamMap = true;
			else if (f.kind == fAction && f.key == "director_view") {
				std::string port = Raw("remote_port"), key = Raw("remote_key");
				if (Raw("remote") == "0") MessageBoxA(h, "The director view comes from the phone remote's web server. Switch the phone remote on (Phone page) first.", "RaceVision", MB_OK | MB_ICONINFORMATION);
				else ShellExecuteA(NULL, "open", ("http://127.0.0.1:" + (port.empty() ? std::string("8090") : port) + "/director?k=" + key).c_str(), NULL, NULL, SW_SHOWNORMAL);
			}
			else if (f.kind == fAction && f.key == "layout_editor") {
				std::string port = Raw("remote_port"), key = Raw("remote_key");
				if (Raw("remote") == "0") MessageBoxA(h, "The layout editor comes from the phone remote's web server. Switch the phone remote on (Phone page) first.", "RaceVision", MB_OK | MB_ICONINFORMATION);
				else ShellExecuteA(NULL, "open", ("http://127.0.0.1:" + (port.empty() ? std::string("8090") : port) + "/overlays?k=" + key).c_str(), NULL, NULL, SW_SHOWNORMAL);
			}
			else if (f.kind == fAction && f.key == "remote_newkey") {
				if (MessageBoxA(h, "Make a new pairing code? Phones that scanned the old one will have to scan the new one.",
				                "RaceVision", MB_YESNO | MB_ICONQUESTION) == IDYES)
					NewRemoteKey();
			}
			return 0;
		}
		switch (id) {
		case ID_PAUSE:
			EnterCriticalSection(&g_lock);
			g_paused = !g_paused;
			Log(g_paused ? "-- paused --\n" : "-- directing --\n");
			LeaveCriticalSection(&g_lock);
			Refresh();
			return 0;
		case ID_OVERLAYS: {
			Field f; f.page = pOverlays; f.key = "overlays"; f.kind = fToggle;
			Commit(f, g_snap.overlays ? "0" : "1");
			for (auto &o : g_fields) if (o.key == "overlays" && o.h) InvalidateRect(o.h, NULL, FALSE);
			return 0;
		}
		case ID_MOVE: g_reqEdit = true; return 0;
		case ID_COPY: CopyLog(); return 0;
		case ID_OPENLOG: ShellExecuteA(h, "open", g_logPath.c_str(), NULL, NULL, SW_SHOWNORMAL); return 0;
		case ID_FIELDLIST:
			if (code == LBN_DBLCLK) {
				int i = (int)SendMessage(g_lbField, LB_GETCURSEL, 0, 0);
				if (i >= 0 && i < (int)g_snap.rows.size()) g_reqFocus = g_snap.rows[i].slot;
			}
			return 0;
		case ID_SERIES:
			if (code == LBN_SELCHANGE) {
				int i = (int)SendMessage(g_lbSeries, LB_GETCURSEL, 0, 0);
				if (i >= 0 && i < (int)g_seriesList.size()) {
					Field f; f.page = pOverlays; f.key = "art_series"; f.kind = fText;   // overlays page: the preview's art
					Commit(f, g_seriesList[i].id);
				}
			}
			return 0;
		case ID_PRESETS:
			if (code == LBN_SELCHANGE) {
				FillDiffs();
				int i = SelectedPreset();
				if (i >= 0 && !g_presets[i].builtin) SetWindowTextA(g_edPresetName, g_presets[i].name.c_str());
			}
			if (code == LBN_DBLCLK) LoadPreset();
			return 0;
		case ID_SAVEPRESET: SavePreset(); return 0;
		case ID_LOADPRESET: LoadPreset(); return 0;
		case ID_DELPRESET: DeletePreset(); return 0;
		case ID_PRESETFOLDER:
			CreateDirectoryA(PresetDir().c_str(), NULL);
			ShellExecuteA(h, "open", PresetDir().c_str(), NULL, NULL, SW_SHOWNORMAL);
			return 0;
		case ID_ADDFOLDER: AddFolder(); return 0;
		case ID_REMOVEFOLDER: {
			int i = (int)SendMessage(g_lbFolders, LB_GETCURSEL, 0, 0);
			if (i >= 0 && i < (int)g_folderList.size()) { g_folderList.erase(g_folderList.begin() + i); SaveFolders(); }
			return 0;
		}
		}
		break;
	}
	case WM_CLOSE: {
		RECT r; GetWindowRect(h, &r);
		if (!IsIconic(h)) {
			char v[64]; snprintf(v, sizeof v, "%d, %d, %d, %d", r.left, r.top, g_uh, g_uw);
			EnterCriticalSection(&g_lock); SaveKey("window_pos", v); LeaveCriticalSection(&g_lock);
		}
		DestroyWindow(h);
		return 0;
	}
	case WM_DESTROY: PostQuitMessage(0); return 0;
	}
	return DefWindowProc(h, m, w, l);
}

// the RaceVision logo (racevision_logo.png, built in as RCDATA 102): the window header and the phone page
static Bitmap *LogoBitmap() {
	static Bitmap *logo = NULL;
	static bool tried = false;
	EnterCriticalSection(&g_lock);
	if (!tried) {
		tried = true;
		HRSRC r = FindResourceA(NULL, MAKEINTRESOURCEA(102), (LPCSTR)RT_RCDATA);
		DWORD n = r ? SizeofResource(NULL, r) : 0;
		HGLOBAL mem = n ? GlobalAlloc(GMEM_MOVEABLE, n) : NULL;
		IStream *st = NULL;
		if (mem) {
			memcpy(GlobalLock(mem), LockResource(LoadResource(NULL, r)), n);
			GlobalUnlock(mem);
			if (SUCCEEDED(CreateStreamOnHGlobal(mem, TRUE, &st))) {       // the stream lives as long as the bitmap
				logo = new Bitmap(st);
				if (logo->GetLastStatus() != Ok) { delete logo; logo = NULL; }
			}
		}
	}
	LeaveCriticalSection(&g_lock);
	return logo;
}
// the logo at one size, smoothly scaled (made once per size)
static Bitmap *LogoAt(int size) {
	static std::map<int, Bitmap *> made;
	EnterCriticalSection(&g_lock);
	Bitmap *&b = made[size];
	Bitmap *src = LogoBitmap();
	if (!b && src) {
		b = new Bitmap(size, size, PixelFormat32bppPARGB);
		Graphics gr(b);
		gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
		gr.SetPixelOffsetMode(PixelOffsetModeHighQuality);
		gr.DrawImage(src, 0, 0, size, size);
	}
	LeaveCriticalSection(&g_lock);
	return b;
}
static HICON AppIcon(int size) {
	return (HICON)LoadImageA(GetModuleHandle(NULL), MAKEINTRESOURCEA(1), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
}

static void CreateUi() {
	HDC sdc = GetDC(NULL); g_dpi = GetDeviceCaps(sdc, LOGPIXELSX); ReleaseDC(NULL, sdc);
	auto font = [](const char *face, double px) {
		return CreateFontA(-Px(px), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
		                   CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
	};
	fTitle = font("Bahnschrift SemiBold Condensed", 32); fH1 = font("Bahnschrift SemiBold", 26);
	fNav = font("Segoe UI Semibold", 15); fLabel = font("Segoe UI Semibold", 14); fBody = font("Segoe UI", 14);
	fSmall = font("Segoe UI", 12.5); fSection = font("Bahnschrift SemiBold", 12.5); fBig = font("Bahnschrift SemiBold", 21);
	fMono = font("Consolas", 13); fInput = font("Segoe UI", 14);
	g_mdc = CreateCompatibleDC(NULL);
	g_inputBrush = CreateSolidBrush(U_INPUT); g_bgBrush = CreateSolidBrush(U_BG); g_cardBrush = CreateSolidBrush(U_CARD);

	WNDCLASSA wc = {};
	wc.lpfnWndProc = UiProc; wc.hInstance = GetModuleHandle(NULL); wc.lpszClassName = "RaceVisionWindow";
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hIcon = AppIcon(GetSystemMetrics(SM_CXICON));
	RegisterClassA(&wc);
	WNDCLASSA bc = {};
	bc.lpfnWndProc = BtnProc; bc.hInstance = GetModuleHandle(NULL); bc.lpszClassName = "RVButton";
	bc.hCursor = LoadCursor(NULL, IDC_HAND);
	RegisterClassA(&bc);
	WNDCLASSA pc = {};
	pc.lpfnWndProc = PickProc; pc.hInstance = GetModuleHandle(NULL); pc.lpszClassName = "RVPicker";
	pc.hCursor = LoadCursor(NULL, IDC_ARROW);
	RegisterClassA(&pc);
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN;
	int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
	EnterCriticalSection(&g_lock);
	std::string pos = Raw("window_pos");
	LeaveCriticalSection(&g_lock);
	int px, py, ph = UH, pw = UW;
	POINT at = { 0, 0 };
	if (sscanf(pos.c_str(), "%d , %d , %d , %d", &px, &py, &ph, &pw) >= 2) {
		RECT want = { px, py, px + 200, py + 60 };
		if (MonitorFromRect(&want, MONITOR_DEFAULTTONULL)) { x = px; y = py; at = { px, py }; }
	}
	// as tall as last time, but never taller than the screen (pages that don't fit scroll)
	g_uh = std::max(UH_MIN, ph);
	MONITORINFO mi = { sizeof mi };
	if (GetMonitorInfo(MonitorFromPoint(at, MONITOR_DEFAULTTOPRIMARY), &mi)) {
		RECT frame = { 0, 0, 0, 0 };
		AdjustWindowRect(&frame, style, FALSE);
		int room = (mi.rcWork.bottom - mi.rcWork.top) - (frame.bottom - frame.top);
		g_uh = std::max(UH_MIN, std::min(g_uh, (int)(room * 96 / g_dpi)));
		if (y != CW_USEDEFAULT && y + Px(g_uh) + (frame.bottom - frame.top) > mi.rcWork.bottom)
			y = std::max((int)mi.rcWork.top, (int)((int)mi.rcWork.bottom - Px(g_uh) - (frame.bottom - frame.top)));
	}
	g_uw = std::max(UW, std::min(pw, 3000));
	if (GetMonitorInfo(MonitorFromPoint(at, MONITOR_DEFAULTTOPRIMARY), &mi)) g_uw = std::max(UW, std::min(g_uw, (int)((mi.rcWork.right - mi.rcWork.left) * 96 / g_dpi)));
	g_cw = std::max(UCW, g_uw - USIDE - 2 * UPAD);
	RECT r = { 0, 0, Px(g_uw), Px(g_uh) };
	AdjustWindowRect(&r, style, FALSE);
	g_ui = CreateWindowExA(0, "RaceVisionWindow", "RaceVision", style, x, y, r.right - r.left, r.bottom - r.top, NULL, NULL,
	                       GetModuleHandle(NULL), NULL);
	SendMessage(g_ui, WM_SETICON, ICON_SMALL, (LPARAM)AppIcon(GetSystemMetrics(SM_CXSMICON)));
	BOOL dark = TRUE;
	DwmSetWindowAttribute(g_ui, 20, &dark, sizeof dark);         // DWMWA_USE_IMMERSIVE_DARK_MODE: dark title bar

	// header buttons (every page)
	int bx = g_uw - 24;
	bx -= 140; g_btnMove = MakeButton(ID_MOVE, -1, bx, 14, 140, 36, "Move overlays");
	bx -= 150; g_btnOverlays = MakeButton(ID_OVERLAYS, -1, bx, 14, 140, 36, "Overlays on");
	bx -= 170; g_btnPause = MakeButton(ID_PAUSE, -1, bx, 14, 160, 36, "Pause directing");
	// live
	g_lbField = MakeList(ID_FIELDLIST, pLive, UCX + 8, UCY + 134, 284, 418);
	g_lbLog = MakeList(ID_LOG, pLive, UCX + 324, UCY + 134, UCW - 332, 418);
	g_btnOpenLog = MakeButton(ID_OPENLOG, pLive, UCX + UCW - 16 - 120, UCY + 104, 120, 28, "Open log file");
	g_btnCopy = MakeButton(ID_COPY, pLive, UCX + UCW - 16 - 120 - 8 - 90, UCY + 104, 90, 28, "Copy log");
	// art
	g_lbFolders = MakeList(ID_FOLDERS, pArt, UCX + 8, UCY + 64, UCW - 16, 130);
	g_btnAdd = MakeButton(ID_ADDFOLDER, pArt, UCX + 16, UCY + 202, 130, 32, "Add folder...");
	g_btnRemove = MakeButton(ID_REMOVEFOLDER, pArt, UCX + 156, UCY + 202, 110, 32, "Remove");
	g_lbSeries = MakeList(ID_SERIES, pArt, UCX + 8, UCY + 296, 330, 254);
	// presets
	g_lbPresets = MakeList(ID_PRESETS, pPresets, UCX + 8, UCY + 34, 388, 400);
	MakeButton(ID_LOADPRESET, pPresets, UCX + 16, UCY + 446, 100, 32, "Load");
	MakeButton(ID_DELPRESET, pPresets, UCX + 126, UCY + 446, 100, 32, "Delete");
	MakeButton(ID_PRESETFOLDER, pPresets, UCX + 236, UCY + 446, 152, 32, "Open presets folder");
	g_edPresetName = MakeCtl("EDIT", ES_AUTOHSCROLL | WS_TABSTOP, ID_PRESETNAME, pPresets, UCX + 445, UCY + 71, 236, 19);
	SendMessage(g_edPresetName, EM_SETLIMITTEXT, 60, 0);
	MakeButton(ID_SAVEPRESET, pPresets, UCX + 700, UCY + 64, 128, 32, "Save preset");
	g_lbDiffs = MakeList(ID_DIFFS, pPresets, UCX + 428, UCY + 252, 408, 236);
	// settings
	EnterCriticalSection(&g_lock);
	g_artWas = S.artFolders + "|" + S.artSeries;
	FlowLayout();
	for (size_t i = 0; i < g_fields.size(); i++) {
		Field &f = g_fields[i];
		if (IsPick(f)) continue;                                  // camera chips are drawn and clicked by the window itself
		RECT b = FieldBox(f);
		int id = ID_FIELD0 + (int)i;
		if (f.kind == fNum || f.kind == fText) {
			std::string value = Raw(f.key);
			if (value.empty() && f.key == "qual_handoff_m") value = "300";
			if (value.empty() && f.key == "qual_close_ms") value = "150";
			if (value.empty() && f.key == "qual_finish_pct") value = "30";
			if (value.empty() && f.key == "qual_break_hold") value = "20";
			f.h = MakeCtl("EDIT", ES_AUTOHSCROLL | WS_TABSTOP, id, f.page, b.left + 9, b.top + 7, b.right - b.left - 18 - (f.unit.empty() ? 0 : 34), 19, value.c_str());
		} else {
			f.h = MakeButton(id, f.page, b.left, b.top, b.right - b.left, b.bottom - b.top, f.kind == fAction ? f.choices.c_str() : "");
		}
		f.ctl = (int)g_ctls.size() - 1;
		g_ctls[f.ctl].tab = f.tab;
	}
	LeaveCriticalSection(&g_lock);
	TakeSnap();
	Refresh();
	ShowPage(pLive);
	ShowWindow(g_ui, SW_SHOWNORMAL);
	SetTimer(g_ui, TIMER_REFRESH, 400, NULL);
	DrainLog();
}

static DWORD WINAPI UiThread(LPVOID ready) {
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	AddFields();
	CreateUi();
	SetEvent((HANDLE)ready);
	MSG m;
	while (GetMessage(&m, NULL, 0, 0) > 0)
		if (!IsDialogMessage(g_ui, &m)) { TranslateMessage(&m); DispatchMessage(&m); }
	g_quit = true;
	CoUninitialize();
	return 0;
}

static HANDLE StartUi() {
	HANDLE ready = CreateEvent(NULL, TRUE, FALSE, NULL);
	HANDLE t = CreateThread(NULL, 0, UiThread, ready, 0, NULL);
	WaitForSingleObject(ready, 5000);
	CloseHandle(ready);
	return t;
}
