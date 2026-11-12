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
	U_INPUT = RGB(12, 14, 18), U_LINE = RGB(50, 55, 66), U_TEXT = RGB(236, 239, 244), U_MUTED = RGB(134, 143, 160),
	U_GOLD = RGB(255, 196, 0), U_GREEN = RGB(70, 200, 120), U_RED = RGB(235, 85, 75), U_TEAL = RGB(60, 200, 200);
static const int UW = 1100, UH = 780, UH_MIN = 640, UHEAD = 64, USIDE = 200, UPAD = 28, UCX = USIDE + UPAD, UCW = UW - USIDE - 2 * UPAD,
	UCY = 150, UCARD_H = 68;

enum Page { pLive, pDirector, pBattles, pReplays, pCameras, pOverlays, pArt, pPresets, pPhone, PAGES };
static const char *PageName[] = { "Live", "Director", "Battles", "Replays", "Cameras", "Overlays", "Art", "Presets", "Phone" };
static const char *PageSub[] = {
	"What the director is doing right now. Double-click a car to put it on TV.",
	"How long shots last and when the director cuts to something better.",
	"What counts as a battle, and how starts and restarts are handled.",
	"Crashes: how they are found, when their replays roll, how they are shot, and every crash this race.",
	"The cameras each kind of shot opens on, and the ones it follows with after that.",
	"Broadcast graphics drawn over the game (NR2003 in windowed or borderless mode).",
	"Number cards, headshots and driver colours, from series art folders.",
	"Save your settings as a preset of your own, and switch between presets in one click.",
	"Run RaceVision from your phone: pick cars and cameras and change settings from its browser.",
};
static int g_page = pLive;
// Pages taller than the window scroll (mouse wheel, or drag the bar on the right). The window can also be made
// taller or shorter (it opens to fit the screen). g_oy = the scroll of the page being painted: the page drawing
// helpers take it off every y, so page code keeps its plain layout numbers.
static int g_uh = UH;                       // window height now (96-dpi)
static int g_scroll[PAGES] = {}, g_oy = 0;
// how far each page's own drawing goes down (from UCY); fields and controls are measured
static const int PAGE_END[PAGES] = { 604, 0, 0, 1720, 558, 792, 558, 552, 604 };

static HFONT fTitle, fH1, fNav, fLabel, fBody, fSmall, fSection, fBig, fMono, fInput;
static HBRUSH g_inputBrush, g_bgBrush, g_cardBrush;

// ---- settings on the pages
enum FieldKind { fNum, fToggle, fText, fColor, fChoice, fAction, fCams };
struct Field {
	int page; std::string key, label, help; FieldKind kind;
	int x, y, w;                 // card, from the content's top left (fCams: the input box)
	int cw = 0;                  // control width (0 = the kind's own)
	std::string choices;         // fChoice "value|Label;..."; fAction the button text
	HWND h = NULL;
	bool bad = false;            // not a number
};
static std::vector<Field> g_fields;
struct Section { int page, y; const char *text; };
static const Section SECTIONS[] = {
	{ pDirector, 0, "SHOTS" }, { pDirector, 262, "WHEN YOU TAKE OVER" },
	{ pBattles, 0, "BATTLES" }, { pBattles, 186, "START AND RESTARTS" }, { pBattles, 362, "LEAD BATTLES" }, 
	{ pReplays, 0, "FINDING CRASHES" }, { pReplays, 264, "AUTOMATIC REPLAYS" }, { pReplays, 528, "HOW A REPLAY IS SHOT" },
	{ pReplays, 792, "PLAY" }, { pReplays, 980, "CRASHES AND HIGHLIGHTS THIS RACE" }, { pReplays, 1722, "SLOW MOTION, WIPE AND PLAYBACK" },
	{ pReplays, 2280, "OTHER HIGHLIGHTS" },
	{ pCameras, 0, "CAMERAS PER SHOT" }, { pCameras, 574, "CAMERA CHANGES" }, { pCameras, 762, "OWN ANGLES (TEST)" }, { pCameras, 1482, "DRONE" }, { pCameras, 1680, "DYNAMIC CAMERAS (TEST)" },
	{ pOverlays, 0, "GRAPHICS" }, { pOverlays, 186, "WHEN THEY SHOW" }, { pOverlays, 602, "PREVIEW (SAMPLE CAR)" },
	{ pArt, 0, "ART FOLDERS" }, { pArt, 262, "SERIES" },
	{ pPresets, 0, "PRESETS" },
	{ pPhone, 0, "PAIR A PHONE" },
};
struct CamRow { const char *label, *key; bool follow; };
static const CamRow CAM_ROWS[] = {
	{ "Crash", "incident", true }, { "Pass", "pass", true }, { "Battle", "battle", true }, { "Leader", "leader", true },
	{ "Running order tour", "tour", true }, { "Restart", "restart", true }, { "Pit road in", "pit", false },
	{ "In the pit stall", "pit_stall", false }, { "Pit road out", "pit_exit", false },
	{ "Qualifying: rolling off", "qual_exit", false }, { "Qualifying run", "qual", true }, { "Crash replay", "replay", false },
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
	auto F = [](int page, const char *key, const char *label, const char *help, FieldKind kind, int x, int y, int w = 414,
	            int cw = 0, const char *choices = "") {
		Field f; f.page = page; f.key = key; f.label = label; f.help = help; f.kind = kind; f.x = x; f.y = y; f.w = w;
		f.cw = cw; f.choices = choices; g_fields.push_back(f);
	};
	F(pDirector, "min_hold", "Shortest shot (s)", "A shot stays at least this long, unless a crash happens.", fNum, 0, 26);
	F(pDirector, "max_hold", "Long shot (s)", "After this a shot counts half, so something fresh can take over.", fNum, 430, 26);
	F(pDirector, "switch_margin", "Cut margin", "A new shot must score this many times the current one.", fNum, 0, 102);
	F(pDirector, "revisit_after", "Revisit after (s)", "A car just shown counts half for this long, so the field gets seen.", fNum, 430, 102);
	F(pDirector, "onboard_hold", "On-board shot (s)", "In-car and roof shots stay at least this long.", fNum, 0, 178);
	F(pDirector, "tour_depth", "Running order tour", "With nothing going on, the tour visits the top this many cars.", fNum, 430, 178);
	F(pDirector, "manual_hold", "Hands off after your pick (s)", "When you pick a car (V key or the Live page) the director waits this long.", fNum, 0, 288);
	F(pDirector, "live_after_climb_out", "Back to live when you climb out", "Getting out of your car starts the game's replay; skip it so the broadcast carries on.", fToggle, 430, 288);
	F(pDirector, "director_view", "Director view", "A live map of the cameras and a timeline of every cut, in your browser (also on the phone).", fAction, 430, 364, 414, 190, "Open director view");
	F(pDirector, "qual_handoff_m", "Qualifying handoff before stripe (m)", "Switch to the next runner this far before the line. Race Control's timing box leads when it runs.", fNum, 0, 364);

	F(pBattles, "battle_gap", "Battle gap (s)", "Two cars this close (seconds) are a battle.", fNum, 0, 26);
	F(pBattles, "battle_min_age", "Battle must last (s)", "A battle is worth a cut once it has lasted this long.", fNum, 430, 26);
	F(pBattles, "short_track_m", "Short track under (m)", "On shorter tracks a battle means at the bumper or alongside.", fNum, 0, 102);
	F(pBattles, "battle_short_m", "Short track battle (m)", "How close counts as a battle on a short track (metres).", fNum, 430, 102);
	F(pBattles, "settle_laps", "Settle laps", "After a green the field is bunched up for this many laps...", fNum, 0, 212);
	F(pBattles, "settle_positions", "Settle positions", "...when only battles up to this position count.", fNum, 430, 212);
	F(pBattles, "settle_hold", "Settle shot length (s)", "Shots stay at least this long while the field settles.", fNum, 0, 288);
	F(pBattles, "lead_battle_depth", "Lead battle: positions", "A battle for this position or better doesn't fade and holds the camera (0 = off).", fNum, 0, 388);
	F(pBattles, "lead_battle_hold", "Lead battle: stay up to (s)", "Only a crash, a restart or something far bigger cuts away before this.", fNum, 430, 388);
	F(pBattles, "lead_battle_pack", "Pack: cars within 1.5 s", "This many cars near the leader (superspeedway draft): lead battles directed as before.", fNum, 0, 464);
	F(pReplays, "crash_drop", "Crash speed loss (m/s)", "Without damage read: losing this much speed quickly is a crash or spin (31 m/s = 70 mph).", fNum, 0, 102);
	F(pReplays, "crash_window", "Within (s)", "...within this many seconds of game time.", fNum, 430, 102);
	F(pReplays, "incident_hold", "Crash stays news (s)", "A crash stays a candidate shot this long.", fNum, 0, 178);
	F(pReplays, "replay_auto", "Replay crashes by themselves", "Roll the replay of the crash most worth it once it is over (you out of your car).", fToggle, 0, 290);
	F(pReplays, "replay_caution_only", "Only under caution", "Automatic replays wait for the yellow; the buttons work any time.", fToggle, 430, 290);
	F(pReplays, "replay_countdown", "Countdown before it rolls (s)", "The crash alert counts down this long first.", fNum, 0, 366);
	F(pReplays, "replay_worthy", "Worth a replay from", "Crash score: its damage + 0.5 for every car past the first.", fNum, 430, 366);
	F(pReplays, "replay_after", "Wait after the crash (s)", "The crash must be over this long before its replay rolls.", fNum, 0, 442);
	F(pReplays, "replay_within", "Too old after (s)", "A crash older than this (game seconds) isn't replayed by itself.", fNum, 430, 442);
	F(pReplays, "replay_preroll", "Start before the hit (s)", "Each angle starts this long before its car's hardest hit.", fNum, 0, 554);
	F(pReplays, "replay_max", "Longest angle (s)", "No angle runs longer than this.", fNum, 430, 554);
	F(pReplays, "replay_angles", "Angles of a multi-car crash", "One per car hit hardest; the next one comes once the car on screen has stopped.", fNum, 0, 630);
	F(pReplays, "replay_take", "Each later angle (s)", "At most this long after its own hit (sooner if its car stops).", fNum, 430, 630);
	F(pReplays, "crash_damage", "A crash from damage", "One contact adding this much damage is a crash (rubs add 0.001 - 0.03).", fNum, 0, 26);
	F(pReplays, "contact_join", "In it from damage", "A contact this big near a crash going on puts that car in it.", fNum, 430, 26);
	F(pReplays, "cam_replay_angles", "Cameras for later angles", "After the Crash replay cameras (Cameras page). They must show the car: TV, chase, helicopter.", fText, 0, 706, 544, 150);
	F(pReplays, "reel_angles", "Angles in the reel", "Per crash in the highlight reel.", fNum, 560, 706, 284, 64);
	F(pReplays, "replay_now", "Replay the best crash", "The one most worth showing lately (again: back to live).", fAction, 0, 818, 414, 150, "Replay crash");
	F(pReplays, "replay_live", "Back to live", "End a replay or the reel now.", fAction, 430, 818, 414, 150, "Back to live");
	F(pReplays, "replay_drive", "Watch while driving: pause", "Ctrl+F7 or a replay button while in your car pauses the race, plays the replay, then resumes.", fToggle, 430, 894);
	F(pReplays, "replay_smart", "Smart slow motion", "A big crash gets one slow-motion look at its hardest hit (the game's own slow motion), back to full speed right after it. Passes never do.", fToggle, 0, 1748);
	F(pReplays, "replay_slow_impact", "Slow motion from a hit of", "Only a crash whose hardest single hit is this big gets a slow-motion look: one, of that hit, from the first replay camera, after the real-speed angles.", fNum, 430, 2610);
	F(pReplays, "replay_slow_max", "Slowest speed (1 - 16)", "A big hit gets this slow: 1 = 1/2 speed ... 16 = 1/17. Smaller hits stay gentler.", fNum, 430, 1748);
	F(pReplays, "replay_transition", "Replay wipe", "A full-screen TV wipe into a replay, between its angles and back to live.", fToggle, 0, 1824);
	F(pReplays, "replay_transition_ms", "Wipe length (ms)", "How long the whole wipe takes: 400 - 5000. 1500 is the default; higher is slower.", fNum, 430, 1824);
	F(pReplays, "replay_wipe_color", "Replay wipe colour", "The wipe's own colour. It has nothing to do with the dash accent, so the dash boxes keep each driver's art colour.", fColor, 0, 1900, 414, 120);
	F(pReplays, "hide_companion_overlays", "Hide other apps' overlays in replays", "While a replay or its wipe is on, other programs' overlay windows (tickers, track maps ...) stay out of sight until you are back to live. RaceVision's own overlays stay.", fToggle, 430, 1900);
	F(pReplays, "companion_apps", "Only these programs", "Blank = every other program's overlays. Or part of an exe name or path; ; between several.", fText, 0, 1976, 414, 120);
	F(pReplays, "companion_keep", "Never hide these", "Programs whose windows always stay (part of an exe name or path; ; between several).", fText, 430, 1976, 414, 120);
	F(pReplays, "tp_start", "Playback: to the start", "Jump to the beginning of the tape (your replay keys do the same).", fAction, 0, 2052, 414, 150, "To the start");
	F(pReplays, "tp_back", "Playback: one frame back", "Step the tape back one frame (it pauses there).", fAction, 430, 2052, 414, 150, "Frame back");
	F(pReplays, "tp_play", "Playback: play / pause", "Start the tape from live if no replay is running (you out of your car).", fAction, 0, 2128, 414, 150, "Play / pause");
	F(pReplays, "tp_fwd", "Playback: one frame forward", "Step the tape forward one frame.", fAction, 430, 2128, 414, 150, "Frame forward");
	F(pReplays, "tp_slower", "Playback: slower", "One step slower: 1/2, 1/3 ... 1/17.", fAction, 0, 2204, 414, 150, "Slower");
	F(pReplays, "tp_faster", "Playback: faster", "One step faster: back to normal, then x2, x4.", fAction, 430, 2204, 414, 150, "Faster");
	F(pReplays, "replay_reel", "Highlight reel", "Every crash worth a replay this race, and the other highlights, in race order.", fAction, 0, 894, 414, 150, "Highlight reel");
	F(pReplays, "highlights", "Find other highlights", "Passes for the front, one car by several in a row, bump and runs and the finish go on the list below, the phone and the reel.", fToggle, 0, 2306);
	F(pReplays, "highlight_gap", "At most one pass every (s)", "Passes, multi-passes and bump and runs: the better one in this many game seconds stays. The last lap is free.", fNum, 430, 2306);
	F(pReplays, "multi_pass_cars", "Multi-pass: cars", "One car by this many cars in one corner or one straight. Last-lap passes for the podium count too.", fNum, 0, 2382);
	F(pReplays, "superspeedways", "Superspeedways", "Track folder names (part of one; ; between). Multi-passes there count only on the last lap.", fText, 430, 2382, 414, 120);
	F(pReplays, "highlight_bump", "Bump and runs", "The nose of the car behind hits the tail of the one ahead (no crash) and it gets by.", fToggle, 0, 2458);
	F(pReplays, "bump_run_window", "Bump and run: by within (s)", "It gets by within this many seconds of the hit.", fNum, 0, 2686);
	F(pReplays, "bump_run_depth", "Bump and run: for P1 to", "Only for this position or better.", fNum, 430, 2686);
	F(pReplays, "highlight_preroll", "Start before the moment (s)", "Highlight replays start this long before the pass or the bump.", fNum, 430, 2458);
	F(pReplays, "replay_finish", "Replay the finish", "Once the winner and the runner-up are across the line, the finish plays by itself (not while you drive).", fToggle, 0, 2534);
	F(pReplays, "replay_finish_after", "...after (s)", "How long after the runner-up crosses the line.", fNum, 430, 2534);
	F(pReplays, "reel_highlights", "Highlights in the reel", "The highlight reel plays these with the crashes. Off = crashes only.", fToggle, 0, 2610);

	for (int r = 0; r < (int)(sizeof CAM_ROWS / sizeof CAM_ROWS[0]); r++) {
		F(pCameras, (std::string("cam_") + CAM_ROWS[r].key).c_str(), "", "", fCams, 180, 64 + r * 40, 0, 130);
		if (CAM_ROWS[r].follow)
			F(pCameras, (std::string("cam_") + CAM_ROWS[r].key + "_follow").c_str(), "", "", fCams, 330, 64 + r * 40, 0, 200);
	}
	F(pCameras, "camera_call", "Game camera switch", "Change cameras the way the C key does (keeps camera sounds right).", fToggle, 0, 600, 544);
	F(pCameras, "onboard_cams", "On-board cameras", "Cameras that count as on-board: they get the in-car dash.", fText, 0, 676, 544, 150);
	F(pCameras, "follow_after", "Follow after (s)", "Leave the opening camera after this long.", fNum, 560, 600, 284, 64);
	F(pCameras, "camera_every", "Next camera (s)", "Then change camera this often; 0 = never.", fNum, 560, 676, 284, 64);
	F(pCameras, "custom_cams", "Own angles on the spare cameras", "Chase 2 (6), Far chase 2 (7) and 22-40 use your own angles: set them on the phone (Cameras > Adjust).", fToggle, 0, 788, 844);
	F(pCameras, "drone_quiet", "Drone (23) without sound", "No helicopter sound on the Drone; the Helicopter (16) keeps it.", fToggle, 0, 1320);
	F(pCameras, "tv_cut_gap", "TV camera change gap (s)", "No cut this close to a TV camera's own change; 0 = off.", fNum, 430, 1320);
	F(pCameras, "tv_cut_wait", "Wait for it at most (s)", "Longer than this, the cut goes ahead anyway.", fNum, 0, 1396);
	F(pCameras, "drone_distance", "Drone trail distance (%)", "How far behind the car the Drone follows, % of the game's helicopter (50 = about half).", fNum, 0, 1508);
	F(pCameras, "drone_top_mph", "Drone top speed (mph)", "The game's 175: faster cars leave it behind until it jumps back beside them.", fNum, 430, 1508);
	F(pCameras, "drone_hover_m", "Drone holds still within (m)", "This close to the car it stops chasing (the game's 25).", fNum, 0, 1584);
	F(pCameras, "cam_map", "Camera map", "Where each TV camera is and what it shows.", fAction, 430, 1396, 414, 150, "Show camera map");
	F(pCameras, "rear_pan", "Rear (11) follows the car behind", "Turns to the nearest car behind or alongside it, with a wide lens.", fToggle, 0, 1092);
	F(pCameras, "rear_pan_span", "Turns up to (degrees)", "Either way from straight back (90 = looking out the side).", fNum, 430, 1092);
	F(pCameras, "rear_pan_fov", "Rear lens (degrees)", "Wider = more fish-eye (the game's is 78).", fNum, 0, 1168);
	F(pCameras, "rear_mount", "Rear (11) from the car ahead", "Shows the TV car from the car just ahead of it; the overlays stay on the TV car.", fToggle, 430, 1168);
	F(pCameras, "rear_mount_m", "Car ahead within (m)", "Nobody that close ahead of the TV car: the setting beside this.", fNum, 0, 1244);
	F(pCameras, "rear_alone", "Nobody ahead (camera)", "11 = its own rear cam looking at the car behind; 4 = chase.", fNum, 430, 1244);
	F(pCameras, "dyn_cams", "Dynamic cameras", "RaceVision places its own static cameras - pit road, crashes, restarts, battles, the finish - and learns the track as it goes. Off = only the game's cameras.", fToggle, 0, 1744);
	F(pCameras, "dyn_chance", "How often (%)", "Of the shots that could have one, this many get a dynamic camera; the rest use the game's cameras.", fNum, 430, 1744);
	F(pCameras, "dyn_pit", "Pit road", "Who dives in, the stall, the race off pit road.", fToggle, 0, 1820);
	F(pCameras, "dyn_crash", "Crashes", "A new angle on the wreck each time.", fToggle, 430, 1820);
	F(pCameras, "dyn_restart", "Restarts", "Low and close at the green.", fToggle, 0, 1896);
	F(pCameras, "dyn_battle", "Battles and passes", "Trackside, ahead of the cars.", fToggle, 430, 1896);
	F(pCameras, "dyn_finish", "The finish", "At the line for the last lap.", fToggle, 0, 1972);
	F(pCameras, "dyn_learn", "Learn the track", "Pit road, the width of the track, where crashes happen, which angles worked (saved per track).", fToggle, 430, 1972);
	F(pCameras, "dyn_half_width", "Track half width (m)", "0 = learn it from where the cars drive.", fNum, 0, 2048);
	F(pCameras, "dyn_yaw_fix", "Aim correction (degrees)", "Added to the turn if the camera looks off to one side of the car.", fNum, 430, 2048);
	F(pCameras, "dyn_pitch_sign", "Tilt direction (1 or -1)", "-1 if the camera looks up when it should look down.", fNum, 0, 2124);
	F(pCameras, "dyn_rig", "Smooth following", "Its own fast thread follows the car, rebuilt between the game's physics ticks (off = the old jumpy way).", fToggle, 430, 2124);
	F(pCameras, "dyn_autozoom", "Zoom with the car", "The lens widens as the car comes close and narrows as it goes: the car keeps its size.", fToggle, 0, 2200);
	F(pCameras, "dyn_smooth", "Pan smoothing", "1 = standard. Lower is quicker and tighter, higher is softer.", fNum, 430, 2200);

	F(pOverlays, "overlays", "Overlays", "Show the broadcast graphics. Ctrl+F9 switches them on and off.", fToggle, 0, 26);
	F(pOverlays, "overlay_style", "Style", "Dash: TV-style in-car dash. Classic: plain tag and gauges.", fChoice, 430, 26, 414, 170, "dash|Dash;classic|Classic");
	F(pOverlays, "dash_accent", "Dash accent colour", "Border and position colour of the dash look.", fColor, 0, 102, 414, 120);
	F(pOverlays, "dash_speed_max", "Speedo top (mph)", "Speed at the end of the dash's speed arc.", fNum, 430, 102);
	F(pOverlays, "overlay_min_run_laps", "Laps into a run", "The driver tag and battle box wait this many green laps.", fNum, 0, 212);
	F(pOverlays, "overlay_gap", "Time between tags (s)", "Tag / battle box at most this often. The dash shows on every in-car shot.", fNum, 430, 212);
	F(pOverlays, "tag_story", "Race story line", "Start vs now, restarts, stages, last stop, laps led under the tag.", fToggle, 0, 288);
	F(pOverlays, "tag_rotate", "Story changes every (s)", "How long each race story fact stays up.", fNum, 430, 288);
	F(pOverlays, "replay_banner", "Replay banner", "REPLAY, the car, the angle and the time back to live, all through a replay (even with overlays off).", fToggle, 0, 440);
	F(pOverlays, "replay_overlays", "Overlays in replays", "The driver tag on the replay's car; the dash when the angle is in-car.", fToggle, 430, 440);
	F(pOverlays, "stage_laps", "Stage end laps", "Laps the stages end on, e.g. 60, 120 (empty = none).", fText, 0, 364, 414, 150);
	F(pOverlays, "move", "Place on screen", "Drag the panels to move them, mouse wheel to resize.", fAction, 430, 364, 414, 150, "Move overlays");
	F(pOverlays, "dash_accent2", "Dash plate fade colour", "The driver plate fades from the accent into this. Pick the accent again for a solid plate.", fColor, 0, 516, 414, 120);
	F(pOverlays, "dash_art_colors", "Driver colours from the art", "Each car's dash in its own colour (the art's colors.json). Cars without one keep the accent.", fToggle, 430, 516);

	F(pPhone, "remote", "Phone remote", "Let phones on this network run RaceVision from their browser.", fToggle, 320, 26, 524);
	F(pPhone, "remote_port", "Port", "The network port the phone page uses (8090 unless something else has it).", fNum, 320, 102, 524);
	F(pPhone, "remote_newkey", "Pairing code", "A new code locks out every phone that scanned the old one.", fAction, 320, 178, 524, 150, "New code");
	F(pPhone, "remote_scanner", "Scanner on the phone", "Phones can play AIMOD's team radio scanner (AIMOD running on this PC with its scanner on).", fToggle, 320, 254, 524);
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
struct Ctl { HWND h; int page, x, y, w, hgt; bool clipped = false; };   // where it sits on its page (96-dpi, unscrolled)
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

// where a field's control sits (96-dpi window coordinates)
static RECT FieldBox(const Field &f) {
	int X = UCX + f.x, Y = UCY + f.y;
	if (f.kind == fCams) return { X, Y, X + f.cw, Y + 32 };
	int cw = f.cw ? f.cw : f.kind == fToggle ? 46 : f.kind == fNum ? 84 : 150;
	int ch = f.kind == fToggle ? 24 : 32;
	int bx = X + f.w - 16 - cw, by = Y + (UCARD_H - ch) / 2;
	return { bx, by, bx + cw, by + ch };
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
static std::string ShowVal(const Field &f, const std::string &v) {
	if (f.kind == fToggle) return atoi(v.c_str()) ? "on" : "off";
	if (f.kind == fChoice) {
		size_t at = f.choices.find(v + "|");
		if (at != std::string::npos) { size_t a = at + v.size() + 1; return f.choices.substr(a, f.choices.find(';', a) - a); }
	}
	return v.empty() ? "(none)" : v;
}
static std::string KeyLabel(const Field &f) {
	if (!f.label.empty()) return f.label;
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
		if (p == &g_tag) { if (dash) DrawTagDash(gr, true); else DrawTag(gr, true); }
		else if (p == &g_battle) { if (dash) DrawBattleDash(gr, true); else DrawBattle(gr, true); }
		else if (p == &g_dash) DrawDash(gr, true); else DrawOnboard(gr, true);
		g_prevName[k] = names[k];
	}
	LeaveCriticalSection(&g_lock);
	g_previewDirty = false;
}
static void PaintPreviews(HDC dc) {
	if (g_previewDirty) MakePreviews();
	float x0 = UCX, y0 = UCY + 628, total = 0, gap = 16, hmax = 0;
	for (auto *b : g_prev) if (b) { total += b->GetWidth(); hmax = std::max(hmax, (float)b->GetHeight()); }
	if (total <= 0) return;
	float k = std::min({ (UCW - 2 * gap - 24) / total, 104.f / hmax, 0.6f });
	URound(dc, x0, y0, UCW, hmax * k + 52, 10, RGB(46, 60, 74));
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

static void PaintField(HDC dc, const Field &f) {
	int X = UCX + f.x, Y = UCY + f.y;
	if (f.kind != fCams) {
		PaintCard(dc, X, Y, f.w, UCARD_H);
		RECT b = FieldBox(f);
		int textW = b.left - X - 28;
		UText(dc, f.label, X + 16, Y + 11, textW, 20, fLabel, U_TEXT);
		UText(dc, f.help, X + 16, Y + 32, textW, 34, fSmall, U_MUTED, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
	}
	if (f.kind == fNum || f.kind == fText || f.kind == fCams) {
		RECT b = FieldBox(f);
		URound(dc, b.left, b.top, b.right - b.left, b.bottom - b.top, 6, U_INPUT, f.bad ? U_RED : U_LINE);
	}
}

static std::string CamList(const std::string &v) {
	std::string out;
	for (int c : IntList(v)) out += (out.empty() ? "" : ", ") + std::string(CamName(c));
	return out.empty() ? "keeps the camera" : out;
}

// ---- the Replays page: every crash this race (best first, click one to play it) and the replay log
static const int RL_Y = 1006, RL_ROW = 34, RL_ROWS = 10, RLOG_Y = RL_Y + 52 + RL_ROWS * RL_ROW + 24;
static std::vector<int> g_rlistIds;                      // the crash under each row, as last painted
static void PaintReplays(HDC dc) {
	struct Row { int id, lap, cars; double worth, damage, ago; bool replayed, playing, worthy; std::string who; int kind; };
	std::vector<Row> rows;
	std::vector<std::string> log;
	EnterCriticalSection(&g_lock);
	for (auto &c : g_crashes)
		rows.push_back({ c.id, c.lap, (int)c.cars.size(), Worth(c), c.damage, std::max(0.0, g_simT - c.first), c.replayed,
		                 g_rp.phase && g_rp.crash == c.id, Worth(c) >= S.replayWorthy, CrashWho(c, 4), HL_CRASH });
	for (auto &c : g_highs)
		rows.push_back({ c.id, c.lap, (int)c.cars.size(), Worth(c), 0, std::max(0.0, g_simT - c.first), c.replayed,
		                 g_rp.phase && g_rp.crash == c.id, true, c.what + ":  " + CrashWho(c, 3), c.kind });
	LeaveCriticalSection(&g_lock);
	EnterCriticalSection(&g_logLock);
	log = g_replayLog;
	LeaveCriticalSection(&g_logLock);
	std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.worth > b.worth; });
	PaintCard(dc, UCX, UCY + RL_Y, UCW, 44 + RL_ROWS * RL_ROW);
	UText(dc, "LAP", UCX + 16, UCY + RL_Y + 12, 50, 16, fSection, U_MUTED);
	UText(dc, "CARS (THE REPLAY'S FIRST CAR FIRST)", UCX + 70, UCY + RL_Y + 12, 420, 16, fSection, U_MUTED);
	UText(dc, "DAMAGE", UCX + 560, UCY + RL_Y + 12, 80, 16, fSection, U_MUTED);
	UText(dc, "SCORE", UCX + 650, UCY + RL_Y + 12, 60, 16, fSection, U_MUTED);
	UText(dc, "CLICK TO PLAY", UCX + 720, UCY + RL_Y + 12, 120, 16, fSection, U_MUTED);
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
		UText(dc, who, UCX + 70, y + 7, 480, 20, fLabel, tc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
		char b[32];
		if (r.kind) snprintf(b, sizeof b, "-"); else snprintf(b, sizeof b, "%.2f", r.damage);
		UText(dc, b, UCX + 560, y + 7, 80, 20, fLabel, tc);
		snprintf(b, sizeof b, "%.1f", r.worth); UText(dc, b, UCX + 650, y + 7, 60, 20, fLabel, r.worthy ? U_GOLD : U_MUTED);
		std::string st = r.playing ? "ON NOW" : r.replayed ? "shown" : r.worthy ? "play" : "play (minor)";
		UText(dc, st, UCX + 720, y + 7, 120, 20, fLabel, r.playing ? U_GOLD : r.replayed ? U_MUTED : U_GREEN);
	}
	UText(dc, "REPLAY LOG", UCX, UCY + RLOG_Y, 400, 20, fSection, U_GOLD);
	PaintCard(dc, UCX, UCY + RLOG_Y + 26, UCW, 248);
	int n = (int)log.size(), first = std::max(0, n - 11);
	if (!n) UText(dc, "Replays and crashes worth a replay show here.", UCX + 16, UCY + RLOG_Y + 40, 600, 20, fSmall, U_MUTED);
	for (int k = first; k < n; k++)
		UText(dc, log[k], UCX + 16, UCY + RLOG_Y + 38 + (k - first) * 20, UCW - 32, 18, fSmall, U_TEXT, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void PaintPageBody(HDC dc);
static void PaintPage(HDC dc) {
	g_oy = g_scroll[g_page];
	PaintPageBody(dc);
	g_oy = 0;
}
static void PaintPageBody(HDC dc) {
	UText(dc, PageName[g_page], UCX, UHEAD + 18, 600, 34, fH1, U_TEXT);
	UText(dc, PageSub[g_page], UCX, UHEAD + 52, UCW, 20, fSmall, U_MUTED);
	for (auto &s : SECTIONS) if (s.page == g_page) UText(dc, s.text, UCX, UCY + s.y, 400, 20, fSection, U_GOLD);
	const Snap &n = g_snap;
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
		UText(dc, "FIELD", UCX + 16, UCY + 112, 200, 16, fSection, U_MUTED);
		PaintCard(dc, UCX + 316, UCY + 100, UCW - 316, 460);
		UText(dc, "LOG", UCX + 332, UCY + 112, 200, 16, fSection, U_MUTED);
		UText(dc, "Ctrl+F9 overlays   Ctrl+F10 move overlays   Ctrl+F11 pause   Ctrl+F12 quit", UCX, UCY + 578, 540, 20, fSmall, U_MUTED);
	}
	if (g_page == pCameras) {
		PaintCard(dc, UCX, UCY + 26, 544, 532);
		UText(dc, "SHOT", UCX + 16, UCY + 40, 150, 16, fSection, U_MUTED);
		UText(dc, "OPENS ON", UCX + 180, UCY + 40, 150, 16, fSection, U_MUTED);
		UText(dc, "THEN FOLLOWS WITH", UCX + 330, UCY + 40, 200, 16, fSection, U_MUTED);
		for (int r = 0; r < (int)(sizeof CAM_ROWS / sizeof CAM_ROWS[0]); r++) {
			int y = UCY + 64 + r * 40;
			UText(dc, CAM_ROWS[r].label, UCX + 16, y + 7, 160, 20, fLabel, U_TEXT);
			if (!CAM_ROWS[r].follow) UText(dc, !strcmp(CAM_ROWS[r].key, "replay") ? "first angle (more: Replays)" : "follows the car along pit road", UCX + 330, y + 8, 200, 20, fSmall, U_MUTED);
		}
		// the legend, and what the lists say in words for the row under the mouse... kept simple: all of them
		PaintCard(dc, UCX + 560, UCY + 26, 284, 532);
		UText(dc, "CAMERA NUMBERS", UCX + 576, UCY + 40, 200, 16, fSection, U_MUTED);
		for (int c = 1; c <= std::min(CamLast(), 30); c++) {
			if (c > 21 && !VCamById(c)) continue;
			int col = (c - 1) / 15, row = (c - 1) % 15;
			int x = UCX + 576 + col * 134, y = UCY + 64 + row * 30;
			bool on = IsOnboard(c);
			URound(dc, x, y, 26, 22, 5, on ? U_GOLD : U_INPUT, on ? CLR_INVALID : U_LINE);
			UText(dc, std::to_string(c), x, y + 2, 26, 18, fLabel, on ? U_INPUT : U_TEXT, DT_CENTER | DT_TOP | DT_SINGLELINE);
			UText(dc, CamName(c), x + 32, y + 3, 100, 18, fSmall, c == 9 ? U_MUTED : U_TEXT);
		}
		UText(dc, "Gold = on-board. 22 and up: RaceVision's own (phone: Cameras, Adjust).", UCX + 576, UCY + 516, 260, 36, fSmall, U_MUTED, DT_LEFT | DT_TOP | DT_WORDBREAK);
	}
	if (g_page == pOverlays) PaintPreviews(dc);
	if (g_page == pReplays) PaintReplays(dc);
	if (g_page == pArt) {
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
		PaintCard(dc, UCX, UCY + 26, 404, 470);
		UText(dc, "SAVE CURRENT SETTINGS", UCX + 420, UCY, 400, 20, fSection, U_GOLD);
		PaintCard(dc, UCX + 420, UCY + 26, 424, 124);
		UText(dc, "Preset name", UCX + 436, UCY + 38, 300, 20, fLabel, U_TEXT);
		URound(dc, UCX + 436, UCY + 64, 392, 32, 6, U_INPUT, U_LINE);
		UText(dc, "Saves the settings on every page but Art.", UCX + 436, UCY + 112, 250, 20, fSmall, U_MUTED);
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
		UText(dc, "The phone has to be on the same network (Wi-Fi) as this PC, or on Tailscale (use the PC's 100.x address). On the phone, add the page to the home screen "
		          "(Share menu on iPhone, browser menu on Android) to open it like an app: it remembers the code. If the app ever "
		          "asks, type the pairing code.  " + fw,
		      UCX, UCY + 480, UCW, 60, fSmall, U_MUTED, DT_LEFT | DT_TOP | DT_WORDBREAK);
		if (!tail.empty()) {                         // friends somewhere else can't scan this screen: send them the link
			PaintCard(dc, UCX, UCY + 548, UCW, 56);
			UText(dc, "FRIENDS ON TAILSCALE", UCX + 16, UCY + 556, 300, 16, fSection, U_GOLD, DT_LEFT | DT_TOP | DT_SINGLELINE);
			UText(dc, tail, UCX + 16, UCY + 576, UCW - 300, 22, fBody, U_TEXT, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
			bool copied = Now() - g_tailCopiedAt < 3;
			UText(dc, copied ? "Copied" : "Click to copy, then send it to them", UCX + UCW - 300, UCY + 566, 284, 22, fSmall,
			      copied ? U_GREEN : U_MUTED, DT_RIGHT | DT_TOP | DT_SINGLELINE);
		}
	}
	for (auto &f : g_fields) if (f.page == g_page) PaintField(dc, f);
}

static void PaintChrome(HDC dc, int mx, int my) {
	UFill(dc, 0, 0, UW, UHEAD, U_SIDE);
	UFill(dc, 0, UHEAD, USIDE, g_uh - UHEAD, U_SIDE);
	UFill(dc, 0, UHEAD - 1, UW, 1, RGB(30, 33, 40));
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
		int y = UHEAD + 16 + p * 44;
		bool sel = p == g_page, hot = mx >= 0 && mx < USIDE && my >= y && my < y + 40;
		if (sel || hot) URound(dc, 10, y, USIDE - 20, 40, 8, sel ? U_CARD : RGB(19, 21, 27));
		if (sel) URound(dc, 10, y + 10, 4, 20, 2, U_GOLD);
		UText(dc, PageName[p], 30, y + 10, 150, 22, fNav, sel ? U_TEXT : U_MUTED);
	}
	const Snap &n = g_snap;
	int y = g_uh - 150;
	UText(dc, "STATUS", 28, y, 150, 16, fSection, U_MUTED);
	struct { const char *label; std::string val; COLORREF c; } rows[] = {
		{ "NR2003", n.attached ? "connected" : "waiting", n.attached ? U_GREEN : U_MUTED },
		{ "Camera", n.camOk ? "ready" : "-", n.camOk ? U_GREEN : U_MUTED },
		{ "Director", n.paused ? "paused" : "directing", n.paused ? U_GOLD : U_GREEN },
		{ "Session", !n.attached ? "-" : n.session == SESSION_RACE ? "race" : "not racing", n.attached && n.session == SESSION_RACE ? U_GREEN : U_MUTED },
	};
	for (int i = 0; i < 4; i++) {
		int ry = y + 24 + i * 26;
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
		std::vector<std::pair<std::string, std::string>> opts;
		std::string list = f->choices + ";";
		size_t a = 0, b;
		while ((b = list.find(';', a)) != std::string::npos) {
			std::string one = list.substr(a, b - a); a = b + 1;
			size_t bar = one.find('|');
			if (bar != std::string::npos) opts.push_back({ one.substr(0, bar), one.substr(bar + 1) });
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
	bool active = (d->CtlID == ID_PAUSE && g_snap.paused) || ((d->CtlID == ID_MOVE || (f && f->key == "move")) && g_snap.edit);
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
	int b = UCY + PAGE_END[p];
	for (auto &f : g_fields) if (f.page == p) b = std::max(b, UCY + f.y + (f.kind == fCams ? 32 : UCARD_H));
	for (auto &c : g_ctls) if (c.page == p) b = std::max(b, c.y + c.hgt);
	return b + 16;
}
static int MaxScroll(int p) { return std::max(0, PageBottom(p) - g_uh); }
// the page's controls follow the scroll: one sliding under the header is cut off there, one out of sight is hidden
static void PlaceCtls() {
	int oy = g_scroll[g_page];
	for (auto &c : g_ctls) {
		if (c.page < 0 || c.page != g_page) continue;
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
	for (auto &c : g_ctls) ShowWindow(c.h, c.page < 0 || c.page == p ? SW_SHOW : SW_HIDE);
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
	RECT side = { 0, Px(g_uh - 160), Px(USIDE), Px(g_uh) };
	InvalidateRect(g_ui, &side, FALSE);
	if (g_page == pLive) { RECT cards = PageRc(UCX, UCY, UCX + UCW, UCY + 90); InvalidateRect(g_ui, &cards, FALSE); }
	if (g_page == pArt) { RECT a = PageRc(UCX + 350, UCY + 300, UCX + UCW, UCY + 380); InvalidateRect(g_ui, &a, FALSE); }
	if (g_page == pReplays) { RECT a = PageRc(UCX, UCY + RL_Y, UCX + UCW, UCY + RLOG_Y + 280); InvalidateRect(g_ui, &a, FALSE); }
	if (g_page == pPhone) {                                       // the code, the link and the phones list
		RECT a = PageRc(UCX, UCY + 26, UCX + 304, UCY + 426), b = PageRc(UCX + 320, UCY + 356, UCX + UCW, UCY + 464);
		RECT t = PageRc(UCX, UCY + 548, UCX + UCW, UCY + 604);
		InvalidateRect(g_ui, &a, FALSE); InvalidateRect(g_ui, &b, FALSE); InvalidateRect(g_ui, &t, FALSE);
	}
}

static const UINT_PTR TIMER_REFRESH = 1, TIMER_COMMIT = 2;
static int g_pendingEdit = -1, g_mouseX = -1, g_mouseY = -1;

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
		if (was || now) { RECT r = { 0, Px(UHEAD), Px(USIDE), Px(UHEAD + 16 + PAGES * 44) }; InvalidateRect(h, &r, FALSE); }
		TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
		SetCursor(LoadCursor(NULL, now && y > UHEAD + 16 && y < UHEAD + 16 + PAGES * 44 ? IDC_HAND : IDC_ARROW));
		return 0;
	}
	case WM_MOUSELEAVE: {
		g_mouseX = g_mouseY = -1;
		RECT r = { 0, Px(UHEAD), Px(USIDE), Px(UHEAD + 16 + PAGES * 44) }; InvalidateRect(h, &r, FALSE);
		return 0;
	}
	case WM_SETCURSOR:
		if (LOWORD(l) == HTCLIENT && (HWND)w == h) return TRUE;   // WM_MOUSEMOVE sets it
		break;
	case WM_LBUTTONDOWN: {
		int x = (int)(short)LOWORD(l) * 96 / g_dpi, y = (int)(short)HIWORD(l) * 96 / g_dpi;
		SetFocus(h);
		int top, len, ty, th;
		if (x >= UW - 24 && y > UHEAD && ScrollBar(top, len, ty, th)) {
			g_dragBar = y >= ty && y < ty + th ? y - ty : th / 2;     // a click on the track jumps there
			SetCapture(h);
			DragBarTo(y);
			InvalidateRect(h, NULL, FALSE);
			return 0;
		}
		int py = y + g_scroll[g_page];                              // on the (scrolled) page
		if (x < USIDE && y > UHEAD + 16) {
			int p = (y - UHEAD - 16) / 44;
			if (p >= 0 && p < PAGES) ShowPage(p);
		}
		if (g_page == pReplays && x >= UCX && x < UCX + UCW && y > UHEAD && py >= UCY + RL_Y + 38) {
			int r = (py - UCY - RL_Y - 38) / RL_ROW;
			if (r >= 0 && r < (int)g_rlistIds.size()) { g_reqReplayId = g_rlistIds[r]; Log("-- replay crash #%d (Replays page) --\n", g_rlistIds[r]); }
		}
		if (g_page == pPhone && x >= UCX && x < UCX + UCW && y > UHEAD && py >= UCY + 548 && py < UCY + 604) {
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
		if (w != SIZE_MINIMIZED && HIWORD(l) > 0) {
			g_uh = HIWORD(l) * 96 / g_dpi;
			g_scroll[g_page] = std::min(g_scroll[g_page], MaxScroll(g_page));
			PlaceCtls();
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
		mm->ptMinTrackSize.x = mm->ptMaxTrackSize.x = a.right - a.left;
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
				POINT pt; GetCursorPos(&pt); ScreenToClient(f.h, &pt);
				RECT r; GetClientRect(f.h, &r);
				std::vector<std::string> vals;
				std::string list = f.choices + ";";
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
			char v[64]; snprintf(v, sizeof v, "%d, %d, %d", r.left, r.top, g_uh);
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
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN;
	int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
	EnterCriticalSection(&g_lock);
	std::string pos = Raw("window_pos");
	LeaveCriticalSection(&g_lock);
	int px, py, ph = UH;
	POINT at = { 0, 0 };
	if (sscanf(pos.c_str(), "%d , %d , %d", &px, &py, &ph) >= 2) {
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
	RECT r = { 0, 0, Px(UW), Px(g_uh) };
	AdjustWindowRect(&r, style, FALSE);
	g_ui = CreateWindowExA(0, "RaceVisionWindow", "RaceVision", style, x, y, r.right - r.left, r.bottom - r.top, NULL, NULL,
	                       GetModuleHandle(NULL), NULL);
	SendMessage(g_ui, WM_SETICON, ICON_SMALL, (LPARAM)AppIcon(GetSystemMetrics(SM_CXSMICON)));
	BOOL dark = TRUE;
	DwmSetWindowAttribute(g_ui, 20, &dark, sizeof dark);         // DWMWA_USE_IMMERSIVE_DARK_MODE: dark title bar

	// header buttons (every page)
	int bx = UW - 24;
	bx -= 140; g_btnMove = MakeButton(ID_MOVE, -1, bx, 14, 140, 36, "Move overlays");
	bx -= 150; g_btnOverlays = MakeButton(ID_OVERLAYS, -1, bx, 14, 140, 36, "Overlays on");
	bx -= 170; g_btnPause = MakeButton(ID_PAUSE, -1, bx, 14, 160, 36, "Pause directing");
	// live
	g_lbField = MakeList(ID_FIELDLIST, pLive, UCX + 8, UCY + 134, 284, 418);
	g_lbLog = MakeList(ID_LOG, pLive, UCX + 324, UCY + 134, UCW - 332, 418);
	g_btnOpenLog = MakeButton(ID_OPENLOG, pLive, UCX + UCW - 130, UCY + 572, 130, 32, "Open log file");
	g_btnCopy = MakeButton(ID_COPY, pLive, UCX + UCW - 250, UCY + 572, 110, 32, "Copy log");
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
	g_edPresetName = MakeCtl("EDIT", ES_AUTOHSCROLL | WS_TABSTOP, ID_PRESETNAME, pPresets, UCX + 445, UCY + 71, 374, 19);
	SendMessage(g_edPresetName, EM_SETLIMITTEXT, 60, 0);
	MakeButton(ID_SAVEPRESET, pPresets, UCX + 698, UCY + 106, 130, 32, "Save preset");
	g_lbDiffs = MakeList(ID_DIFFS, pPresets, UCX + 428, UCY + 252, 408, 236);
	// settings
	EnterCriticalSection(&g_lock);
	g_artWas = S.artFolders + "|" + S.artSeries;
	for (size_t i = 0; i < g_fields.size(); i++) {
		Field &f = g_fields[i];
		RECT b = FieldBox(f);
		int id = ID_FIELD0 + (int)i;
		if (f.kind == fNum || f.kind == fText || f.kind == fCams) {
			std::string value = Raw(f.key);
			if (value.empty() && f.key == "qual_handoff_m") value = "300";
			f.h = MakeCtl("EDIT", ES_AUTOHSCROLL | WS_TABSTOP, id, f.page, b.left + 9, b.top + 7, b.right - b.left - 18, 19, value.c_str());
		} else {
			f.h = MakeButton(id, f.page, b.left, b.top, b.right - b.left, b.bottom - b.top, f.kind == fAction ? f.choices.c_str() : "");
		}
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
