#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xserver/internal.h"

// core fonts: the two Xorg has built in whatever the font path says, "fixed"
// (misc-fixed 6x13, iso8859-1, every GC's font until it gets another) and
// "cursor" (what XCreateFontCursor opens, its glyphs never drawn here).
// enough for what draws with core text still: Xlib font sets falling back
// to the GC's font (steam's updater), xterm -fn fixed, Xt and Motif bits

struct xs_font {
	const char *name; // the XLFD, ListFonts' answer
	const char *alias; // what's mostly asked for
	bool glyphs; // fixed's, the cursor font draws nothing
	uint16_t min_char, max_char, default_char;
	int16_t width, ascent, descent;
};

static const struct xs_font fonts[] = {
	{"-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso8859-1", "fixed", true, 0, 255,
		0, XS_FIXED_WIDTH, XS_FIXED_ASCENT, XS_FIXED_DESCENT},
	{"cursor", "cursor", false, 0, 153, 0, 16, 16, 0},
};

#define FIXED (&fonts[0])
#define FONTS (sizeof(fonts) / sizeof(fonts[0]))

// a font a client opened
struct xs_font_ref {
	struct wl_list link; // flame_xserver.fonts
	uint32_t id;
	struct xs_client *owner;
	const struct xs_font *font;
};

// ---- names ----

// X's font name patterns: * any run, ? any one, case doesn't matter. one
// star to go back to at a time, linear: a client's pattern can't stall us
static bool matches(const char *pattern, size_t len, const char *name) {
	size_t p = 0, star = SIZE_MAX;
	const char *retry = NULL;
	while (*name) {
		if (p < len && (pattern[p] == '?'
				|| tolower((unsigned char)pattern[p]) == tolower((unsigned char)*name))) {
			p++;
			name++;
		} else if (p < len && pattern[p] == '*') {
			star = p++;
			retry = name;
		} else if (star != SIZE_MAX) {
			p = star + 1;
			name = ++retry;
		} else {
			return false;
		}
	}
	while (p < len && pattern[p] == '*') {
		p++;
	}
	return p == len;
}

// the font a name or pattern opens: its alias or its XLFD
static const struct xs_font *font_named(const char *pattern, size_t len) {
	for (size_t i = 0; i < FONTS; i++) {
		if (matches(pattern, len, fonts[i].alias) || matches(pattern, len, fonts[i].name)) {
			return &fonts[i];
		}
	}
	// "6x13" is fixed too, like Xorg's alias
	return matches(pattern, len, "6x13") ? FIXED : NULL;
}

// ---- glyphs ----

static bool char_exists(const struct xs_font *font, uint8_t byte1, uint8_t byte2) {
	if (byte1 != 0 || byte2 < font->min_char || byte2 > font->max_char) {
		return false;
	}
	return !font->glyphs || (xs_fixed_present[byte2 / 32] >> (byte2 % 32)) & 1;
}

bool xs_font_glyph(const struct xs_font *font, uint8_t byte1, uint8_t byte2,
		const uint8_t **rows) {
	if (!char_exists(font, byte1, byte2)) {
		// not in the font: its default char, else nothing at all
		byte1 = (uint8_t)(font->default_char >> 8);
		byte2 = (uint8_t)font->default_char;
		if (!char_exists(font, byte1, byte2)) {
			return false;
		}
	}
	*rows = font->glyphs ? xs_fixed_glyphs[byte2] : NULL;
	return true;
}

int xs_font_width(const struct xs_font *font) {
	return font->width;
}

int xs_font_ascent(const struct xs_font *font) {
	return font->ascent;
}

int xs_font_descent(const struct xs_font *font) {
	return font->descent;
}

// ---- resources ----

static struct xs_font_ref *ref_find(struct flame_xserver *xserver, uint32_t id) {
	struct xs_font_ref *ref;
	wl_list_for_each(ref, &xserver->fonts, link) {
		if (ref->id == id) {
			return ref;
		}
	}
	return NULL;
}

const struct xs_font *xs_font_find(struct flame_xserver *xserver, uint32_t id) {
	struct xs_font_ref *ref = ref_find(xserver, id);
	return ref ? ref->font : NULL;
}

bool xs_font_is_cursor(const struct xs_font *font) {
	return font == &fonts[1];
}

const struct xs_font *xs_font_default(void) {
	return FIXED;
}

// a FONTABLE: a font, or a GC for its font. NULL: neither
static const struct xs_font *fontable(struct flame_xserver *xserver, uint32_t id) {
	const struct xs_font *font = xs_font_find(xserver, id);
	if (font) {
		return font;
	}
	struct xs_gc *gc = xs_gc_find(xserver, id);
	return gc ? (gc->font ? gc->font : FIXED) : NULL;
}

bool xs_font_id_in_use(struct flame_xserver *xserver, uint32_t id) {
	return ref_find(xserver, id) != NULL;
}

size_t xs_font_ids(struct flame_xserver *xserver, uint32_t base, uint32_t *out, size_t max) {
	size_t len = 0;
	struct xs_font_ref *ref;
	wl_list_for_each(ref, &xserver->fonts, link) {
		if ((ref->id & ~0x1fffffu) == base && (!out || len < max)) {
			if (out) {
				out[len] = ref->id;
			}
			len++;
		}
	}
	return len;
}

static void ref_free(struct xs_font_ref *ref) {
	wl_list_remove(&ref->link);
	free(ref);
}

void xs_font_client_gone(struct flame_xserver *xserver, struct xs_client *client) {
	struct xs_font_ref *ref, *tmp;
	wl_list_for_each_safe(ref, tmp, &xserver->fonts, link) {
		if (!client || ref->owner == client) {
			ref_free(ref);
		}
	}
}

// ---- replies ----

static void put_charinfo(struct xout *out, const struct xs_font *font, bool exists) {
	xout16(out, 0); // left bearing
	xout16(out, exists ? (uint16_t)font->width : 0); // right bearing
	xout16(out, exists ? (uint16_t)font->width : 0);
	xout16(out, exists ? (uint16_t)font->ascent : 0);
	xout16(out, exists ? (uint16_t)font->descent : 0);
	xout16(out, 0); // attributes
}

struct prop {
	const char *name;
	const char *atom; // a string property, its value an atom
	int32_t value;
};

static size_t font_props(const struct xs_font *font, struct prop *props) {
	size_t n = 0;
	props[n++] = (struct prop){"FONT", font->name, 0};
	if (font->glyphs) {
		static const struct prop fixed[] = {
			{"FOUNDRY", "Misc", 0}, {"FAMILY_NAME", "Fixed", 0},
			{"WEIGHT_NAME", "Medium", 0}, {"SLANT", "R", 0},
			{"SETWIDTH_NAME", "SemiCondensed", 0}, {"PIXEL_SIZE", NULL, 13},
			{"POINT_SIZE", NULL, 120}, {"RESOLUTION_X", NULL, 75},
			{"RESOLUTION_Y", NULL, 75}, {"SPACING", "C", 0},
			{"AVERAGE_WIDTH", NULL, 60}, {"CHARSET_REGISTRY", "ISO8859", 0},
			{"CHARSET_ENCODING", "1", 0}, {"DEFAULT_CHAR", NULL, 0},
			{"FONT_ASCENT", NULL, XS_FIXED_ASCENT}, {"FONT_DESCENT", NULL, XS_FIXED_DESCENT},
			{"CAP_HEIGHT", NULL, 9}, {"X_HEIGHT", NULL, 6},
			{"COPYRIGHT", "Public domain font.  Share and enjoy.", 0},
		};
		memcpy(props + n, fixed, sizeof(fixed));
		n += sizeof(fixed) / sizeof(fixed[0]);
	}
	return n;
}

#define MAX_PROPS 24

// QueryFont's and ListFontsWithInfo's common part, from the min bounds
// through the font descent. tail: QueryFont's char info count, or
// ListFontsWithInfo's replies hint
static void put_info(struct flame_xserver *xserver, struct xout *out,
		const struct xs_font *font, uint32_t tail, struct prop *props, size_t n) {
	put_charinfo(out, font, true); // min bounds: every glyph is one cell
	xout32(out, 0);
	put_charinfo(out, font, true); // max bounds
	xout32(out, 0);
	xout16(out, font->min_char);
	xout16(out, font->max_char);
	xout16(out, font->default_char);
	xout16(out, (uint16_t)n);
	xout8(out, 0); // left to right
	xout8(out, 0); // min byte1
	xout8(out, 0); // max byte1
	xout8(out, !font->glyphs); // all chars exist: fixed misses 0x7f-0x9f
	xout16(out, (uint16_t)font->ascent);
	xout16(out, (uint16_t)font->descent);
	xout32(out, tail);
	for (size_t i = 0; i < n; i++) {
		xout32(out, xserver_atom(xserver, props[i].name));
		xout32(out, props[i].atom ? xserver_atom(xserver, props[i].atom)
			: (uint32_t)props[i].value);
	}
}

static void query_font(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	const struct xs_font *font = fontable(xserver, xin32(in, 4));
	if (!font) {
		xs_error(client, XS_ERROR_FONT, xin32(in, 4), 47, 0);
		return;
	}
	struct prop props[MAX_PROPS];
	size_t n = font_props(font, props);
	uint32_t chars = font->max_char - font->min_char + 1u;
	struct xout out;
	xs_reply_begin(client, &out, 0);
	put_info(xserver, &out, font, chars, props, n);
	for (uint32_t c = font->min_char; c <= font->max_char; c++) {
		put_charinfo(&out, font, char_exists(font, 0, (uint8_t)c));
	}
	xs_reply_send(client, &out);
}

static void query_text_extents(struct xs_client *client, const struct xin *in) {
	const struct xs_font *font = fontable(client->xserver, xin32(in, 4));
	if (!font) {
		xs_error(client, XS_ERROR_FONT, xin32(in, 4), 48, 0);
		return;
	}
	// CHAR2Bs, the last one padding when odd length says so
	size_t chars = (in->len - 8) / 2 - (xin8(in, 1) & 1);
	int width = 0;
	bool any = false;
	for (size_t i = 0; i < chars; i++) {
		const uint8_t *rows;
		if (xs_font_glyph(font, xin8(in, 8 + 2 * i), xin8(in, 9 + 2 * i), &rows)) {
			width += font->width;
			any = true;
		}
	}
	struct xout out;
	xs_reply_begin(client, &out, 0); // left to right
	xout16(&out, (uint16_t)font->ascent);
	xout16(&out, (uint16_t)font->descent);
	xout16(&out, any ? (uint16_t)font->ascent : 0);
	xout16(&out, any ? (uint16_t)font->descent : 0);
	xout32(&out, (uint32_t)width);
	xout32(&out, 0); // overall left
	xout32(&out, (uint32_t)width); // overall right
	xs_reply_send(client, &out);
}

// the names a pattern matches, Xorg's way: aliases and XLFDs
static size_t list_names(const char *pattern, size_t len, const char **names, size_t max) {
	static const char *const all[] = {
		"cursor", "fixed", "6x13", "-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso8859-1",
	};
	size_t n = 0;
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]) && n < max; i++) {
		if (matches(pattern, len, all[i])) {
			names[n++] = all[i];
		}
	}
	return n;
}

static void list_fonts(struct xs_client *client, const struct xin *in) {
	uint16_t max = xin16(in, 4), len = xin16(in, 6);
	if (8u + len > in->len) {
		xs_error(client, XS_ERROR_LENGTH, 0, 49, 0);
		return;
	}
	const char *names[8];
	size_t n = list_names((const char *)in->data + 8, len, names, max < 8 ? max : 8);
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout16(&out, (uint16_t)n);
	xout_zero(&out, 22);
	for (size_t i = 0; i < n; i++) {
		xout8(&out, (uint8_t)strlen(names[i]));
		xout_bytes(&out, names[i], strlen(names[i]));
	}
	xout_pad(&out);
	xs_reply_send(client, &out);
}

// a reply for each font that matches, then one with an empty name
static void list_fonts_with_info(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint16_t max = xin16(in, 4), len = xin16(in, 6);
	if (8u + len > in->len) {
		xs_error(client, XS_ERROR_LENGTH, 0, 50, 0);
		return;
	}
	const char *names[8];
	size_t n = list_names((const char *)in->data + 8, len, names, max < 8 ? max : 8);
	struct xout out;
	for (size_t i = 0; i < n; i++) {
		const struct xs_font *font = font_named(names[i], strlen(names[i]));
		struct prop props[MAX_PROPS];
		size_t count = font_props(font, props);
		size_t name_len = strlen(names[i]);
		xs_reply_begin(client, &out, (uint8_t)name_len);
		put_info(xserver, &out, font, (uint32_t)(n - 1 - i), props, count);
		xout_bytes(&out, names[i], name_len);
		xout_pad(&out);
		xs_reply_send(client, &out);
	}
	xs_reply_begin(client, &out, 0);
	xout_zero(&out, 52);
	xs_reply_send(client, &out);
}

void xs_font_request(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint8_t opcode = xin8(in, 0);
	switch (opcode) {
	case 45: { // OpenFont
		uint32_t id = xin32(in, 4);
		uint16_t len = xin16(in, 8);
		if (12u + len > in->len) {
			xs_error(client, XS_ERROR_LENGTH, 0, opcode, 0);
			break;
		}
		if (!xs_id_available(client, id)) {
			xs_error(client, XS_ERROR_ID_CHOICE, id, opcode, 0);
			break;
		}
		const struct xs_font *font = font_named((const char *)in->data + 12, len);
		if (!font) {
			xs_error(client, XS_ERROR_NAME, 0, opcode, 0);
			break;
		}
		struct xs_font_ref *ref = calloc(1, sizeof(*ref));
		if (!ref) {
			xs_error(client, XS_ERROR_ALLOC, id, opcode, 0);
			break;
		}
		ref->id = id;
		ref->owner = client;
		ref->font = font;
		wl_list_insert(&xserver->fonts, &ref->link);
		break;
	}
	case 46: { // CloseFont: GCs drawing with it keep it, it's built in
		struct xs_font_ref *ref = ref_find(xserver, xin32(in, 4));
		if (!ref) {
			xs_error(client, XS_ERROR_FONT, xin32(in, 4), opcode, 0);
			break;
		}
		ref_free(ref);
		break;
	}
	case 47:
		query_font(client, in);
		break;
	case 48:
		query_text_extents(client, in);
		break;
	case 49:
		list_fonts(client, in);
		break;
	case 50:
		list_fonts_with_info(client, in);
		break;
	case 51: // SetFontPath: the built-ins are all there is
		break;
	case 52: { // GetFontPath
		struct xout out;
		xs_reply_begin(client, &out, 0);
		xout16(&out, 1);
		xout_zero(&out, 22);
		xout8(&out, 9);
		xout_bytes(&out, "built-ins", 9);
		xout_pad(&out);
		xs_reply_send(client, &out);
		break;
	}
	}
}
