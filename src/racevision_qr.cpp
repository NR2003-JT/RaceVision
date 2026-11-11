// QR code for the phone link (included by racevision_remote.cpp; stands alone for testing)
//
// Byte mode, error correction level L, versions 1-5 (one Reed-Solomon block each): up to
// 106 characters, plenty for http://<ip>:<port>/?k=<code>. Returns the dark modules, without
// the quiet zone, or nothing when the text is too long.

static std::vector<std::vector<bool>> MakeQr(const std::string &text) {
	static const int DATA[] = { 0, 19, 34, 55, 80, 108 }, ECC[] = { 0, 7, 10, 15, 20, 26 };
	int ver = 1;
	while (ver <= 5 && (int)text.size() + 2 > DATA[ver]) ver++;     // mode + count + data + terminator = n + 2 bytes
	if (ver > 5) return {};

	// the data codewords: mode 0100 (bytes), 8-bit count, the text, terminator, pad bytes
	std::vector<int> bits;
	auto put = [&](int v, int n) { for (int i = n - 1; i >= 0; i--) bits.push_back((v >> i) & 1); };
	put(4, 4);
	put((int)text.size(), 8);
	for (unsigned char c : text) put(c, 8);
	int cap = DATA[ver] * 8;
	put(0, std::min(4, cap - (int)bits.size()));
	while (bits.size() % 8) bits.push_back(0);
	for (int pad = 0xEC; (int)bits.size() < cap; pad ^= 0xEC ^ 0x11) put(pad, 8);
	std::vector<int> words;
	for (size_t i = 0; i < bits.size(); i += 8) {
		int v = 0;
		for (int j = 0; j < 8; j++) v = v << 1 | bits[i + j];
		words.push_back(v);
	}

	// error correction: Reed-Solomon over GF(256) with the polynomial 0x11D
	auto mul = [](int x, int y) {
		int z = 0;
		for (int i = 7; i >= 0; i--) { z = (z << 1) ^ ((z >> 7) * 0x11D); z ^= ((y >> i) & 1) * x; }
		return z;
	};
	int deg = ECC[ver];
	std::vector<int> div(deg, 0);
	div[deg - 1] = 1;
	for (int i = 0, root = 1; i < deg; i++) {
		for (int j = 0; j < deg; j++) { div[j] = mul(div[j], root); if (j + 1 < deg) div[j] ^= div[j + 1]; }
		root = mul(root, 2);
	}
	std::vector<int> rem(deg, 0);
	for (int b : words) {
		int factor = b ^ rem[0];
		rem.erase(rem.begin()); rem.push_back(0);
		for (int i = 0; i < deg; i++) rem[i] ^= mul(div[i], factor);
	}
	words.insert(words.end(), rem.begin(), rem.end());

	// the fixed patterns
	int n = 17 + 4 * ver;
	std::vector<std::vector<bool>> m(n, std::vector<bool>(n, false)), fn(n, std::vector<bool>(n, false));
	auto set = [&](std::vector<std::vector<bool>> &g, int x, int y, bool dark) { g[y][x] = dark; fn[y][x] = true; };
	for (int i = 0; i < n; i++) { set(m, 6, i, i % 2 == 0); set(m, i, 6, i % 2 == 0); }       // timing
	auto finder = [&](int cx, int cy) {
		for (int dy = -4; dy <= 4; dy++)
			for (int dx = -4; dx <= 4; dx++) {
				int x = cx + dx, y = cy + dy, d = std::max(abs(dx), abs(dy));
				if (x >= 0 && y >= 0 && x < n && y < n) set(m, x, y, d != 2 && d != 4);
			}
	};
	finder(3, 3); finder(n - 4, 3); finder(3, n - 4);
	if (ver >= 2)
		for (int dy = -2; dy <= 2; dy++)
			for (int dx = -2; dx <= 2; dx++) set(m, n - 7 + dx, n - 7 + dy, std::max(abs(dx), abs(dy)) != 1);
	auto format = [&](std::vector<std::vector<bool>> &g, int mask) {
		int data = 1 << 3 | mask, r = data;                  // level L = 01
		for (int i = 0; i < 10; i++) r = (r << 1) ^ ((r >> 9) * 0x537);
		int b = (data << 10 | r) ^ 0x5412;
		auto bit = [&](int i) { return ((b >> i) & 1) != 0; };
		for (int i = 0; i <= 5; i++) set(g, 8, i, bit(i));
		set(g, 8, 7, bit(6)); set(g, 8, 8, bit(7)); set(g, 7, 8, bit(8));
		for (int i = 9; i < 15; i++) set(g, 14 - i, 8, bit(i));
		for (int i = 0; i < 8; i++) set(g, n - 1 - i, 8, bit(i));
		for (int i = 8; i < 15; i++) set(g, 8, n - 15 + i, bit(i));
		set(g, 8, n - 8, true);                              // the dark module
	};
	format(m, 0);                                            // reserves the format areas

	// the codewords, in two-column zigzags from the bottom right
	size_t i = 0;
	for (int right = n - 1; right >= 1; right -= 2) {
		if (right == 6) right = 5;
		for (int vert = 0; vert < n; vert++)
			for (int j = 0; j < 2; j++) {
				int x = right - j, y = ((right + 1) & 2) == 0 ? n - 1 - vert : vert;
				if (fn[y][x] || i >= words.size() * 8) continue;
				m[y][x] = ((words[i >> 3] >> (7 - (i & 7))) & 1) != 0;
				i++;
			}
	}

	// try the eight masks, keep the one that scans best
	auto masked = [&](int k, int x, int y) {
		switch (k) {
		case 0: return (x + y) % 2 == 0;
		case 1: return y % 2 == 0;
		case 2: return x % 3 == 0;
		case 3: return (x + y) % 3 == 0;
		case 4: return (x / 3 + y / 2) % 2 == 0;
		case 5: return x * y % 2 + x * y % 3 == 0;
		case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
		default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
		}
	};
	auto penalty = [&](const std::vector<std::vector<bool>> &g) {
		int p = 0, dark = 0;
		auto at = [&](int x, int y, bool rows) { return rows ? g[y][x] : g[x][y]; };
		for (int pass = 0; pass < 2; pass++) {
			bool rows = pass == 0;
			for (int a = 0; a < n; a++) {
				int run = 1;
				for (int b = 1; b < n; b++) {
					if (at(b, a, rows) == at(b - 1, a, rows)) { run++; if (run == 5) p += 3; else if (run > 5) p++; }
					else run = 1;
				}
				// finder-like 1011101 with four light modules on one side (outside counts as light)
				static const int pat[] = { 1, 0, 1, 1, 1, 0, 1 };
				for (int b = -4; b < n; b++) {
					bool ok = true;
					for (int k = 0; k < 7 && ok; k++) { int c = b + k; ok = (c >= 0 && c < n ? at(c, a, rows) : false) == (pat[k] != 0); }
					if (!ok) continue;
					bool before = true, after = true;
					for (int k = 1; k <= 4; k++) {
						int c0 = b - k, c1 = b + 6 + k;
						if (c0 >= 0 && c0 < n && at(c0, a, rows)) before = false;
						if (c1 >= 0 && c1 < n && at(c1, a, rows)) after = false;
					}
					if (before) p += 40;
					if (after) p += 40;
				}
			}
		}
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				dark += g[y][x];
				if (x + 1 < n && y + 1 < n && g[y][x] == g[y][x + 1] && g[y][x] == g[y + 1][x] && g[y][x] == g[y + 1][x + 1]) p += 3;
			}
		int total = n * n;
		p += ((abs(dark * 20 - total * 10) + total - 1) / total - 1) * 10;
		return p;
	};
	std::vector<std::vector<bool>> best;
	int bestP = 1 << 30;
	for (int k = 0; k < 8; k++) {
		std::vector<std::vector<bool>> g = m;
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++)
				if (!fn[y][x] && masked(k, x, y)) g[y][x] = !g[y][x];
		format(g, k);
		int p = penalty(g);
		if (p < bestP) { bestP = p; best = g; }
	}
	return best;
}
