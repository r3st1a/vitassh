/*
 * Generates the sce_sys artwork for the vpk.
 *
 * There is no image toolchain on this machine, so the PNGs are encoded here:
 * Node's zlib does the compression and the chunks are written by hand. The text
 * in the images is drawn with the same 8x8 font the app renders with, parsed
 * out of src/font8x8.c, so the artwork and the terminal agree.
 *
 * Usage: node tools/make_assets.js
 */

'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const ROOT = path.join(__dirname, '..');
const OUT = path.join(ROOT, 'sce_sys');

/* --- the font ------------------------------------------------------------ */

function loadFont() {
	const src = fs.readFileSync(path.join(ROOT, 'src', 'font8x8.c'), 'utf8');
	const start = src.indexOf('font8x8[FONT_GLYPHS]');
	if (start < 0) throw new Error('font8x8 table not found in src/font8x8.c');

	// The per-row comments carry the codepoint ("/* 0x21 '!' */"), which would
	// otherwise be scraped up as font data, so drop comments before matching.
	const block = src
		.slice(src.indexOf('{', start), src.indexOf('\n};', start))
		.replace(/\/\*[\s\S]*?\*\//g, '');
	const bytes = (block.match(/0x[0-9a-fA-F]{2}/g) || []).map((s) => parseInt(s, 16));

	if (bytes.length % 8 !== 0) throw new Error('font8x8 table is not a multiple of 8 bytes');

	const glyphs = [];
	for (let i = 0; i < bytes.length; i += 8) glyphs.push(bytes.slice(i, i + 8));

	return glyphs; // index 0 is 0x20
}

const FONT = loadFont();
const FONT_FIRST = 0x20;

/* --- PNG encoding -------------------------------------------------------- */

const CRC_TABLE = (() => {
	const t = new Int32Array(256);
	for (let n = 0; n < 256; n++) {
		let c = n;
		for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
		t[n] = c;
	}
	return t;
})();

function crc32(buf) {
	let c = 0xffffffff;
	for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
	return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
	const len = Buffer.alloc(4);
	len.writeUInt32BE(data.length, 0);

	const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);

	const crc = Buffer.alloc(4);
	crc.writeUInt32BE(crc32(body), 0);

	return Buffer.concat([len, body, crc]);
}

/*
 * Written as 8-bit indexed colour (colour type 3), matching the SDK's own
 * livearea art -- hello_world's icon0/bg/startup are all indexed, and the
 * launcher's own artwork is too. A truecolour PNG (colour type 2) of the same
 * image is a perfectly valid file and Vita3K installs it without complaint,
 * but the real installer rejects the whole vpk when it sees one, so the format
 * is not a free choice here. The art uses a few dozen distinct colours, so the
 * palette is exact rather than quantised.
 */
function encodePNG(w, h, rgba) {
	// Index the colours. The step only ever grows if the art outgrows the
	// palette -- a gradient with more than 256 steps would need it.
	let step = 0;
	let palette, indices;

	for (;;) {
		const mask = step === 0 ? 0xff : (0xff << step) & 0xff;
		const seen = new Map();

		palette = [];
		indices = new Uint8Array(w * h);

		let overflow = false;

		for (let i = 0; i < w * h; i++) {
			const s = i * 4;
			const r = rgba[s] & mask;
			const g = rgba[s + 1] & mask;
			const b = rgba[s + 2] & mask;
			const key = (r << 16) | (g << 8) | b;

			const idx = seen.get(key);

			if (idx === undefined) {
				if (palette.length === 256) {
					overflow = true;
					break;
				}
				seen.set(key, palette.length);
				indices[i] = palette.length;
				palette.push({ r: r, g: g, b: b, a: rgba[s + 3] });
			} else {
				indices[i] = idx;
				if (rgba[s + 3] < palette[idx].a) palette[idx].a = rgba[s + 3];
			}
		}

		if (!overflow) break;
		if (step === 7) throw new Error('cannot fit the image into 256 palette entries');
		step++;
	}

	const ihdr = Buffer.alloc(13);
	ihdr.writeUInt32BE(w, 0);
	ihdr.writeUInt32BE(h, 4);
	ihdr[8] = 8; // bit depth
	ihdr[9] = 3; // indexed
	ihdr[10] = 0; // deflate
	ihdr[11] = 0; // adaptive filtering
	ihdr[12] = 0; // no interlace

	const plte = Buffer.alloc(palette.length * 3);
	palette.forEach((c, i) => {
		plte[i * 3] = c.r;
		plte[i * 3 + 1] = c.g;
		plte[i * 3 + 2] = c.b;
	});

	// tRNS is optional -- the SDK's own bg.png omits it -- so only emit it when
	// the art really is transparent, with the trailing opaque entries trimmed.
	let trns = Buffer.alloc(0);
	if (palette.some((c) => c.a < 255)) {
		let last = palette.length - 1;
		while (last > 0 && palette[last].a === 255) last--;

		trns = Buffer.alloc(last + 1);
		for (let i = 0; i <= last; i++) trns[i] = palette[i].a;
	}

	// One filter byte (0 = none) in front of every scanline, then one index
	// byte per pixel.
	const raw = Buffer.alloc(h * (w + 1));
	for (let y = 0; y < h; y++) {
		const dst = y * (w + 1) + 1;

		for (let x = 0; x < w; x++) raw[dst + x] = indices[y * w + x];
	}

	return Buffer.concat([
		Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
		chunk('IHDR', ihdr),
		chunk('PLTE', plte),
		...(trns.length > 0 ? [chunk('tRNS', trns)] : []),
		chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
		chunk('IEND', Buffer.alloc(0)),
	]);
}

/* --- a tiny canvas ------------------------------------------------------- */

class Canvas {
	constructor(w, h) {
		this.w = w;
		this.h = h;
		this.px = Buffer.alloc(w * h * 4, 0);
	}

	blend(x, y, [r, g, b, a = 255]) {
		if (x < 0 || y < 0 || x >= this.w || y >= this.h || a === 0) return;

		const i = (y * this.w + x) * 4;
		const inv = 255 - a;

		this.px[i] = (r * a + this.px[i] * inv) / 255;
		this.px[i + 1] = (g * a + this.px[i + 1] * inv) / 255;
		this.px[i + 2] = (b * a + this.px[i + 2] * inv) / 255;
		this.px[i + 3] = Math.min(255, a + (this.px[i + 3] * inv) / 255);
	}

	fill(x, y, w, h, color) {
		for (let yy = y; yy < y + h; yy++)
			for (let xx = x; xx < x + w; xx++) this.blend(xx, yy, color);
	}

	/* Vertical gradient, one colour per row. */
	gradient(x, y, w, h, top, bottom) {
		for (let yy = 0; yy < h; yy++) {
			const t = h <= 1 ? 0 : yy / (h - 1);
			const c = [
				Math.round(top[0] + (bottom[0] - top[0]) * t),
				Math.round(top[1] + (bottom[1] - top[1]) * t),
				Math.round(top[2] + (bottom[2] - top[2]) * t),
				top[3] === undefined ? 255 : top[3],
			];
			this.fill(x, y + yy, w, 1, c);
		}
	}

	frame(x, y, w, h, color, t) {
		this.fill(x, y, w, t, color);
		this.fill(x, y + h - t, w, t, color);
		this.fill(x, y, t, h, color);
		this.fill(x + w - t, y, t, h, color);
	}

	disc(cx, cy, r, color) {
		for (let y = -r; y <= r; y++)
			for (let x = -r; x <= r; x++)
				if (x * x + y * y <= r * r) this.blend(cx + x, cy + y, color);
	}

	text(x, y, scale, color, str) {
		let cx = x;

		for (const ch of str) {
			const g = ch.codePointAt(0) - FONT_FIRST;

			if (g >= 0 && g < FONT.length) {
				const rows = FONT[g];

				for (let ry = 0; ry < 8; ry++) {
					const bits = rows[ry];

					for (let rx = 0; rx < 8; rx++)
						if (bits & (1 << rx))
							this.fill(cx + rx * scale, y + ry * scale, scale, scale, color);
				}
			}

			cx += 8 * scale;
		}

		return cx;
	}

	textWidth(scale, str) {
		return str.length * 8 * scale;
	}

	save(file) {
		fs.mkdirSync(path.dirname(file), { recursive: true });
		fs.writeFileSync(file, encodePNG(this.w, this.h, this.px));
		console.log(`${path.relative(ROOT, file)}  ${this.w}x${this.h}`);
	}
}

/* --- palette ------------------------------------------------------------- */

const INK = [0xd8, 0xd8, 0xd8, 255];
const GREEN = [0x5c, 0xff, 0x6c, 255];
const DIM = [0x7c, 0x8c, 0x9c, 255];
const WHITE = [0xff, 0xff, 0xff, 255];
const TERM_BG = [0x0c, 0x0e, 0x12, 255];
const TERM_EDGE = [0x3a, 0x5c, 0x7e, 255];
const BAR = [0x22, 0x30, 0x3e, 255];

/* A terminal window: body, title bar and traffic-light dots. */
function terminalWindow(c, x, y, w, h, barH, dotR) {
	c.fill(x, y, w, h, TERM_BG);
	c.frame(x, y, w, h, TERM_EDGE, Math.max(2, Math.round(w / 260)));
	c.fill(x + 1, y + 1, w - 2, barH, BAR);

	const cy = y + 1 + Math.round(barH / 2);
	const step = dotR * 4;
	for (let i = 0; i < 3; i++)
		c.disc(x + dotR * 2 + step * i, cy, dotR, [0x8c, 0x9c, 0xac, 255]);

	return y + barH + 1;
}

/* --- icon0.png ----------------------------------------------------------- */

function makeIcon() {
	const c = new Canvas(128, 128);

	c.gradient(0, 0, 128, 128, [0x16, 0x22, 0x30], [0x0a, 0x0e, 0x16]);

	const inner = terminalWindow(c, 8, 22, 112, 84, 10, 2);

	c.text(18, inner + 10, 1, INK, '$ ssh home');
	c.text(18, inner + 22, 1, GREEN, 'Connected');
	c.text(18, inner + 34, 1, INK, '$ uname -a');
	c.fill(18, inner + 46, 6, 9, GREEN); // a fresh prompt with a cursor block

	c.save(path.join(OUT, 'icon0.png'));
}

/* --- livearea bg.png ----------------------------------------------------- */

function makeBackground() {
	const c = new Canvas(840, 500);

	c.gradient(0, 0, 840, 500, [0x12, 0x1c, 0x28], [0x06, 0x08, 0x0c]);

	const inner = terminalWindow(c, 60, 56, 720, 388, 32, 6);

	c.text(100, inner + 34, 4, WHITE, 'VitaSSH');
	c.text(100, inner + 78, 2, DIM, 'SSH client for the PS Vita');

	c.fill(100, inner + 108, 640, 2, [0x2c, 0x3c, 0x4c, 255]);

	c.text(100, inner + 126, 2, GREEN, '$ ssh home');
	c.text(100, inner + 154, 2, INK, 'Connected to 192.168.1.10');
	c.text(100, inner + 182, 2, GREEN, '$ uname -a');
	c.text(100, inner + 210, 2, INK, 'Linux 6.1.0 aarch64');

	c.fill(100, inner + 244, 14, 14, GREEN); // cursor

	c.save(path.join(OUT, 'livearea', 'contents', 'bg.png'));
}

/* --- livearea startup.png ------------------------------------------------ */

function makeStartup() {
	const c = new Canvas(280, 158);

	c.gradient(0, 0, 280, 158, [0x14, 0x1e, 0x2a], [0x08, 0x0a, 0x10]);

	const title = 'VitaSSH';
	c.text((280 - c.textWidth(3, title)) / 2, 52, 3, WHITE, title);

	c.fill(60, 92, 160, 2, TERM_EDGE);

	const sub = 'starting up...';
	c.text((280 - c.textWidth(1, sub)) / 2, 112, 1, DIM, sub);

	c.save(path.join(OUT, 'livearea', 'contents', 'startup.png'));
}

/* --- template.xml -------------------------------------------------------- */

/* Deliberately identical to the SDK hello_world sample's template, down to the
 * a1 style. The richer form -- style="psmobile" with <frame>/<liveitem> text,
 * as pretty_livearea uses -- is what this shipped with first, and the real
 * installer rejected the vpk that carried it. Which of the two differences
 * (this or the truecolour PNGs) was fatal is not known, so both are gone. */
const TEMPLATE = `<?xml version="1.0" encoding="utf-8"?>

<livearea style="a1" format-ver="01.00" content-rev="1">
	<livearea-background>
		<image>bg.png</image>
	</livearea-background>

	<gate>
		<startup-image>startup.png</startup-image>
	</gate>
</livearea>
`;

function makeTemplate() {
	const file = path.join(OUT, 'livearea', 'contents', 'template.xml');

	fs.mkdirSync(path.dirname(file), { recursive: true });
	fs.writeFileSync(file, TEMPLATE);
	console.log(`${path.relative(ROOT, file)}`);
}

/* --- go ------------------------------------------------------------------ */

makeIcon();
makeBackground();
makeStartup();
makeTemplate();
console.log(`\nfont: ${FONT.length} glyphs, 0x${FONT_FIRST.toString(16)}-0x${(FONT_FIRST + FONT.length - 1).toString(16)}`);
