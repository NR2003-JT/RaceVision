// RaceVision overlay layouts (included by tv_director.cpp after the Draw... functions, before Render)
//
// An overlay is a LAYOUT: a size and a list of elements (box, text, image, portrait, bar, segment bar, arc, ordinal,
// coloured run of text). Each element has a position, a size and a data binding: a text carries {tokens} such as
// {car.last} or {car.speed_mph:0}, a bar takes a number such as car.throttle, an element can be shown only while
// a source is true (show = "story.any !replay"). Layouts are plain JSON files in <app folder>\layouts\*.json, written by
// the layout editor (the Director page's Overlays button, /overlays) or by hand. Backgrounds and pictures are PNGs from
// GIMP in layouts\images, fonts (.ttf / .otf) in layouts\fonts.
//
// Every built-in overlay is a layout too (tag_classic, tag_dash, battle_classic, battle_dash, onboard_classic, dash_dash):
// written to the folder when missing and never overwritten, so they can be edited. A panel's layout is the ini key
// layout_<panel> (tag, battle, onboard, dash, custom1..custom4), else the one for overlay_style. The old hard-coded
// drawing code stays as the fallback when a layout is missing or broken.

// ---------------------------------------------------------------- JSON (small: layouts and the editor's calls)
struct JV {
	enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
	bool b = false; double n = 0; std::string s;
	std::vector<JV> a; std::vector<std::pair<std::string, JV>> o;
	JV() {}
	static JV Str_(const std::string &v) { JV j; j.t = Str; j.s = v; return j; }
	static JV Num_(double v) { JV j; j.t = Num; j.n = v; return j; }
	static JV Bool_(bool v) { JV j; j.t = Bool; j.b = v; return j; }
	static JV Arr_() { JV j; j.t = Arr; return j; }
	static JV Obj_() { JV j; j.t = Obj; return j; }
	const JV *get(const std::string &k) const { if (t != Obj) return nullptr; for (auto &p : o) if (p.first == k) return &p.second; return nullptr; }
	bool has(const std::string &k) const { return get(k) != nullptr; }
	std::string str(const std::string &k, const std::string &d = "") const { const JV *v = get(k); return v && v->t == Str ? v->s : d; }
	double num(const std::string &k, double d = 0) const {
		const JV *v = get(k);
		return !v ? d : v->t == Num ? v->n : v->t == Bool ? (v->b ? 1 : 0) : v->t == Str ? atof(v->s.c_str()) : d;
	}
	bool flag(const std::string &k, bool d = false) const {
		const JV *v = get(k);
		return !v ? d : v->t == Bool ? v->b : v->t == Num ? v->n != 0 : v->t == Str ? (v->s == "true" || v->s == "1") : d;
	}
	JV &set(const std::string &k, const JV &v) {
		for (auto &p : o) if (p.first == k) { p.second = v; return p.second; }
		o.push_back({ k, v }); return o.back().second;
	}
};
struct JP { const char *p, *e; int depth = 0; };
static void JWs(JP &j) { while (j.p < j.e && (*j.p == ' ' || *j.p == '\t' || *j.p == '\r' || *j.p == '\n')) j.p++; }
static void JUtf8(std::string &o, unsigned c) {
	if (c < 0x80) o += (char)c;
	else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 63)); }
	else { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
}
static bool JStrParse(JP &j, std::string &o) {
	if (j.p >= j.e || *j.p != '"') return false;
	j.p++;
	while (j.p < j.e && *j.p != '"') {
		if (*j.p == '\\') {
			if (++j.p >= j.e) return false;
			switch (*j.p) {
			case 'n': o += '\n'; break; case 't': o += '\t'; break; case 'r': o += '\r'; break;
			case 'b': o += '\b'; break; case 'f': o += '\f'; break;
			case 'u': {
				if (j.e - j.p < 5) return false;
				unsigned c = (unsigned)strtoul(std::string(j.p + 1, 4).c_str(), NULL, 16);
				j.p += 4; JUtf8(o, c); break;
			}
			default: o += *j.p;
			}
			j.p++;
		} else o += *j.p++;
	}
	if (j.p >= j.e) return false;
	j.p++;
	return true;
}
static bool JValParse(JP &j, JV &v) {
	if (++j.depth > 40) return false;
	JWs(j);
	if (j.p >= j.e) return false;
	bool ok = true;
	char ch = *j.p;
	if (ch == '{') {
		v = JV::Obj_(); j.p++; JWs(j);
		if (j.p < j.e && *j.p == '}') j.p++;
		else for (;;) {
			JWs(j);
			std::string k; JV c;
			if (!JStrParse(j, k)) { ok = false; break; }
			JWs(j);
			if (j.p >= j.e || *j.p++ != ':') { ok = false; break; }
			if (!JValParse(j, c)) { ok = false; break; }
			v.o.push_back({ k, c });
			JWs(j);
			if (j.p < j.e && *j.p == ',') { j.p++; continue; }
			if (j.p < j.e && *j.p == '}') { j.p++; break; }
			ok = false; break;
		}
	} else if (ch == '[') {
		v = JV::Arr_(); j.p++; JWs(j);
		if (j.p < j.e && *j.p == ']') j.p++;
		else for (;;) {
			JV c;
			if (!JValParse(j, c)) { ok = false; break; }
			v.a.push_back(c);
			JWs(j);
			if (j.p < j.e && *j.p == ',') { j.p++; continue; }
			if (j.p < j.e && *j.p == ']') { j.p++; break; }
			ok = false; break;
		}
	} else if (ch == '"') { v = JV(); v.t = JV::Str; ok = JStrParse(j, v.s); }
	else if (j.e - j.p >= 4 && !strncmp(j.p, "true", 4)) { v = JV::Bool_(true); j.p += 4; }
	else if (j.e - j.p >= 5 && !strncmp(j.p, "false", 5)) { v = JV::Bool_(false); j.p += 5; }
	else if (j.e - j.p >= 4 && !strncmp(j.p, "null", 4)) { v = JV(); j.p += 4; }
	else {
		char *end = nullptr;
		double d = strtod(j.p, &end);
		if (end == j.p) ok = false; else { v = JV::Num_(d); j.p = end; }
	}
	j.depth--;
	return ok;
}
static bool JParse(const std::string &text, JV &out) {
	JP j{ text.c_str(), text.c_str() + text.size() };
	if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) j.p += 3;
	if (!JValParse(j, out)) return false;
	JWs(j);
	return j.p == j.e;
}
static void JQuote(std::string &o, const std::string &s) {
	o += '"';
	for (unsigned char ch : s) {
		if (ch == '"' || ch == '\\') { o += '\\'; o += (char)ch; }
		else if (ch == '\n') o += "\\n";
		else if (ch == '\t') o += "\\t";
		else if (ch < 32) { char b[8]; snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
		else o += (char)ch;
	}
	o += '"';
}
static void JDump(const JV &v, std::string &o, int ind, bool compact = false) {
	auto pad = [&](int n) { if (!compact) { o += '\n'; o.append((size_t)n * 2, ' '); } };
	switch (v.t) {
	case JV::Null: o += "null"; break;
	case JV::Bool: o += v.b ? "true" : "false"; break;
	case JV::Num: { char b[40]; snprintf(b, sizeof b, "%.10g", v.n); o += b; break; }
	case JV::Str: JQuote(o, v.s); break;
	case JV::Arr:
		o += '[';
		for (size_t i = 0; i < v.a.size(); i++) {
			if (i) o += ',';
			bool leaf = v.a[i].t != JV::Obj && v.a[i].t != JV::Arr;
			if (!leaf || v.a.size() > 12) pad(ind + 1);
			JDump(v.a[i], o, ind + 1, compact || (v.a[i].t == JV::Obj && ind >= 1));   // an element on one line
		}
		if (!v.a.empty()) pad(ind);
		o += ']';
		break;
	case JV::Obj:
		o += '{';
		for (size_t i = 0; i < v.o.size(); i++) {
			if (i) o += compact ? "," : ", ";
			if (!compact && ind < 1) pad(ind + 1);
			JQuote(o, v.o[i].first); o += ':';
			JDump(v.o[i].second, o, ind + 1, compact);
		}
		if (!compact && ind < 1 && !v.o.empty()) pad(ind);
		o += '}';
		break;
	}
}
static std::string JText(const JV &v) { std::string o; JDump(v, o, 0); return o + "\n"; }

static std::string LayAnsi(const std::string &u) {          // a layout's UTF-8 text to the game's ANSI
	bool ascii = true;
	for (unsigned char c : u) if (c >= 128) { ascii = false; break; }
	if (ascii) return u;
	int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), NULL, 0);
	std::wstring w(n, 0);
	MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), &w[0], n);
	int m = WideCharToMultiByte(CP_ACP, 0, w.c_str(), n, NULL, 0, NULL, NULL);
	std::string o(m, 0);
	WideCharToMultiByte(CP_ACP, 0, w.c_str(), n, &o[0], m, NULL, NULL);
	return o;
}

// ---------------------------------------------------------------- the layouts on disk
struct Layout {
	std::string name, path, title, panel, when, accent;
	float w = 300, h = 100, rowGap = 34;
	JV doc;
};
static std::map<std::string, Layout> g_lays;
// style presets (layouts\presets\*.json): a name for a whole look: which look it builds on (dash / classic: when the dash,
// tag and gauges show) and which layout each panel uses. overlay_style = preset:<name> picks one.
struct StylePreset { std::string name, title, base; JV layouts; };
static std::map<std::string, StylePreset> g_styles;
static std::map<std::string, std::string> g_layBad;           // file name -> why it was not loaded
static bool g_layLoaded = false;
static unsigned long long g_laySig = 0;
static double g_layChecked = 0;

static std::string LayDir() {
	std::string d = g_ini;
	size_t k = d.find_last_of("\\/");
	return (k == std::string::npos ? std::string(".") : d.substr(0, k)) + "\\layouts";
}
static bool LayName(const std::string &n) {
	if (n.empty() || n.size() > 40) return false;
	for (char c : n) if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
	return true;
}

// the built-in layouts: the looks RaceVision has always drawn, as data
struct LayDefault { const char *name, *json; };
static const LayDefault LAY_DEFAULTS[] = {
{ "tag_classic", R"LAY({"title":"Driver tag (classic)","panel":"tag","w":560,"h":126,"accent":"car","elements":[
{"name":"Panel","type":"rect","x":0,"y":0,"w":560,"h":96,"radius":10,"fill":"@bg"},
{"name":"Place block","type":"rect","x":0,"y":0,"w":96,"h":96,"radius":10,"fill":"@gold"},
{"name":"Driver colour","type":"rect","x":96,"y":90,"w":464,"h":6,"fill":"@car"},
{"name":"Place","type":"text","x":0,"y":18,"w":96,"size":44,"align":"center","color":"#141414","text":"{car.place_p}"},
{"name":"Portrait","type":"portrait","x":112,"y":2,"w":82,"h":88,"tab":false,"who":"car"},
{"name":"Driver name","type":"text","x":204,"y":14,"w":262,"size":30,"upper":true,"color":"@white","text":"{car.full}"},
{"name":"Number card","type":"image","x":470,"y":10,"w":80,"h":38,"art":"number","who":"car","alignRight":true},
{"name":"Number (no card art)","type":"text","x":470,"y":10,"w":80,"size":28,"align":"right","color":"@gold","text":"#{car.number}","show":"!car.has_card"},
{"name":"What he is doing","type":"run","x":206,"y":54,"w":346,"size":15,"show":"shot.sub","parts":[{"text":"RACE   ","color":"@gold"},{"text":"{shot.sub}","color":"@grey"}]},
{"name":"Story bar","type":"rect","x":12,"y":98,"w":536,"h":26,"radius":6,"fill":"#181b24e1","show":"story.any"},
{"name":"Story edge","type":"rect","x":12,"y":98,"w":4,"h":26,"fill":"@gold","show":"story.any"},
{"name":"Story place","type":"text","x":24,"y":102,"w":50,"size":14,"color":"@gold","text":"{story.label}","show":"story.any"},
{"name":"Story","type":"run","x":74,"y":102,"w":468,"size":13,"show":"story.any","parts":[{"story":true}]}
]})LAY" },
{ "tag_dash", R"LAY({"title":"Driver tag (dash look)","panel":"tag","w":520,"h":140,"accent":"car","elements":[
{"name":"Panel","type":"rect","x":0,"y":0,"w":520,"h":110,"fill":"#0a0c10eb","stroke":"@dash","strokeW":3},
{"name":"Place block","type":"rect","x":0,"y":0,"w":150,"h":110,"slant":22,"fill":"@dashfrom","fill2":"@dashto"},
{"name":"Place","type":"ordinal","x":8,"y":0,"size":34,"font":"dash","src":"car.place","under":"PLACE","color":"@white"},
{"name":"Portrait","type":"portrait","x":56,"y":14,"w":96,"h":96,"tab":false,"who":"car"},
{"name":"Number card","type":"image","x":420,"y":10,"w":84,"h":44,"art":"number","who":"car","alignRight":true},
{"name":"Number (no card art)","type":"text","x":420,"y":10,"w":84,"size":34,"align":"right","font":"dash","bold":true,"italic":true,"color":"@white","text":"{car.number}","show":"!car.has_card"},
{"name":"First name","type":"text","x":160,"y":6,"w":250,"size":18,"font":"dash","bold":true,"italic":true,"upper":true,"color":"@white","text":"{car.first}"},
{"name":"Last name","type":"text","x":156,"y":24,"w":258,"size":36,"font":"dash","bold":true,"italic":true,"upper":true,"color":"@white","text":"{car.last}"},
{"name":"What he is doing","type":"run","x":162,"y":76,"w":350,"size":16,"font":"dash","bold":true,"italic":true,"upper":true,"show":"shot.sub","parts":[{"text":"RACE   ","color":"@dash"},{"text":"{shot.sub}","color":"@grey"}]},
{"name":"Driver colour","type":"rect","x":146,"y":104,"w":371,"h":3,"fill":"@car","show":"car.color"},
{"name":"Story bar","type":"rect","x":0,"y":112,"w":520,"h":28,"fill":"#0a0c10eb","show":"story.any"},
{"name":"Story plate","type":"rect","x":0,"y":112,"w":90,"h":28,"slant":8,"fill":"@dashfrom","fill2":"@dashto","show":"story.any"},
{"name":"Story plate text","type":"text","x":8,"y":114,"w":74,"size":17,"font":"dash","bold":true,"italic":true,"upper":true,"color":"@white","text":"LAP {race.lap}","show":"story.any"},
{"name":"Story","type":"run","x":99,"y":114,"w":411,"size":17,"font":"dash","bold":true,"italic":true,"upper":true,"show":"story.any","parts":[{"story":true}]}
]})LAY" },
{ "battle_classic", R"LAY({"title":"Battle box (classic)","panel":"battle","w":420,"h":110,"accent":"car","rowGap":34,"elements":[
{"name":"Panel","type":"rect","x":0,"y":0,"w":420,"h":110,"radius":10,"fill":"@bg"},
{"name":"Title","type":"text","x":14,"y":6,"w":392,"size":14,"color":"@gold","text":"{battle.title}"},
{"name":"Lower place","row":"bottom","type":"text","x":14,"y":64,"w":58,"size":22,"color":"@white","text":"{bottom.place_p}"},
{"name":"Lower number card","row":"bottom","type":"image","x":70,"y":66,"w":56,"h":28,"art":"number","who":"bottom"},
{"name":"Lower number","row":"bottom","type":"text","x":70,"y":64,"w":68,"size":22,"color":"@gold","text":"#{bottom.number}","show":"!bottom.has_card"},
{"name":"Lower name","row":"bottom","type":"text","x":140,"y":64,"w":170,"size":22,"bold":false,"color":"@white","text":"{bottom.short}"},
{"name":"Upper place","row":"top","type":"text","x":14,"y":30,"w":58,"size":22,"color":"@white","text":"{top.place_p}"},
{"name":"Upper number card","row":"top","type":"image","x":70,"y":32,"w":56,"h":28,"art":"number","who":"top"},
{"name":"Upper number","row":"top","type":"text","x":70,"y":30,"w":68,"size":22,"color":"@gold","text":"#{top.number}","show":"!top.has_card"},
{"name":"Upper name","row":"top","type":"text","x":140,"y":30,"w":170,"size":22,"bold":false,"color":"@white","text":"{top.short}"},
{"name":"Gap","type":"text","x":300,"y":47,"w":110,"size":28,"align":"right","color":"@white","text":"{battle.gap:2}s"}
]})LAY" },
{ "battle_dash", R"LAY({"title":"Battle box (dash look)","panel":"battle","w":460,"h":130,"accent":"car","rowGap":42,"elements":[
{"name":"Panel","type":"rect","x":0,"y":0,"w":460,"h":130,"fill":"#0a0c10eb","stroke":"@dash","strokeW":3},
{"name":"Title plate","type":"rect","x":0,"y":0,"w":300,"h":30,"slant":10,"fill":"@dashfrom","fill2":"@dashto"},
{"name":"Title","type":"text","x":12,"y":1,"w":274,"size":22,"font":"dash","bold":true,"italic":true,"upper":true,"color":"@white","text":"{battle.title_ord}"},
{"name":"Lower place","row":"bottom","type":"text","x":10,"y":80,"w":34,"size":28,"align":"right","font":"dash","bold":true,"italic":true,"color":"@dash:bottom","text":"{bottom.place}"},
{"name":"Lower number card","row":"bottom","type":"image","x":52,"y":83,"w":56,"h":30,"art":"number","who":"bottom"},
{"name":"Lower number","row":"bottom","type":"text","x":52,"y":80,"w":60,"size":24,"font":"dash","bold":true,"italic":true,"color":"@white","text":"{bottom.number}","show":"!bottom.has_card"},
{"name":"Lower name","row":"bottom","type":"text","x":116,"y":80,"w":200,"size":28,"font":"dash","bold":true,"italic":true,"upper":true,"color":"@white","text":"{bottom.short}"},
{"name":"Lower colour","row":"bottom","type":"rect","x":46,"y":86,"w":3,"h":28,"fill":"@bottom","show":"bottom.color"},
{"name":"Upper place","row":"top","type":"text","x":10,"y":38,"w":34,"size":28,"align":"right","font":"dash","bold":true,"italic":true,"color":"@dash:top","text":"{top.place}"},
{"name":"Upper number card","row":"top","type":"image","x":52,"y":41,"w":56,"h":30,"art":"number","who":"top"},
{"name":"Upper number","row":"top","type":"text","x":52,"y":38,"w":60,"size":24,"font":"dash","bold":true,"italic":true,"color":"@white","text":"{top.number}","show":"!top.has_card"},
{"name":"Upper name","row":"top","type":"text","x":116,"y":38,"w":200,"size":28,"font":"dash","bold":true,"italic":true,"upper":true,"color":"@white","text":"{top.short}"},
{"name":"Upper colour","row":"top","type":"rect","x":46,"y":44,"w":3,"h":28,"fill":"@top","show":"top.color"},
{"name":"Gap","type":"text","x":320,"y":52,"w":130,"size":38,"align":"center","font":"dash","bold":true,"italic":true,"color":"@white","text":"{battle.gap:2}"},
{"name":"SEC","type":"text","x":320,"y":96,"w":130,"size":14,"align":"center","font":"dash","bold":true,"italic":true,"color":"@grey","text":"SEC"}
]})LAY" },
{ "onboard_classic", R"LAY({"title":"On-board gauges (classic)","panel":"onboard","w":300,"h":120,"accent":"car","elements":[
{"name":"Panel","type":"rect","x":0,"y":0,"w":300,"h":120,"radius":10,"fill":"@bg"},
{"name":"Speed","type":"text","x":10,"y":2,"w":150,"size":54,"align":"right","color":"@white","text":"{car.speed_mph:0}"},
{"name":"MPH","type":"text","x":164,"y":40,"w":60,"size":16,"color":"@grey","text":"MPH"},
{"name":"RPM","type":"text","x":164,"y":14,"w":130,"size":16,"color":"@white","text":"{car.rpm:0} RPM"},
{"name":"RPM bar","type":"bar","x":14,"y":80,"w":272,"h":10,"src":"car.rpm_10k","color":"@gold","track":"#323640"},
{"name":"Throttle bar","type":"bar","x":14,"y":98,"w":272,"h":10,"src":"car.throttle","color":"@green","track":"#323640"},
{"name":"Number card","type":"image","x":214,"y":48,"w":72,"h":28,"art":"number","who":"car","alignRight":true},
{"name":"Number","type":"text","x":230,"y":54,"w":56,"size":16,"color":"@gold","text":"#{car.number}","show":"!car.has_card"}
]})LAY" },
{ "dash_dash", R"LAY({"title":"In-car dash","panel":"dash","w":660,"h":140,"accent":"car","elements":[
{"name":"Panel","type":"rect","x":0,"y":0,"w":660,"h":110,"fill":"#0a0c10eb","stroke":"@dash","strokeW":3},
{"name":"Place block","type":"rect","x":0,"y":0,"w":150,"h":110,"slant":22,"fill":"@dashfrom","fill2":"@dashto"},
{"name":"Place","type":"ordinal","x":8,"y":0,"size":34,"font":"dash","src":"car.place","under":"PLACE","color":"@white"},
{"name":"Portrait","type":"portrait","x":56,"y":14,"w":96,"h":96,"tab":false,"who":"car"},
{"name":"Number card","type":"image","x":160,"y":66,"w":84,"h":36,"art":"number","who":"car"},
{"name":"Number (no card art)","type":"text","x":160,"y":62,"w":84,"size":32,"font":"dash","bold":true,"italic":true,"color":"@white","text":"{car.number}","show":"!car.has_card"},
{"name":"First name","type":"text","x":160,"y":6,"w":200,"size":18,"font":"dash","bold":true,"italic":true,"upper":true,"color":"@white","text":"{car.first}"},
{"name":"Last name","type":"text","x":156,"y":24,"w":204,"size":36,"font":"dash","bold":true,"italic":true,"upper":true,"color":"@white","text":"{car.last}"},
{"name":"Throttle arc","type":"arc","x":372,"y":24,"w":76,"h":76,"src":"car.throttle","from":150,"sweep":240,"thick":12,"segs":24,"color":"#5ac828","color2":"#ffe628","track":"#343840"},
{"name":"Speed","type":"text","x":360,"y":42,"w":100,"size":28,"align":"center","font":"dash","bold":true,"italic":true,"color":"@white","text":"{car.speed_mph:0}"},
{"name":"Brake bar","type":"segbar","x":370,"y":78,"w":80,"h":12,"src":"car.brake","segs":16,"gap":1.2,"color":"#be3228","color2":"#eb2828","track":"#343840"},
{"name":"MPH","type":"text","x":360,"y":74,"w":100,"size":14,"align":"center","font":"dash","bold":true,"italic":true,"color":"@white","text":"MPH"},
{"name":"RPM bar","type":"segbar","x":482,"y":16,"w":108,"h":26,"src":"car.rpm_frac","segs":12,"gap":3,"color":"#ff8c1e","color2":"#ff8c1e","hot":2,"hotColor":"#eb3c28","track":"#343840"},
{"name":"RPM","type":"text","x":470,"y":46,"w":125,"size":28,"align":"center","font":"dash","bold":true,"italic":true,"color":"@white","text":"{car.rpm:0}"},
{"name":"RPM label","type":"text","x":470,"y":88,"w":125,"size":14,"align":"center","font":"dash","bold":true,"italic":true,"color":"@white","text":"RPM"},
{"name":"Divider","type":"rect","x":600,"y":14,"w":2,"h":82,"fill":"#343840"},
{"name":"Gear","type":"text","x":602,"y":8,"w":56,"size":56,"align":"center","font":"dash","bold":true,"italic":true,"color":"@white","text":"{car.gear}","show":"car.in_gear"},
{"name":"Gear (N / R)","type":"text","x":602,"y":8,"w":56,"size":56,"align":"center","font":"dash","bold":true,"italic":true,"color":"@dash","text":"{car.gear}","show":"!car.in_gear"},
{"name":"GEAR label","type":"text","x":602,"y":88,"w":56,"size":14,"align":"center","font":"dash","bold":true,"italic":true,"color":"@white","text":"GEAR"},
{"name":"Story bar","type":"rect","x":0,"y":112,"w":660,"h":28,"fill":"#0a0c10eb","show":"story.any"},
{"name":"Story plate","type":"rect","x":0,"y":112,"w":90,"h":28,"slant":8,"fill":"@dashfrom","fill2":"@dashto","show":"story.any"},
{"name":"Story plate text","type":"text","x":8,"y":114,"w":74,"size":17,"font":"dash","bold":true,"italic":true,"upper":true,"color":"@white","text":"LAP {race.lap}","show":"story.any"},
{"name":"Story","type":"run","x":99,"y":114,"w":551,"size":17,"font":"dash","bold":true,"italic":true,"upper":true,"show":"story.any","parts":[{"story":true}]}
]})LAY" },
};

static void LayMkdir(const std::string &d) { CreateDirectoryA(d.c_str(), NULL); }
static void LayInstall() {
	std::string dir = LayDir();
	LayMkdir(dir); LayMkdir(dir + "\\images"); LayMkdir(dir + "\\fonts");
	{   // the overlay pack next to the exe (layouts_stock\): its layouts, pictures and style presets are copied in once; what was
		// copied is noted in layouts\stock.txt, so one you delete stays deleted
		std::string exeDir = g_ini;
		size_t k = exeDir.find_last_of("\\/");
		exeDir = k == std::string::npos ? std::string(".") : exeDir.substr(0, k);
		std::string from = exeDir + "\\layouts_stock", note = dir + "\\stock.txt", seen = "\n" + ReadAll(note), add;
		for (size_t q = 0; (q = seen.find("\r\n", q)) != std::string::npos;) seen.erase(q, 1);
		LayMkdir(dir + "\\presets");
		struct Sub { const char *sub, *pat; } subs[] = { { "", "*.json" }, { "\\images", "*.*" }, { "\\presets", "*.json" } };
		for (auto &sb : subs) {
			WIN32_FIND_DATAA w;
			HANDLE h = FindFirstFileA((from + sb.sub + "\\" + sb.pat).c_str(), &w);
			if (h == INVALID_HANDLE_VALUE) continue;
			do {
				if (w.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
				std::string rel = std::string(sb.sub) + "\\" + w.cFileName;
				if (seen.find("\n" + rel + "\n") != std::string::npos) continue;
				CopyFileA((from + rel).c_str(), (dir + rel).c_str(), TRUE);       // (never over one that is there)
				add += rel + "\n";
			} while (FindNextFileA(h, &w));
			FindClose(h);
		}
		if (!add.empty()) { FILE *f = fopen(note.c_str(), "ab"); if (f) { fwrite(add.data(), 1, add.size(), f); fclose(f); } Log("-- overlay pack: new layouts / presets copied into layouts --\n"); }
	}
	{   // the font pack next to the exe (fontpack\): copied in when layouts\fonts has no fonts yet
		std::string fd = dir + "\\fonts", pack = g_ini;
		pack = (pack.find_last_of("\\/") == std::string::npos ? std::string(".") : pack.substr(0, pack.find_last_of("\\/"))) + "\\fontpack";
		WIN32_FIND_DATAA w;
		HANDLE h = FindFirstFileA((fd + "\\*.*").c_str(), &w);
		bool have = false;
		if (h != INVALID_HANDLE_VALUE) { do { if (!(w.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) have = true; } while (!have && FindNextFileA(h, &w)); FindClose(h); }
		if (!have && IsDir(pack)) {
			h = FindFirstFileA((pack + "\\*.*").c_str(), &w);
			if (h != INVALID_HANDLE_VALUE) {
				do { if (!(w.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) CopyFileA((pack + "\\" + w.cFileName).c_str(), (fd + "\\" + w.cFileName).c_str(), TRUE); } while (FindNextFileA(h, &w));
				FindClose(h);
			}
			Log("-- overlay fonts: the font pack was copied into layouts\\fonts --\n");
		}
	}
	for (const LayDefault &d : LAY_DEFAULTS) {
		if (!d.json) continue;
		std::string path = dir + "\\" + d.name + ".json";
		if (IsFile(path)) continue;
		JV doc;
		if (!JParse(d.json, doc)) { Log("-- built-in layout %s did not parse (bug) --\n", d.name); continue; }
		FILE *f = fopen(path.c_str(), "wb");
		if (f) { std::string t = JText(doc); fwrite(t.data(), 1, t.size(), f); fclose(f); }
	}
	std::string rd = dir + "\\README.txt";
	if (!IsFile(rd)) {
		FILE *f = fopen(rd.c_str(), "wb");
		if (f) {
			fputs("RaceVision overlay layouts\r\n\r\n"
			      "Each .json file here is one overlay. Build and edit them in RaceVision's Director page (Overlays button),\r\n"
			      "or edit the files by hand: RaceVision picks up a changed file within a second.\r\n\r\n"
			      "images\\  PNG / JPG pictures for the layouts (draw backgrounds and ticker art in GIMP, export as PNG here).\r\n"
			      "fonts\\   .ttf / .otf fonts to use in text (read when RaceVision starts).\r\n\r\n"
			      "The built-in layouts (tag_classic, tag_dash, battle_classic, battle_dash, onboard_classic, dash_dash) are\r\n"
			      "written back only when missing; the editor has a Reset button for them.\r\n", f);
			fclose(f);
		}
	}
}

static void LayFinish(Layout &L) {
	L.title = L.doc.str("title", L.name);
	L.panel = L.doc.str("panel", "custom");
	L.when = L.doc.str("when", "");
	L.accent = L.doc.str("accent", "car");
	L.w = (float)std::min(2400.0, std::max(20.0, L.doc.num("w", 300)));
	L.h = (float)std::min(1400.0, std::max(20.0, L.doc.num("h", 100)));
	L.rowGap = (float)L.doc.num("rowGap", 34);
}
static unsigned long long LaySig() {
	unsigned long long sig = 0;
	WIN32_FIND_DATAA fd;
	for (const char *pat : { "\\*.json", "\\presets\\*.json" }) {
		HANDLE h = FindFirstFileA((LayDir() + pat).c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) continue;
		do { sig = sig * 31 + fd.ftLastWriteTime.dwLowDateTime + fd.ftLastWriteTime.dwHighDateTime + fd.nFileSizeLow + 7; } while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	return sig;
}
static void LayApplySizes();
static void LayLoad() {
	g_lays.clear(); g_layBad.clear();
	std::string dir = LayDir();
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "\\*.json").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			std::string file = fd.cFileName, name = file.substr(0, file.size() - 5);
			if (!LayName(name)) continue;
			std::string text = ReadAll(dir + "\\" + file);
			Layout L; L.name = name; L.path = dir + "\\" + file;
			if (!JParse(text, L.doc) || L.doc.t != JV::Obj) { g_layBad[name] = "not valid JSON"; Log("-- layout %s is not valid JSON, skipped --\n", file.c_str()); continue; }
			LayFinish(L);
			g_lays[name] = L;
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	g_styles.clear();
	h = FindFirstFileA((dir + "\\presets\\*.json").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			std::string file = fd.cFileName, name = file.substr(0, file.size() - 5);
			JV doc;
			if (!LayName(name) || !JParse(ReadAll(dir + "\\presets\\" + file), doc) || doc.t != JV::Obj) continue;
			StylePreset sp;
			sp.name = name; sp.title = doc.str("title", name); sp.base = doc.str("base", "classic");
			if (const JV *l = doc.get("layouts")) if (l->t == JV::Obj) sp.layouts = *l;
			g_styles[name] = sp;
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	g_laySig = LaySig();
	g_layLoaded = true;
	LayApplySizes();
}

// fonts: .ttf / .otf in layouts\fonts (and the companion app's fonts folder), read once at start
static PrivateFontCollection *g_layPfc = nullptr;
static std::vector<std::string> g_layFontNames;
struct LayFontFile { std::string family, file, path; bool mine; };          // one font file: its family, name, whole path
static std::vector<LayFontFile> g_layFontFiles;
static std::vector<std::string> g_laySysFonts;                              // the fonts installed in Windows
static std::string LayU8(const wchar_t *w) { char u8[200] = ""; WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, sizeof u8, NULL, NULL); return u8; }
// a font file's family name (the name table's ID 1, which is what GDI+ calls the family), read straight from the file
// (a second PrivateFontCollection per file just for this unloaded the fonts again when it went away)
static std::string LayFontName(const std::string &path) {
	std::string d = ReadAll(path);
	auto u16 = [&](size_t o) { return o + 2 <= d.size() ? ((unsigned char)d[o] << 8) | (unsigned char)d[o + 1] : 0; };
	auto u32 = [&](size_t o) { return o + 4 <= d.size() ? ((unsigned)u16(o) << 16) | (unsigned)u16(o + 2) : 0u; };
	size_t base = d.size() > 4 && !memcmp(d.data(), "ttcf", 4) ? u32(12) : 0;
	int tables = u16(base + 4);
	for (int i = 0; i < tables && i < 64; i++) {
		size_t rec = base + 12 + (size_t)i * 16;
		if (rec + 16 > d.size() || memcmp(d.data() + rec, "name", 4)) continue;
		size_t t = u32(rec + 8), count = u16(t + 2), strs = t + u16(t + 4);
		std::string best;
		for (size_t k = 0; k < count && k < 400; k++) {
			size_t r = t + 6 + k * 12;
			if (r + 12 > d.size() || u16(r + 6) != 1) continue;
			size_t len = u16(r + 8), off = strs + u16(r + 10);
			if (off + len > d.size()) continue;
			if (u16(r) == 3) {                                  // Windows, UTF-16BE
				std::wstring w;
				for (size_t j = 0; j + 1 < len; j += 2) w += (wchar_t)u16(off + j);
				return LayU8(w.c_str());
			}
			if (best.empty()) best = d.substr(off, len);        // Mac, one byte per letter
		}
		if (!best.empty()) return best;
	}
	size_t sl = path.find_last_of('\\');
	return path.substr(sl == std::string::npos ? 0 : sl + 1);
}
static void LayLoadFonts() {
	if (g_layPfc) return;
	g_layPfc = new PrivateFontCollection();
	std::vector<std::string> dirs = { LayDir() + "\\fonts" };
	for (auto &r : g_artRoots) dirs.push_back(r + "\\fonts");
	for (auto &d : dirs) {
		for (const char *pat : { "\\*.ttf", "\\*.otf" }) {
			WIN32_FIND_DATAA fd;
			HANDLE h = FindFirstFileA((d + pat).c_str(), &fd);
			if (h == INVALID_HANDLE_VALUE) continue;
			do {
				std::string p = d + "\\" + fd.cFileName;
				std::wstring w(p.begin(), p.end());
				g_layPfc->AddFontFile(w.c_str());
				g_layFontFiles.push_back({ LayFontName(p), fd.cFileName, p, d == dirs[0] });
			} while (FindNextFileA(h, &fd));
			FindClose(h);
		}
	}
	int n = g_layPfc->GetFamilyCount(), got = 0;
	if (n > 0) {
		std::vector<FontFamily> fams(n);
		g_layPfc->GetFamilies(n, fams.data(), &got);
		for (int i = 0; i < got; i++) {
			wchar_t nm[LF_FACESIZE] = L"";
			fams[i].GetFamilyName(nm);
			g_layFontNames.push_back(LayU8(nm));
		}
	}
	InstalledFontCollection sys;
	int sn = sys.GetFamilyCount(), sg = 0;
	if (sn > 0) {
		std::vector<FontFamily> fams(sn);
		sys.GetFamilies(sn, fams.data(), &sg);
		for (int i = 0; i < sg; i++) { wchar_t nm[LF_FACESIZE] = L""; fams[i].GetFamilyName(nm); g_laySysFonts.push_back(LayU8(nm)); }
	}
}
static void LayEnsure() {
	if (g_layLoaded) return;
	LayInstall();
	LayLoad();
}
static void LayPoll() {                          // a hand-edited file is picked up within a second
	double now = Now();
	if (!g_layLoaded || now - g_layChecked < 1.0) return;
	g_layChecked = now;
	if (LaySig() != g_laySig) { Log("-- overlay layouts changed on disk: reloaded --\n"); LayLoad(); }
}

static const StylePreset *LayStyleNow() {
	if (S.overlayStyle.compare(0, 7, "preset:") != 0) return nullptr;
	LayEnsure();
	auto it = g_styles.find(S.overlayStyle.substr(7));
	return it == g_styles.end() ? nullptr : &it->second;
}
static bool StyleIsDash() {
	if (const StylePreset *sp = LayStyleNow()) return _stricmp(sp->base.c_str(), "dash") == 0;
	return _stricmp(S.overlayStyle.c_str(), "dash") == 0;
}
// the Style box's choices: the two built-in looks and every preset
static std::string LayStyleChoices() {
	EnterCriticalSection(&g_lock);
	LayEnsure();
	std::string o = "dash|Dash;classic|Classic";
	for (auto &kv : g_styles) {
		std::string t = kv.second.title;
		for (auto &c : t) if (c == ';' || c == '|') c = ' ';
		o += ";preset:" + kv.first + "|" + t;
	}
	LeaveCriticalSection(&g_lock);
	return o;
}
static void LayClearPanelKeys() {                    // a new style is the whole look: the per-panel picks go
	for (const char *k : { "tag", "battle", "onboard", "dash", "custom1", "custom2", "custom3", "custom4" })
		if (!Trim(S.raw["layout_" + std::string(k)]).empty()) SaveKey(std::string("layout_") + k, "");
}
static const Layout *LayoutFor(const Panel &p) {
	LayEnsure();
	std::string key = p.key, name;
	auto it = S.raw.find("layout_" + key);
	if (it != S.raw.end()) name = Trim(it->second);
	if (name.empty()) if (const StylePreset *sp = LayStyleNow()) { const JV *m = sp->layouts.get(key); if (m && m->t == JV::Str) name = m->s; }
	if (name.empty()) {
		bool dash = DashLook();
		if (key == "tag") name = dash && !OvReplay() ? "tag_dash" : "tag_classic";
		else if (key == "battle") name = dash ? "battle_dash" : "battle_classic";
		else if (key == "onboard") name = "onboard_classic";
		else if (key == "dash") name = "dash_dash";
		else return nullptr;
	}
	auto l = g_lays.find(name);
	return l == g_lays.end() ? nullptr : &l->second;
}
static void LayApplySizes() {
	for (Panel *p : g_panels) if (const Layout *L = LayoutFor(*p)) { p->baseW = (int)L->w; p->baseH = (int)L->h; }
}

// ---------------------------------------------------------------- data
struct SmpDrv { int num; const char *first, *last; unsigned rgb; };
static const SmpDrv SMP[] = {
	{ 24, "Jeff", "Gordon", 0x2a5db0 }, { 48, "Jimmie", "Johnson", 0x1e90d6 }, { 88, "Dale", "Jarrett", 0xd8a010 },
	{ 8, "Dale", "Earnhardt Jr.", 0xc82828 }, { 20, "Tony", "Stewart", 0x3cb44a }, { 17, "Matt", "Kenseth", 0x8a8a8a },
	{ 6, "Mark", "Martin", 0xe0e0e0 }, { 99, "Carl", "Edwards", 0xd04010 }, { 9, "Kasey", "Kahne", 0x40a0ff },
	{ 18, "Bobby", "Labonte", 0x30c030 }, { 12, "Ryan", "Newman", 0xc0c0ff }, { 31, "Jeff", "Burton", 0xd0d000 } };

struct LCtx {
	bool sample = false, battle = false;
	int car = -1, other = -1, top = -1, bottom = -1;
	double slide = 1.0;
	std::string accent = "car";
	// worked out once per drawing
	bool factsDone = false; std::vector<Fact> facts; size_t factN = 0;
	bool gapDone = false; double gap = 0.31, metres = 24; int hi = 2;
};
struct Drv { int slot = -1, sidx = -1, splace = 0; bool ok = false; };
struct LV {
	std::string s; double n = 0; bool isNum = false, truth = false, unknown = false;
};

static Drv LayWho(const LCtx &cx, const std::string &who) {
	Drv d;
	auto pick = [&](int slot, int sidx, int splace) {
		if (cx.sample) { d.sidx = ((sidx % 12) + 12) % 12; d.splace = splace; d.ok = true; }
		else if (slot >= 0 && slot < NUM_SLOTS && g_car[slot].valid) { d.slot = slot; d.ok = true; }
	};
	auto order = [&](int rel) -> int {              // the car `rel` places along the live order from the car on screen
		for (size_t k = 0; k < g_order.size(); k++)
			if (g_order[k] == cx.car) { long at = (long)k + rel; return at >= 0 && at < (long)g_order.size() ? g_order[at] : -1; }
		return -1;
	};
	if (who == "car") pick(cx.car, 0, 3);
	else if (who == "other") pick(cx.other, 1, 2);
	else if (who == "top") pick(cx.battle ? cx.top : cx.other, 1, 2);
	else if (who == "bottom") pick(cx.battle ? cx.bottom : cx.car, 0, 3);
	else if (who == "leader") pick(g_order.empty() ? -1 : g_order[0], 2, 1);
	else if (who == "ahead") pick(order(-1), 1, 2);
	else if (who == "behind") pick(order(1), 3, 4);
	else if (who.size() >= 2 && who[0] == 'p' && isdigit((unsigned char)who[1])) {
		int n = atoi(who.c_str() + 1);
		if (n >= 1 && n <= 99) pick(n <= (int)g_order.size() ? g_order[n - 1] : -1, n + 1, n);
	}
	return d;
}
static bool LayIsWho(const std::string &w) {
	return w == "car" || w == "other" || w == "top" || w == "bottom" || w == "leader" || w == "ahead" || w == "behind" ||
	       (w.size() >= 2 && w[0] == 'p' && isdigit((unsigned char)w[1]));
}

static std::string LapStr(int ms) {
	if (ms <= 0) return "--";
	char b[32];
	if (ms >= 60000) snprintf(b, sizeof b, "%d:%06.3f", ms / 60000, (ms % 60000) / 1000.0);
	else snprintf(b, sizeof b, "%.3f", ms / 1000.0);
	return b;
}
static void LNum(LV &o, double n, const char *fmt = "%.0f") { char b[48]; snprintf(b, sizeof b, fmt, n); o.s = b; o.n = n; o.isNum = true; o.truth = n != 0; }
static void LStr(LV &o, const std::string &s) { o.s = s; o.truth = !s.empty(); }
static void LBool(LV &o, bool v, const char *yes = "YES", const char *no = "") { o.s = v ? yes : no; o.n = v ? 1 : 0; o.truth = v; }

static std::map<int, float> g_layTopRpm;           // highest RPM seen per car = the end of an RPM bar

static bool LayCarField(LCtx &cx, const Drv &d, const std::string &f, LV &o) {
	bool live = d.slot >= 0;
	const Car *c = live ? &g_car[d.slot] : nullptr;
	const SmpDrv &sm = SMP[d.sidx >= 0 ? d.sidx : 0];
	int number = live ? c->number : sm.num;
	bool replayCar = live && OvReplay() && d.slot == OvCar();
	auto place = [&]() { return live ? LivePos(d.slot) : d.splace; };
	auto mph = [&]() {
		float v = !live ? 182.f : replayCar ? OvRec(d.slot, REC_MPH) / 1.30f : c->speed * 2.23694f;
		return v < 0 || v > 400 || v != v ? 0.f : v;
	};
	auto rpm = [&]() { return !live ? 8275.f : OvRec(d.slot, REC_RPM); };
	auto thr = [&]() { float v = !live ? 0.85f : replayCar ? OvRec(d.slot, REC_THROTTLE) : c->thr; return v != v ? 0.f : std::min(1.f, std::max(0.f, v)); };
	auto steer = [&]() { return !live ? -12.f : c->steer; };
	auto brk = [&]() { return !live ? 0.f : replayCar ? 0.f : std::min(1.f, std::max(0.f, c->brk)); };
	auto gear = [&]() { return !live ? 4 : replayCar ? -9 : c->gear; };
	if (f == "place") LNum(o, place());
	else if (f == "place_p") { LStr(o, "P" + std::to_string(place())); o.n = place(); }
	else if (f == "place_ord") { LStr(o, Ordinal(place())); o.n = place(); }
	else if (f == "number") { LStr(o, NumLabel(number)); o.n = number; }
	else if (f == "first") LStr(o, live ? c->first : sm.first);
	else if (f == "last") LStr(o, live ? c->last : sm.last);
	else if (f == "full") LStr(o, live ? c->first + " " + c->last : std::string(sm.first) + " " + sm.last);
	else if (f == "short") LStr(o, live ? Short(d.slot) : std::string(sm.last));
	else if (f == "speed_mph") LNum(o, mph());
	else if (f == "speed_kph") LNum(o, mph() * 1.609344);
	else if (f == "rpm") LNum(o, rpm());
	else if (f == "rpm_10k") LNum(o, std::min(1.f, std::max(0.f, rpm() / 10000.f)), "%.3f");
	else if (f == "rpm_frac") {
		float &top = g_layTopRpm[live ? d.slot : -1 - d.sidx];
		float r = rpm();
		top = std::max(top, std::max(r, 8000.0f));
		LNum(o, std::min(1.f, std::max(0.f, r / top)), "%.3f");
	}
	else if (f == "gear") { int g = gear(); LStr(o, g == -9 ? "-" : g < 0 ? "R" : g == 0 ? "N" : std::to_string(g)); o.n = g; }
	else if (f == "in_gear") LBool(o, gear() > 0);
	else if (f == "throttle") LNum(o, thr(), "%.3f");
	else if (f == "brake") LNum(o, brk(), "%.3f");
	else if (f == "throttle_pct") LNum(o, thr() * 100);
	else if (f == "brake_pct") LNum(o, brk() * 100);
	else if (f == "steer") LNum(o, steer(), "%.1f");
	else if (f == "steer_abs") LNum(o, fabs(steer()), "%.1f");
	else if (f == "laps") LNum(o, live ? c->laps : 112);
	else if (f == "laps_led") LNum(o, live ? c->lapsLed : 31);
	else if (f == "pits") LNum(o, live ? c->pits : 2);
	else if (f == "start_pos") LNum(o, live ? c->startPos : 14);
	else if (f == "places_gained") {
		int from = live ? c->startPos : 14, now = place();
		if (from <= 0) { LStr(o, ""); return true; }
		int dl = from - now;
		LStr(o, dl > 0 ? "+" + std::to_string(dl) : dl < 0 ? "-" + std::to_string(-dl) : "EVEN"); o.n = dl; o.truth = true;
	}
	else if (f == "last_lap") { int ms = live ? c->lapLastMs : 48217; LStr(o, LapStr(ms)); o.n = ms / 1000.0; o.truth = ms > 0; }
	else if (f == "best_lap") { int ms = live ? c->lapBestMs : 47984; LStr(o, LapStr(ms)); o.n = ms / 1000.0; o.truth = ms > 0; }
	else if (f == "interval" || f == "gap_leader") {
		if (!live) { LStr(o, f == "interval" ? "+0.31" : "+3.84"); return true; }
		int idx = -1;
		for (size_t k = 0; k < g_order.size(); k++) if (g_order[k] == d.slot) idx = (int)k;
		if (idx <= 0) { LStr(o, idx == 0 ? "LEADER" : ""); return true; }
		int ref = f == "interval" ? g_order[idx - 1] : g_order[0];
		double lapsDown = floor((g_car[ref].progress - c->progress) / std::max(g_trackLen, 400.0f) + 0.02);
		if (lapsDown >= 1) { LStr(o, "+" + std::to_string((int)lapsDown) + (lapsDown >= 2 ? " LAPS" : " LAP")); o.n = lapsDown; return true; }
		double sec = std::max(0.0, g_car[ref].progress - c->progress) / std::max(c->speed, 10.0f);
		char b[32]; snprintf(b, sizeof b, sec >= 10 ? "+%.1f" : "+%.2f", sec);
		LStr(o, b); o.n = sec;
	}
	else if (f == "pit") LBool(o, live && c->pit);
	else if (f == "color") {
		bool has = live ? g_colors.count(NumKey(number)) > 0 : true;
		LBool(o, has);
	}
	else if (f == "has_card") LBool(o, NumberArt(number) != nullptr);
	else if (f == "valid") LBool(o, true);
	else { o.unknown = true; return false; }
	return true;
}

static void LayFacts(LCtx &cx) {
	if (cx.factsDone) return;
	cx.factsDone = true;
	if (!S.tagStory) return;
	cx.facts = StoryFacts(cx.sample ? -1 : OvCar());
	if (!cx.facts.empty()) cx.factN = (size_t)(std::max(0.0, Now() - g_shot.since) / std::max(1.0, S.tagRotate)) % cx.facts.size();
}
static void LayGap(LCtx &cx) {
	if (cx.gapDone) return;
	cx.gapDone = true;
	if (cx.sample) return;
	if (cx.battle && cx.top >= 0 && cx.bottom >= 0) {
		PairGap(cx.top, cx.bottom, cx.gap, cx.metres);
		cx.hi = std::min(LivePos(cx.top), LivePos(cx.bottom));
	} else if (cx.car >= 0 && cx.other >= 0 && g_car[cx.car].valid && g_car[cx.other].valid) {
		cx.gap = fabs(Gap(cx.other, cx.car)); cx.metres = fabs(Ahead(cx.other, cx.car));
		cx.hi = std::min(LivePos(cx.car), LivePos(cx.other));
	}
}

static bool LayVal(const std::string &name, LCtx &cx, LV &o) {
	size_t dot = name.find('.');
	if (dot == std::string::npos) {
		if (name == "replay") { LBool(o, OvReplay()); return true; }
		o.unknown = true; return false;
	}
	std::string a = name.substr(0, dot), f = name.substr(dot + 1);
	if (LayIsWho(a)) {
		Drv d = LayWho(cx, a);
		if (!d.ok) return false;
		return LayCarField(cx, d, f, o);
	}
	if (a == "shot" && f == "sub") { LStr(o, ShotSub(cx.sample)); return true; }
	if (a == "story") {
		LayFacts(cx);
		if (f == "any") { LBool(o, !cx.facts.empty()); return true; }
		if (f == "label") { LStr(o, cx.facts.empty() ? "" : cx.facts[cx.factN].label); return true; }
	}
	if (a == "battle") {
		LayGap(cx);
		bool pass = g_shot.kind == kPass && !cx.sample;
		if (f == "title") { LStr(o, std::string(pass ? "PASS FOR P" : "BATTLE FOR P") + std::to_string(cx.hi)); return true; }
		if (f == "title_ord") { LStr(o, std::string(pass ? "PASS FOR " : "BATTLE FOR ") + Ordinal(cx.hi)); return true; }
		if (f == "place") { LNum(o, cx.hi); return true; }
		if (f == "gap") { LNum(o, cx.gap, "%.2f"); return true; }
		if (f == "metres") { LNum(o, cx.metres); return true; }
	}
	if (a == "race") {
		if (f == "lap") { LNum(o, cx.sample ? 112 : g_leaderLaps); return true; }
		if (f == "laps_total") { LNum(o, cx.sample ? 200 : g_lapsTotal); return true; }
		if (f == "progress") { LNum(o, cx.sample ? 0.56 : g_lapsTotal > 0 ? std::min(1.0, (double)g_leaderLaps / g_lapsTotal) : 0, "%.3f"); return true; }
		if (f == "laps_to_go") { LNum(o, cx.sample ? 88 : std::max(0, g_lapsTotal - g_leaderLaps)); return true; }
		if (f == "green") { LBool(o, cx.sample || g_flag == FLAG_GREEN); return true; }
		if (f == "flag") { LStr(o, cx.sample || g_flag == FLAG_GREEN ? "GREEN" : g_flag == 5 ? "ONE TO GO" : g_flag == 0 ? "" : "CAUTION"); return true; }
	}
	if (a == "track" && f == "name") { LStr(o, cx.sample ? "Talladega" : g_trackName); return true; }
	o.unknown = true;
	return false;
}

// "{car.last}", "{car.speed_mph:0}" (digits = decimals) inside a text; the rest is kept as written
static std::string LayExpand(const std::string &tpl, LCtx &cx, bool *missing = nullptr) {
	std::string out;
	for (size_t i = 0; i < tpl.size();) {
		if (tpl[i] == '{') {
			size_t e = tpl.find('}', i);
			if (e == std::string::npos) { out += LayAnsi(tpl.substr(i)); break; }
			std::string tok = tpl.substr(i + 1, e - i - 1), fmt;
			size_t c = tok.find(':');
			if (c != std::string::npos) { fmt = tok.substr(c + 1); tok.resize(c); }
			LV v;
			if (LayVal(tok, cx, v)) {
				if (v.isNum && !fmt.empty() && isdigit((unsigned char)fmt[0])) { char b[64]; snprintf(b, sizeof b, "%.*f", std::min(6, atoi(fmt.c_str())), v.n); out += b; }
				else out += v.s;
			} else if (v.unknown) out += "?";
			else if (missing) *missing = true;           // that car is not there: the whole text stays off
			i = e + 1;
		} else {
			size_t e = tpl.find('{', i);
			if (e == std::string::npos) e = tpl.size();
			out += LayAnsi(tpl.substr(i, e - i));
			i = e;
		}
	}
	return out;
}
static bool LayShow(const std::string &rule, LCtx &cx) {
	size_t i = 0;
	while (i < rule.size()) {
		while (i < rule.size() && (rule[i] == ' ' || rule[i] == ',')) i++;
		size_t e = i;
		while (e < rule.size() && rule[e] != ' ' && rule[e] != ',') e++;
		if (e > i) {
			std::string tok = rule.substr(i, e - i);
			bool neg = tok[0] == '!';
			if (neg) tok.erase(0, 1);
			LV v;
			bool t = LayVal(tok, cx, v) && v.truth;
			if (t == neg) return false;
		}
		i = e;
	}
	return true;
}

// ---------------------------------------------------------------- colours
static Color LayPaint(const LCtx &cx, const std::string &who, bool &has) {
	Drv d = LayWho(cx, who);
	has = false;
	if (!d.ok) return ACCENT;
	if (d.slot < 0) { has = true; unsigned r = SMP[d.sidx].rgb; return C(255, (r >> 16) & 255, (r >> 8) & 255, r & 255); }
	auto it = g_colors.find(NumKey(g_car[d.slot].number));
	if (it == g_colors.end()) return ACCENT;
	has = true;
	return it->second;
}
static int LayNumber(const LCtx &cx, const std::string &who) {
	Drv d = LayWho(cx, who);
	return !d.ok ? NO_CAR_COLOR : d.slot >= 0 ? g_car[d.slot].number : SMP[d.sidx].num;
}
static Color LayColor(const std::string &spec, const LCtx &cx, double opacity) {
	Color col = WHITE;
	if (!spec.empty() && spec[0] == '#') {
		unsigned long v = strtoul(spec.c_str() + 1, NULL, 16);
		if (spec.size() >= 9) col = C(v & 255, (v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255);       // #rrggbbaa
		else col = C(255, (v >> 16) & 255, (v >> 8) & 255, v & 255);
	} else if (!spec.empty() && spec[0] == '@') {
		std::string name = spec.substr(1), who;
		size_t c = name.find(':');
		if (c != std::string::npos) { who = name.substr(c + 1); name.resize(c); }
		bool has;
		if (name == "gold") col = ACCENT;
		else if (name == "white") col = WHITE;
		else if (name == "grey") col = GREY;
		else if (name == "red") col = RED;
		else if (name == "green") col = GREEN;
		else if (name == "bg") col = BG;
		else if (name == "dashbg") col = C(235, 10, 12, 16);
		else if (name == "dim") col = C(255, 52, 56, 64);
		else if (name == "car" || name == "other" || name == "top" || name == "bottom" || name == "leader") col = LayPaint(cx, name, has);
		else if (name == "dash" || name == "dashfrom" || name == "dashto") {
			AccentFor af(LayNumber(cx, who.empty() ? cx.accent : who));
			Color to = C(255, (S.dashAccent2 >> 16) & 255, (S.dashAccent2 >> 8) & 255, S.dashAccent2 & 255), from = DashAccent(), art;
			if (ArtColor(art)) { from = MixWhite(art, 0.12f); to = Darker(from, 0.35f); }
			col = name == "dash" ? DashAccent() : name == "dashfrom" ? from : to;
		}
	}
	if (opacity < 1) col = Color((BYTE)(col.GetA() * std::max(0.0, opacity)), col.GetR(), col.GetG(), col.GetB());
	return col;
}

// ---------------------------------------------------------------- pictures and fonts
// where a layout's picture is: a file in layouts\\images, or "art:..." in the selected art folder (read only):
// art:@series/manufacturers/x.png = the series folder now in use, art:images/ticker/x.png = under an art folder's root
static std::string LayPicPath(const std::string &file) {
	if (file.empty() || file.find("..") != std::string::npos || file.find(':', file.compare(0, 4, "art:") == 0 ? 4 : 0) != std::string::npos) return "";
	if (file.compare(0, 4, "art:") == 0) {
		std::string rel = file.substr(4);
		for (auto &c : rel) if (c == '/') c = '\\';
		if (rel.empty() || rel[0] == '\\') return "";
		if (rel.compare(0, 7, "@series") == 0 && (rel.size() == 7 || rel[7] == '\\')) return g_artSeriesDir.empty() ? "" : g_artSeriesDir + rel.substr(7);
		for (auto &r : g_artRoots) { std::string p = r + "\\" + rel; if (IsFile(p)) return p; }
		return "";
	}
	if (file[0] == '\\' || file[0] == '/') return "";
	return LayDir() + "\\images\\" + file;
}
static Image *LayImg(const std::string &file) {
	std::string path = LayPicPath(file);
	if (path.empty()) return nullptr;
	struct E { Image *img = nullptr; ULONGLONG stamp = 0; double checked = -10; };
	static std::map<std::string, E> cache;
	E &c = cache[path];
	double now = Now();
	if (c.img && now - c.checked < 1.0) return c.img;
	c.checked = now;
	WIN32_FILE_ATTRIBUTE_DATA fa;
	if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) return c.img;
	ULONGLONG stamp = (((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime) ^ fa.nFileSizeLow;
	if (c.img && c.stamp == stamp) return c.img;
	std::string data = ReadAll(path);                // read whole and let go of the file, so GIMP can export over it while we run
	if (data.empty()) return c.img;
	HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, data.size());
	if (!hg) return c.img;
	memcpy(GlobalLock(hg), data.data(), data.size()); GlobalUnlock(hg);
	IStream *st = nullptr;
	if (CreateStreamOnHGlobal(hg, TRUE, &st) != S_OK) { GlobalFree(hg); return c.img; }
	Bitmap *copy = nullptr;
	{
		Bitmap src(st, FALSE);
		if (src.GetLastStatus() == Ok && src.GetWidth() > 0 && src.GetHeight() > 0) {
			copy = new Bitmap((INT)src.GetWidth(), (INT)src.GetHeight(), PixelFormat32bppPARGB);
			Graphics g(copy);
			g.SetCompositingMode(CompositingModeSourceCopy);
			g.DrawImage(&src, Rect(0, 0, (INT)src.GetWidth(), (INT)src.GetHeight()), 0, 0, (INT)src.GetWidth(), (INT)src.GetHeight(), UnitPixel);
		}
	}
	st->Release();
	if (copy) { delete c.img; c.img = copy; c.stamp = stamp; }
	return c.img;
}
static const FontFamily *LayFam(const std::string &name) {
	LayLoadFonts();                                  // (first use: after the art folders are known, for the companion app's fonts)
	static std::map<std::string, FontFamily *> cache;
	auto it = cache.find(name);
	if (it != cache.end()) return it->second;
	FontFamily *f = nullptr;
	std::wstring w;
	if (name.empty() || name == "ui") w = L"Segoe UI";
	else if (name == "dash") w = g_condensed;
	else w = std::wstring(name.begin(), name.end());
	if (g_layPfc && g_layPfc->GetFamilyCount() > 0) { f = new FontFamily(w.c_str(), g_layPfc); if (!f->IsAvailable()) { delete f; f = nullptr; } }
	if (!f) { f = new FontFamily(w.c_str()); if (!f->IsAvailable()) { delete f; f = new FontFamily(L"Segoe UI"); } }
	return cache[name] = f;
}

// ---------------------------------------------------------------- drawing the elements
static void LayPath(GraphicsPath &p, float x, float y, float w, float h, float r, float slant) {
	if (slant != 0) {
		p.AddLine(x, y, x + w, y); p.AddLine(x + w, y, x + w - slant, y + h); p.AddLine(x + w - slant, y + h, x, y + h);
		p.CloseFigure();
	} else if (r > 0) {
		r = std::min(r, std::min(w, h) / 2);
		p.AddArc(x, y, r * 2, r * 2, 180, 90); p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
		p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90); p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
		p.CloseFigure();
	} else p.AddRectangle(RectF(x, y, w, h));
}
static std::string LayUpper(std::string s) { for (auto &c : s) c = (char)toupper((unsigned char)c); return s; }

static void LayOrdinal(Graphics &gr, int n, float x, float y, float size, const std::string &under, const FontFamily &fam, Color col) {
	std::string ord = Ordinal(n);
	size_t cut = ord.size() - 2;
	int style = FontStyleBold | FontStyleItalic;
	FitText(gr, ord.substr(0, cut), fam, size, style, col, x, y, StringAlignmentNear, 0, 0);
	std::wstring digits(ord.begin(), ord.begin() + cut);
	Font font(&fam, size, style, UnitPixel);
	RectF box;
	StringFormat tight(StringFormat::GenericTypographic());
	gr.MeasureString(digits.c_str(), -1, &font, PointF(0, 0), &tight, &box);
	FitText(gr, ord.substr(cut), fam, size * 0.44f, style, col, x + box.Width + size * 0.24f, y + size * 0.12f, StringAlignmentNear, 0, 0);
	if (!under.empty()) FitText(gr, under, fam, size * 0.41f, style, col, x + 2, y + size * 1.06f, StringAlignmentNear, 0, 0);
}

// the driver photo, cropped in on the head and shoulders (zoom, else headshot_zoom); with tab on, the number card on an
// offset fill across its bottom edge (the look RaceVision always had); tab off = the photo alone, to place the number yourself
static void LayPortrait(Graphics &gr, const JV &e, int number, float x, float y, float w, float h, Color fill, const wchar_t *dashFont) {
	bool tab = e.flag("tab", true);
	float offset = tab ? (float)e.num("offset", 8) : 0, zoom = (float)(e.has("zoom") ? e.num("zoom", 1.35) : S.headshotZoom);
	if (zoom < 0.5f) zoom = 0.5f;
	if (e.has("back")) { SolidBrush bb(LayColor(e.str("back"), LCtx(), 1)); gr.FillRectangle(&bb, x, y, w, h); }
	Image *head = Headshot(number);
	if (head) {
		float iw = (float)head->GetWidth(), ih = (float)head->GetHeight(), aspect = w / h;
		float sw = std::min(iw, ih * aspect) / zoom, sh = sw / aspect;
		float sx = (iw - sw) / 2, sy = std::min(ih * (float)e.num("top", 0.03), std::max(0.f, ih - sh));
		GraphicsState st = gr.Save();
		gr.SetClip(RectF(x, y, w, h));
		gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
		gr.DrawImage(head, RectF(x, y, w, h), sx, sy, sw, sh, UnitPixel);
		gr.Restore(st);
	}
	if (!tab) return;
	float ph = h * 0.30f, py = y + h - ph;
	SolidBrush plate(fill);
	gr.FillRectangle(&plate, x - offset, py, w + offset, ph);
	Image *card = NumberArt(number);
	if (card) DrawFit(gr, card, x - offset + 4, py + 2, w + offset - 8, ph - 4);
	else if (dashFont) { FontFamily fam(dashFont); FitText(gr, NumLabel(number), fam, ph, FontStyleBold | FontStyleItalic, ACCENT, x - offset, py - 2, StringAlignmentCenter, w + offset, 0); }
	else Text(gr, "#" + NumLabel(number), x - offset, py + 1, ph * 0.8f, ACCENT, true, StringAlignmentCenter, w + offset);
}

static void LayElement(Graphics &gr, const JV &e, LCtx &cx, float dy) {
	std::string show = e.str("show");
	if (!show.empty() && !LayShow(show, cx)) return;
	if (e.flag("hidden")) return;
	std::string type = e.str("type");
	float x = (float)e.num("x"), y = (float)e.num("y") + dy, w = (float)e.num("w"), h = (float)e.num("h"), size = (float)e.num("size", 20);
	double op = e.has("opacity") ? e.num("opacity", 1) : 1;
	auto col = [&](const char *k, const char *d) { return LayColor(e.str(k, d), cx, op); };
	bool dashFont = e.str("font") == "dash";
	int style = (e.flag("bold", true) ? FontStyleBold : 0) | (e.flag("italic", dashFont) ? FontStyleItalic : 0);

	if (type == "rect") {
		GraphicsPath p;
		float sw = (float)e.num("strokeW", 0);
		LayPath(p, x, y, w, h, (float)e.num("radius"), (float)e.num("slant"));
		if (e.has("fill")) {
			Color a = col("fill", "#ffffff");
			if (e.has("fill2")) { LinearGradientBrush g(PointF(x, y), PointF(x + w, y + h), a, col("fill2", "#000000")); gr.FillPath(&g, &p); }
			else { SolidBrush b(a); gr.FillPath(&b, &p); }
		}
		if (e.has("stroke") && sw > 0) {
			GraphicsPath q;
			LayPath(q, x + sw / 2, y + sw / 2, w - sw, h - sw, std::max(0.f, (float)e.num("radius") - sw / 2), 0);
			Pen pen(col("stroke", "#ffffff"), sw);
			gr.DrawPath(&pen, &q);
		}
	} else if (type == "text") {
		bool miss = false;
		std::string s = LayExpand(e.str("text"), cx, &miss);
		if (miss) return;
		if (e.flag("upper")) s = LayUpper(s);
		if (s.empty()) return;
		std::string al = e.str("align", "left");
		FitText(gr, s, *LayFam(e.str("font")), size, style, col("color", "#ffffff"), x, y,
		        al == "center" ? StringAlignmentCenter : al == "right" ? StringAlignmentFar : StringAlignmentNear, w, 0);
	} else if (type == "run") {
		std::vector<std::pair<std::string, Color>> run;
		const JV *parts = e.get("parts");
		if (!parts) return;
		for (const JV &p : parts->a) {
			if (p.flag("story")) {
				LayFacts(cx);
				if (!cx.facts.empty()) for (auto &q : cx.facts[cx.factN].parts) run.push_back({ q.first, q.second });
			} else {
				bool miss = false;
				std::string t = LayExpand(p.str("text"), cx, &miss);
				if (!miss) run.push_back({ t, LayColor(p.str("color", "#ffffff"), cx, op) });
			}
		}
		if (e.flag("upper")) for (auto &p : run) p.first = LayUpper(p.first);
		bool any = false;
		for (auto &p : run) if (!Trim(p.first).empty()) any = true;
		if (any) DrawRun(gr, run, *LayFam(e.str("font")), size, style, x, y, w);
	} else if (type == "ordinal") {
		LV v;
		if (!LayVal(e.str("src"), cx, v) || !v.isNum) return;
		LayOrdinal(gr, (int)v.n, x, y, size, LayExpand(e.str("under"), cx), *LayFam(e.str("font", "dash")), col("color", "#ffffff"));
	} else if (type == "image") {
		Image *img = nullptr;
		std::string art = e.str("art");
		if (!art.empty()) {
			Drv d = LayWho(cx, e.str("who", "car"));
			if (!d.ok) return;
			int num = d.slot >= 0 ? g_car[d.slot].number : SMP[d.sidx].num;
			img = art == "headshot" ? Headshot(num) : NumberArt(num);
			if (img) DrawFit(gr, img, x, y, w, h, e.flag("alignRight"));
		} else {
			img = LayImg(e.str("file"));
			if (!img) return;
			ImageAttributes ia;
			if (op < 1) { ColorMatrix m = { 1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0, 0,0,0,(REAL)op,0, 0,0,0,0,1 }; ia.SetColorMatrix(&m); }
			if (w <= 0) w = (float)img->GetWidth();
			if (h <= 0) h = (float)img->GetHeight();
			gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
			gr.SetPixelOffsetMode(PixelOffsetModeHalf);
			if (e.str("fit") == "contain") {
				float k = std::min(w / img->GetWidth(), h / img->GetHeight()), dw = img->GetWidth() * k, dh = img->GetHeight() * k;
				gr.DrawImage(img, RectF(x + (w - dw) / 2, y + (h - dh) / 2, dw, dh), 0, 0, (REAL)img->GetWidth(), (REAL)img->GetHeight(), UnitPixel, &ia);
			} else gr.DrawImage(img, RectF(x, y, w, h), 0, 0, (REAL)img->GetWidth(), (REAL)img->GetHeight(), UnitPixel, &ia);
		}
	} else if (type == "portrait") {
		Drv d = LayWho(cx, e.str("who", "car"));
		if (!d.ok) return;
		int num = d.slot >= 0 ? g_car[d.slot].number : SMP[d.sidx].num;
		LayPortrait(gr, e, num, x, y, w, h, LayColor(e.str("fill", "#0a0c10eb"), cx, op), dashFont ? g_condensed.c_str() : nullptr);
	} else if (type == "bar" || type == "segbar" || type == "arc" || type == "dial" || type == "needle") {
		LV v;
		if (!LayVal(e.str("src"), cx, v) || !v.isNum) return;
		double mn = e.num("min", 0), mx = e.num("max", 1);
		float frac = (float)std::min(1.0, std::max(0.0, (v.n - mn) / (mx - mn != 0 ? mx - mn : 1)));
		Color fill = col("color", "#ffc400"), track = col("track", "#343840");
		auto setAlpha = [&](Image *img, ImageAttributes &ia) { if (op < 1) { ColorMatrix m = { 1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0, 0,0,0,(REAL)op,0, 0,0,0,0,1 }; ia.SetColorMatrix(&m); } };
		if (type == "needle") {                       // a picture turned about a pivot: from + sweep * value degrees
			Image *img = LayImg(e.str("file"));
			if (!img) return;
			float ang = (float)e.num("from", -120) + (float)e.num("sweep", 240) * frac;
			float pw = (w > 0 ? w : (float)img->GetWidth()), ph = (h > 0 ? h : (float)img->GetHeight());
			float px = (float)e.num("pivotX", 0.5) * pw, py = (float)e.num("pivotY", 0.5) * ph;
			GraphicsState st = gr.Save();
			gr.TranslateTransform(x + px, y + py);
			gr.RotateTransform(ang);
			ImageAttributes ia; setAlpha(img, ia);
			gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
			gr.DrawImage(img, RectF(-px, -py, pw, ph), 0, 0, (REAL)img->GetWidth(), (REAL)img->GetHeight(), UnitPixel, &ia);
			gr.Restore(st);
		} else if ((type == "bar" || type == "dial") && (e.has("fillFile") || e.has("trackFile"))) {
			// a picture-made bar or ring: the empty picture whole, the full picture shown only as far as the value
			Image *tr = LayImg(e.str("trackFile")), *fl = LayImg(e.str("fillFile"));
			ImageAttributes ia; setAlpha(nullptr, ia);
			auto put = [&](Image *img) { gr.DrawImage(img, RectF(x, y, w, h), 0, 0, (REAL)img->GetWidth(), (REAL)img->GetHeight(), UnitPixel, &ia); };
			gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
			gr.SetPixelOffsetMode(PixelOffsetModeHalf);
			if (tr) put(tr);
			if (fl && frac > 0) {
				GraphicsState st = gr.Save();
				if (type == "dial") {
					GraphicsPath pie;
					float from = (float)e.num("from", -90), sweep = (float)e.num("sweep", 360) * frac;
					float cx0 = x + (float)e.num("pivotX", 0.5) * w, cy0 = y + (float)e.num("pivotY", 0.5) * h, R = std::max(w, h) * 2;
					pie.AddPie(cx0 - R, cy0 - R, R * 2, R * 2, from, sweep);
					gr.SetClip(&pie);
				} else {
					std::string dir = e.str("dir", "right");
					RectF r(x, y, w, h);
					if (dir == "left") r = RectF(x + w * (1 - frac), y, w * frac, h);
					else if (dir == "up") r = RectF(x, y + h * (1 - frac), w, h * frac);
					else if (dir == "down") r = RectF(x, y, w, h * frac);
					else r = RectF(x, y, w * frac, h);
					gr.SetClip(r);
				}
				put(fl);
				gr.Restore(st);
			}
		} else if (type == "bar") {
			SolidBrush tb(track), fb(fill);
			if (e.flag("vertical")) { gr.FillRectangle(&tb, x, y, w, h); gr.FillRectangle(&fb, x, y + h * (1 - frac), w, h * frac); }
			else { gr.FillRectangle(&tb, x, y, w, h); gr.FillRectangle(&fb, x, y, w * frac, h); }
		} else if (type == "segbar") {
			int segs = (int)std::min(80.0, std::max(1.0, e.num("segs", 12))), lit = (int)(frac * segs + 0.5f), hot = (int)e.num("hot", 0);
			float gap = (float)e.num("gap", 1), pitch = (e.flag("vertical") ? h : w) / segs;
			Color c2 = col("color2", e.str("color", "#ffc400").c_str()), ch = col("hotColor", "#eb3c28");
			for (int k = 0; k < segs; k++) {
				float t = segs > 1 ? k / (float)(segs - 1) : 0;
				Color c = k >= lit ? track : k >= segs - hot ? ch
				        : Color(fill.GetA(), (BYTE)(fill.GetR() + (c2.GetR() - fill.GetR()) * t), (BYTE)(fill.GetG() + (c2.GetG() - fill.GetG()) * t), (BYTE)(fill.GetB() + (c2.GetB() - fill.GetB()) * t));
				SolidBrush sb(c);
				if (e.flag("vertical")) gr.FillRectangle(&sb, x, y + h - (k + 1) * pitch + gap, w, pitch - gap);
				else gr.FillRectangle(&sb, x + k * pitch, y, pitch - gap, h);
			}
		} else {
			float from = (float)e.num("from", 150), sweep = (float)e.num("sweep", 240), thick = (float)e.num("thick", 12);
			int segs = (int)std::min(80.0, std::max(1.0, e.num("segs", 24)));
			Pen tp(track, thick);
			gr.DrawArc(&tp, x, y, w, h, from, sweep);
			Color c2 = col("color2", e.str("color", "#ffc400").c_str());
			for (int k = 0; k < segs && k < (int)(frac * segs + 0.5f); k++) {
				float t = segs > 1 ? k / (float)(segs - 1) : 0;
				Pen sp(Color(fill.GetA(), (BYTE)(fill.GetR() + (c2.GetR() - fill.GetR()) * t), (BYTE)(fill.GetG() + (c2.GetG() - fill.GetG()) * t), (BYTE)(fill.GetB() + (c2.GetB() - fill.GetB()) * t)), thick);
				gr.DrawArc(&sp, x, y, w, h, from + sweep * k / segs, sweep / segs + 0.6f);
			}
		}
	}
}

static void LayDraw(Graphics &gr, const Layout &L, LCtx &cx) {
	cx.accent = L.accent;
	const JV *els = L.doc.get("elements");
	if (!els) return;
	double e = cx.slide * cx.slide * (3 - 2 * cx.slide), off = (1 - e) * L.rowGap;
	for (const JV &el : els->a) {
		std::string row = el.str("row");
		LayElement(gr, el, cx, row == "top" ? (float)off : row == "bottom" ? (float)-off : 0.f);
	}
}

// the panel's own drawing, from its layout; false = no layout (the old code draws it)
static bool LayDrawPanel(Graphics &gr, Panel &p, bool sample) {
	const Layout *L = LayoutFor(p);
	if (!L) return false;
	LCtx cx;
	cx.sample = sample;
	cx.car = sample ? -1 : OvCar(); cx.other = sample ? -1 : g_shot.other;
	if (L->panel == "battle") {
		if (cx.other < 0 || cx.car < 0) cx.sample = true;
		cx.battle = true;
		if (!cx.sample) { cx.top = cx.other; cx.bottom = g_shot.car; BattleOrder(cx.other, g_shot.car, cx.top, cx.bottom, cx.slide); }
	}
	LayDraw(gr, *L, cx);
	return true;
}

// ---------------------------------------------------------------- custom panels (tickers and the like): when they show
// the layout's "when": words that must all hold - always, race, green, onboard, offboard, battle, replay
static bool LayWhen(const Layout &L, bool onboard, bool battleShot) {
	std::string w = L.when;
	for (auto &c : w) c = (char)tolower((unsigned char)c);
	size_t i = 0;
	while (i < w.size()) {
		while (i < w.size() && (w[i] == ' ' || w[i] == ',')) i++;
		size_t e = i;
		while (e < w.size() && w[e] != ' ' && w[e] != ',') e++;
		std::string t = w.substr(i, e - i);
		i = e;
		if (t.empty() || t == "always") continue;
		bool ok = t == "race" ? g_session == SESSION_RACE : t == "green" ? g_session == SESSION_RACE && g_flag == FLAG_GREEN
		        : t == "onboard" ? onboard : t == "offboard" ? !onboard : t == "battle" ? battleShot : t == "replay" ? OvReplay() : true;
		if (!ok) return false;
	}
	return true;
}
static bool LayCustomShow(Panel &p, bool onboard, bool battleShot) {
	const Layout *L = LayoutFor(p);
	if (!L) return false;
	if (g_edit) return true;
	if (!g_overlaysOn || (!OvReplay() && (g_shot.car < 0 || !g_car[g_shot.car].valid)) ) return false;
	return LayWhen(*L, onboard, battleShot);
}

// ---------------------------------------------------------------- the editor's calls (/api/overlay/...)
struct SrcDef { const char *key, *label, *kind; };
static const SrcDef LAY_CAR_FIELDS[] = {
	{ "place", "Place (number)", "num" }, { "place_p", "Place as P12", "text" }, { "place_ord", "Place as 12TH", "text" },
	{ "number", "Car number", "text" }, { "first", "First name", "text" }, { "last", "Last name", "text" },
	{ "full", "Full name", "text" }, { "short", "Short name (last)", "text" },
	{ "speed_mph", "Speed, mph", "num" }, { "speed_kph", "Speed, km/h", "num" }, { "rpm", "RPM", "num" },
	{ "rpm_frac", "RPM, share of the highest seen (0-1)", "num" }, { "rpm_10k", "RPM / 10000 (0-1)", "num" },
	{ "gear", "Gear", "text" }, { "in_gear", "In a forward gear (yes/no)", "bool" },
	{ "throttle", "Throttle (0-1)", "num" }, { "brake", "Brake (0-1)", "num" },
	{ "throttle_pct", "Throttle, percent (0-100)", "num" }, { "brake_pct", "Brake, percent (0-100)", "num" },
	{ "steer", "Steering, degrees (+ = left)", "num" }, { "steer_abs", "Steering, degrees (no sign)", "num" },
	{ "laps", "Laps done", "num" }, { "laps_led", "Laps led", "num" }, { "pits", "Pit stops", "num" },
	{ "start_pos", "Started in (place)", "num" }, { "places_gained", "Places gained since the start", "text" },
	{ "last_lap", "Last lap time", "text" }, { "best_lap", "Best lap time", "text" },
	{ "interval", "Gap to the car ahead", "text" }, { "gap_leader", "Gap to the leader", "text" },
	{ "pit", "In the pits (yes/no)", "bool" }, { "color", "Has a paint colour (yes/no)", "bool" },
	{ "has_card", "Has number-card art (yes/no)", "bool" }, { "valid", "Is on track (yes/no)", "bool" } };
static const SrcDef LAY_GLOBALS[] = {
	{ "shot.sub", "What he is doing (battle line, REPLAY)", "text" }, { "story.label", "Race story: place plate", "text" },
	{ "story.any", "A race story is available (yes/no)", "bool" },
	{ "battle.title", "Battle title: BATTLE FOR P2", "text" }, { "battle.title_ord", "Battle title: BATTLE FOR 2ND", "text" },
	{ "battle.place", "Battle place", "num" }, { "battle.gap", "Gap between the two (seconds)", "num" }, { "battle.metres", "Gap between the two (metres)", "num" },
	{ "replay", "A replay is playing (yes/no)", "bool" },
	{ "race.lap", "Leader's lap", "num" }, { "race.laps_total", "Race length", "num" }, { "race.laps_to_go", "Laps to go", "num" }, { "race.progress", "Race distance done (0-1)", "num" },
	{ "race.green", "Green flag (yes/no)", "bool" }, { "race.flag", "Flag", "text" }, { "track.name", "Track", "text" } };

static const char *LAY_PANELS[][2] = { { "tag", "Driver tag" }, { "battle", "Battle box" }, { "onboard", "On-board gauges" }, { "dash", "In-car dash" },
                                        { "custom1", "Custom 1" }, { "custom2", "Custom 2" }, { "custom3", "Custom 3" }, { "custom4", "Custom 4" } };

static std::string LayPng(Bitmap &bmp) {
	UINT n = 0, size = 0;
	GetImageEncodersSize(&n, &size);
	if (!size) return "";
	std::vector<BYTE> buf(size);
	ImageCodecInfo *enc = (ImageCodecInfo *)buf.data();
	GetImageEncoders(n, size, enc);
	CLSID png = {};
	for (UINT k = 0; k < n; k++) if (wcscmp(enc[k].MimeType, L"image/png") == 0) png = enc[k].Clsid;
	IStream *st = nullptr;
	if (CreateStreamOnHGlobal(NULL, TRUE, &st) != S_OK) return "";
	std::string out;
	if (bmp.Save(st, &png, NULL) == Ok) {
		HGLOBAL hg = NULL;
		GetHGlobalFromStream(st, &hg);
		SIZE_T len = GlobalSize(hg);
		STATSTG ss; if (st->Stat(&ss, STATFLAG_NONAME) == S_OK && ss.cbSize.QuadPart < len) len = (SIZE_T)ss.cbSize.QuadPart;
		out.assign((const char *)GlobalLock(hg), len);
		GlobalUnlock(hg);
	}
	st->Release();
	return out;
}

static std::string LayErr(const std::string &m) { std::string o = "{\"error\":"; JQuote(o, m); return o + "}"; }
static std::string LayUsed(const std::string &name) {      // which panels use this layout now
	std::string o;
	for (auto &pn : LAY_PANELS) {
		Panel *p = nullptr;
		for (Panel *q : g_panels) if (!strcmp(q->key, pn[0])) p = q;
		if (!p) continue;
		const Layout *L = LayoutFor(*p);
		if (L && L->name == name) o += (o.empty() ? "" : ",") + std::string(pn[0]);
	}
	return o;
}
static bool LayWriteFile(const std::string &path, const std::string &data) {
	std::string tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f) return false;
	bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	fclose(f);
	if (!ok) { DeleteFileA(tmp.c_str()); return false; }
	return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
static std::string LayDefaultJson(const std::string &name) {
	for (const LayDefault &d : LAY_DEFAULTS) if (d.json && name == d.name) return d.json;
	if (!LayName(name)) return "";
	std::string exeDir = g_ini;
	size_t k = exeDir.find_last_of("\\/");
	exeDir = k == std::string::npos ? std::string(".") : exeDir.substr(0, k);
	return ReadAll(exeDir + "\\layouts_stock\\" + name + ".json");           // the overlay pack's version
}

// the pictures in the art folder, for the editor's picker: the series folder in use (not the thousands of numbers and
// headshots: those have their own elements) and each art root's images\\ tree
static bool LayIsPic(const std::string &n) {
	std::string lo = n;
	for (auto &c : lo) c = (char)tolower((unsigned char)c);
	return lo.size() > 4 && (lo.substr(lo.size() - 4) == ".png" || lo.substr(lo.size() - 4) == ".jpg" || lo.substr(lo.size() - 5) == ".jpeg");
}
static void LayWalk(const std::string &base, const std::string &rel, int depth, const std::string &prefix, JV &out, int &budget) {
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((base + (rel.empty() ? "" : "\\" + rel) + "\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		std::string n = fd.cFileName;
		if (n == "." || n == "..") continue;
		std::string r = rel.empty() ? n : rel + "/" + n;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			std::string lo = n;
			for (auto &c : lo) c = (char)tolower((unsigned char)c);
			if (depth > 0 && lo != "numbers" && lo != "headshots" && lo != "flags_cache") LayWalk(base, r, depth - 1, prefix, out, budget);
		} else if (LayIsPic(n) && budget > 0 && fd.nFileSizeLow < 6000000) {
			budget--;
			JV o = JV::Obj_();
			o.set("path", JV::Str_("art:" + prefix + r));
			size_t sl = r.find_last_of('/');
			o.set("group", JV::Str_((prefix == "@series/" ? "series: " : "") + (sl == std::string::npos ? std::string("(top)") : r.substr(0, sl))));
			out.a.push_back(o);
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
}

static std::string LayApi(const std::string &sub, std::map<std::string, std::string> &q, const std::string &body, int &code, std::string &ctype) {
	ctype = "application/json; charset=utf-8";
	code = 200;
	EnterCriticalSection(&g_lock);
	LayEnsure();
	LayPoll();
	std::string out;
	std::string name = q["name"];
	if (sub.empty()) {
		JV r = JV::Obj_();
		r.set("dir", JV::Str_(LayDir()));
		JV ls = JV::Arr_();
		for (auto &kv : g_lays) {
			JV l = JV::Obj_();
			l.set("name", JV::Str_(kv.first)); l.set("title", JV::Str_(kv.second.title)); l.set("panel", JV::Str_(kv.second.panel));
			l.set("w", JV::Num_(kv.second.w)); l.set("h", JV::Num_(kv.second.h));
			l.set("builtin", JV::Bool_(!LayDefaultJson(kv.first).empty())); l.set("used", JV::Str_(LayUsed(kv.first)));
			ls.a.push_back(l);
		}
		r.set("layouts", ls);
		JV sts = JV::Arr_();
		for (auto &kv : g_styles) { JV o = JV::Obj_(); o.set("name", JV::Str_(kv.first)); o.set("title", JV::Str_(kv.second.title)); o.set("base", JV::Str_(kv.second.base)); sts.a.push_back(o); }
		r.set("styles", sts); r.set("style", JV::Str_(S.overlayStyle));
		JV bad = JV::Arr_();
		for (auto &kv : g_layBad) bad.a.push_back(JV::Str_(kv.first + ": " + kv.second));
		r.set("broken", bad);
		JV ps = JV::Arr_();
		for (auto &pn : LAY_PANELS) {
			Panel *p = nullptr;
			for (Panel *q2 : g_panels) if (!strcmp(q2->key, pn[0])) p = q2;
			if (!p) continue;
			JV o = JV::Obj_();
			auto it = S.raw.find(std::string("layout_") + pn[0]);
			const Layout *L = LayoutFor(*p);
			o.set("key", JV::Str_(pn[0])); o.set("label", JV::Str_(pn[1]));
			o.set("set", JV::Str_(it == S.raw.end() ? "" : Trim(it->second)));
			o.set("using", JV::Str_(L ? L->name : ""));
			ps.a.push_back(o);
		}
		r.set("panels", ps);
		JV src = JV::Obj_(), cf = JV::Arr_(), gl = JV::Arr_();
		for (auto &d : LAY_CAR_FIELDS) { JV o = JV::Obj_(); o.set("key", JV::Str_(d.key)); o.set("label", JV::Str_(d.label)); o.set("kind", JV::Str_(d.kind)); cf.a.push_back(o); }
		for (auto &d : LAY_GLOBALS) { JV o = JV::Obj_(); o.set("key", JV::Str_(d.key)); o.set("label", JV::Str_(d.label)); o.set("kind", JV::Str_(d.kind)); gl.a.push_back(o); }
		src.set("car", cf); src.set("global", gl);
		JV whos = JV::Arr_();
		for (const char *w : { "car", "other", "top", "bottom", "leader", "ahead", "behind", "p1", "p2", "p3", "p4", "p5", "p6", "p7", "p8", "p9", "p10" }) whos.a.push_back(JV::Str_(w));
		src.set("whos", whos);
		r.set("sources", src);
		LayLoadFonts();
		JV fonts = JV::Arr_(), sysf = JV::Arr_();
		for (auto &f : g_layFontFiles) { JV o = JV::Obj_(); o.set("family", JV::Str_(f.family)); o.set("file", JV::Str_(f.file)); o.set("mine", JV::Bool_(f.mine)); fonts.a.push_back(o); }
		for (auto &f : g_laySysFonts) sysf.a.push_back(JV::Str_(f));
		r.set("fonts", fonts); r.set("sysfonts", sysf);
		r.set("artSeries", JV::Str_(g_artSeriesDir.empty() ? "" : g_artSeries));
		JV imgs = JV::Arr_();
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA((LayDir() + "\\images\\*.*").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				std::string n = fd.cFileName, lo = n;
				for (auto &c : lo) c = (char)tolower((unsigned char)c);
				if (lo.size() > 4 && (lo.substr(lo.size() - 4) == ".png" || lo.substr(lo.size() - 4) == ".jpg" || lo.substr(lo.size() - 5) == ".jpeg")) imgs.a.push_back(JV::Str_(n));
			} while (FindNextFileA(h, &fd));
			FindClose(h);
		}
		r.set("images", imgs);
		bool live = g_shot.car >= 0 && g_car[g_shot.car].valid;
		r.set("live", JV::Bool_(live));
		out = JText(r);
	} else if (sub == "pictures") {
		JV r = JV::Obj_(), art = JV::Arr_(), mine = JV::Arr_();
		if (g_artSeriesDir.empty()) { ArtRoots(); PickSeries(); }          // (the series is normally picked once cars are on track)
		int budget = 1200;
		if (!g_artSeriesDir.empty()) LayWalk(g_artSeriesDir, "", 2, "@series/", art, budget);
		for (auto &root : g_artRoots) { LayWalk(root + "\\images", "", 3, "images/", art, budget); }
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA((LayDir() + "\\images\\*.*").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE) { do { if (LayIsPic(fd.cFileName)) mine.a.push_back(JV::Str_(fd.cFileName)); } while (FindNextFileA(h, &fd)); FindClose(h); }
		r.set("mine", mine); r.set("art", art); r.set("series", JV::Str_(g_artSeries));
		out = JText(r);
	} else if (sub == "image" || sub == "fontfile") {
		std::string data, path;
		if (sub == "image") path = LayPicPath(q["src"]);
		else for (auto &f : g_layFontFiles) if (f.file == q["name"]) path = f.path;
		if (!path.empty()) data = ReadAll(path);
		if (data.empty()) { code = 404; out = LayErr("No such file."); }
		else {
			out = data;
			bool jpg = data.size() > 2 && (unsigned char)data[0] == 0xFF;
			ctype = sub == "fontfile" ? "font/ttf" : jpg ? "image/jpeg" : "image/png";
		}
	} else if (sub == "layout") {
		auto it = g_lays.find(name);
		if (it == g_lays.end()) { code = 404; out = LayErr("No such layout."); }
		else out = JText(it->second.doc);
	} else if (sub == "save") {
		JV doc;
		if (!LayName(name)) { code = 400; out = LayErr("Layout names are letters, digits, _ and - (up to 40)."); }
		else if (!JParse(body, doc) || doc.t != JV::Obj) { code = 400; out = LayErr("That layout is not valid JSON."); }
		else {
			const JV *els = doc.get("elements");
			if (!els || els->t != JV::Arr || els->a.size() > 400) { code = 400; out = LayErr("A layout needs an elements list (up to 400)."); }
			else if (!LayWriteFile(LayDir() + "\\" + name + ".json", JText(doc))) { code = 400; out = LayErr("Could not write the layout file."); }
			else { LayLoad(); Log("-- overlay layout %s saved --\n", name.c_str()); out = "{\"ok\":true}"; }
		}
	} else if (sub == "delete") {
		if (!g_lays.count(name)) { code = 404; out = LayErr("No such layout."); }
		else if (!LayDefaultJson(name).empty()) { code = 400; out = LayErr("Built-in layouts can be reset, not deleted."); }
		else if (!LayUsed(name).empty()) { code = 400; out = LayErr("A panel is using this layout: pick another for it first."); }
		else { DeleteFileA((LayDir() + "\\" + name + ".json").c_str()); LayLoad(); out = "{\"ok\":true}"; }
	} else if (sub == "reset") {
		std::string def = LayDefaultJson(name);
		JV doc;
		if (def.empty() || !JParse(def, doc)) { code = 400; out = LayErr("That layout has no built-in version."); }
		else { LayWriteFile(LayDir() + "\\" + name + ".json", JText(doc)); LayLoad(); out = "{\"ok\":true}"; }
	} else if (sub == "duplicate") {
		std::string as = q["as"];
		auto it = g_lays.find(name);
		if (it == g_lays.end()) { code = 404; out = LayErr("No such layout."); }
		else if (!LayName(as)) { code = 400; out = LayErr("Layout names are letters, digits, _ and - (up to 40)."); }
		else if (g_lays.count(as)) { code = 400; out = LayErr("There is a layout called that already."); }
		else {
			JV doc = it->second.doc;
			doc.set("title", JV::Str_(as));
			LayWriteFile(LayDir() + "\\" + as + ".json", JText(doc)); LayLoad(); out = "{\"ok\":true}";
		}
	} else if (sub == "stylesave") {
		// the look as it is now (each panel's layout) saved as a named style preset, and switched to
		std::string title = q["title"].empty() ? name : q["title"];
		if (!LayName(name)) { code = 400; out = LayErr("Style names are letters, digits, _ and - (up to 40)."); }
		else {
			JV doc = JV::Obj_(), map = JV::Obj_();
			doc.set("title", JV::Str_(title));
			doc.set("base", JV::Str_(q["base"] == "classic" ? "classic" : q["base"] == "dash" ? "dash" : DashLook() ? "dash" : "classic"));
			for (auto &pn : LAY_PANELS) {
				Panel *p = nullptr;
				for (Panel *q2 : g_panels) if (!strcmp(q2->key, pn[0])) p = q2;
				const Layout *L = p ? LayoutFor(*p) : nullptr;
				if (L) map.set(pn[0], JV::Str_(L->name));
			}
			doc.set("layouts", map);
			LayMkdir(LayDir() + "\\presets");
			if (!LayWriteFile(LayDir() + "\\presets\\" + name + ".json", JText(doc))) { code = 400; out = LayErr("Could not write the style."); }
			else {
				LayLoad();
				LayClearPanelKeys();
				S.overlayStyle = "preset:" + name; SaveKey("overlay_style", S.overlayStyle); LayApplySizes();
				out = "{\"ok\":true}";
			}
		}
	} else if (sub == "styleuse") {
		if (name != "dash" && name != "classic" && !g_styles.count(name)) { code = 404; out = LayErr("No such style."); }
		else {
			LayClearPanelKeys();
			S.overlayStyle = (name == "dash" || name == "classic") ? name : "preset:" + name;
			SaveKey("overlay_style", S.overlayStyle); LayApplySizes();
			out = "{\"ok\":true}";
		}
	} else if (sub == "styledelete") {
		if (!g_styles.count(name)) { code = 404; out = LayErr("No such style."); }
		else {
			DeleteFileA((LayDir() + "\\presets\\" + name + ".json").c_str());
			if (S.overlayStyle == "preset:" + name) { S.overlayStyle = "classic"; SaveKey("overlay_style", "classic"); }
			LayLoad(); out = "{\"ok\":true}";
		}
	} else if (sub == "use") {
		std::string panel = q["panel"];
		bool known = false;
		for (auto &pn : LAY_PANELS) if (panel == pn[0]) known = true;
		if (!known) { code = 400; out = LayErr("No such panel."); }
		else if (!name.empty() && !g_lays.count(name)) { code = 404; out = LayErr("No such layout."); }
		else { SaveKey("layout_" + panel, name); LayApplySizes(); out = "{\"ok\":true}"; }
	} else if (sub == "render") {
		JV doc;
		if (!JParse(body, doc) || doc.t != JV::Obj) { code = 400; out = LayErr("That layout is not valid JSON."); }
		else {
			Layout L; L.name = "draft"; L.doc = doc; LayFinish(L);
			float scale = (float)std::min(3.0, std::max(0.5, atof(q["s"].c_str()) > 0 ? atof(q["s"].c_str()) : 1.0));
			LCtx cx;
			bool live = q["live"] == "1" && g_shot.car >= 0 && g_car[g_shot.car].valid;
			cx.sample = !live;
			cx.car = live ? OvCar() : -1; cx.other = live ? g_shot.other : -1;
			if (L.panel == "battle") {
				cx.battle = true;
				if (!live || cx.other < 0) cx.sample = true;
				else { cx.top = cx.other; cx.bottom = g_shot.car; BattleOrder(cx.other, g_shot.car, cx.top, cx.bottom, cx.slide); }
			}
			Bitmap bmp((INT)(L.w * scale), (INT)(L.h * scale), PixelFormat32bppARGB);
			{
				Graphics gr(&bmp);
				gr.SetSmoothingMode(SmoothingModeAntiAlias);
				gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
				gr.Clear(C(0, 0, 0, 0));
				gr.ScaleTransform(scale, scale);
				LayDraw(gr, L, cx);
			}
			out = LayPng(bmp);
			ctype = "image/png";
			if (out.empty()) { code = 400; ctype = "application/json; charset=utf-8"; out = LayErr("Could not draw that."); }
		}
	} else if (sub == "upload") {
		std::string lo = name;
		for (auto &c : lo) c = (char)tolower((unsigned char)c);
		bool png = body.size() > 8 && !memcmp(body.data(), "\x89PNG", 4), jpg = body.size() > 4 && (unsigned char)body[0] == 0xFF && (unsigned char)body[1] == 0xD8;
		bool extOk = lo.size() > 4 && (lo.substr(lo.size() - 4) == ".png" || lo.substr(lo.size() - 4) == ".jpg");
		bool nameOk = !name.empty() && name.size() <= 60;
		for (char c : name) if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.' && c != ' ') nameOk = false;
		if (name.find("..") != std::string::npos) nameOk = false;
		if (!nameOk || !extOk || !(png || jpg)) { code = 400; out = LayErr("Upload a PNG or JPG (letters, digits, _ - . in the name)."); }
		else if (!LayWriteFile(LayDir() + "\\images\\" + name, body)) { code = 400; out = LayErr("Could not save the picture."); }
		else out = "{\"ok\":true}";
	} else { code = 404; out = LayErr("not found"); }
	LeaveCriticalSection(&g_lock);
	return out;
}
