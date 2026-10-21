#include <stdlib.h>
#include <string.h>

#include "xserver/internal.h"

// GLX for apps drawing with the gpu in their own process (direct
// rendering): the configs, drawables' attributes, and which GL library
// glvnd loads, nvidia's own on nvidia (xserver_set_glx_vendor), mesa's
// elsewhere. frames come in through DRI3 and Present. contexts, windows and
// buffers live in the client, the requests about them are accepted

#define GLX_VENDOR 1
#define GLX_VERSION 2
#define GLX_EXTENSIONS 3
#define GLX_VENDOR_NAMES_EXT 0x20f6

// what direct rendering gets here, Xwayland's list without what needs the
// server's pixmaps on the gpu (texture from pixmap, copy sub buffer,
// pbuffers): flame's are in memory
static const char extensions[] = "GLX_ARB_context_flush_control GLX_ARB_create_context "
	"GLX_ARB_create_context_no_error GLX_ARB_create_context_profile "
	"GLX_ARB_create_context_robustness GLX_ARB_framebuffer_sRGB GLX_ARB_get_proc_address "
	"GLX_ARB_multisample GLX_EXT_create_context_es_profile "
	"GLX_EXT_create_context_es2_profile GLX_EXT_framebuffer_sRGB GLX_EXT_get_drawable_type "
	"GLX_EXT_libglvnd GLX_EXT_no_config_context GLX_EXT_visual_info GLX_EXT_visual_rating "
	"GLX_OML_swap_method GLX_SGI_make_current_read GLX_SGIS_multisample GLX_SGIX_fbconfig "
	"GLX_SGIX_visual_select_group";

// 8 bit rgb on the two visuals, the way mesa's drivers make them: double
// and single buffered, depth and stencil 24/8, 24/0, 16/0, 0/8, 0/0, 0 to
// 8 samples, srgb or not. the plain one (double, 24/8, no samples, not
// srgb) is a visual's first: GLX 1.2 apps find configs by visual
struct config {
	uint32_t id;
	uint32_t visual;
	uint8_t alpha, buffer; // bits
	uint8_t doublebuffer, depth, stencil, samples, srgb;
};

static const uint8_t depth_stencil[][2] = {{24, 8}, {24, 0}, {16, 0}, {0, 8}, {0, 0}};
static const uint8_t sample_counts[] = {0, 2, 4, 8};

#define CONFIGS (2 * 2 * 5 * 4 * 2)
static struct config configs[CONFIGS];

static void make_configs(void) {
	if (configs[0].id) {
		return;
	}
	size_t n = 0;
	for (int v = 0; v < 2; v++) {
		for (int db = 1; db >= 0; db--) {
			for (size_t ds = 0; ds < 5; ds++) {
				for (size_t sc = 0; sc < 4; sc++) {
					for (int srgb = 0; srgb < 2; srgb++) {
						configs[n] = (struct config){
							.id = (uint32_t)n + 1,
							.visual = v ? XS_VISUAL_32 : XS_VISUAL_24,
							.alpha = v ? 8 : 0,
							.buffer = v ? 32 : 24,
							.doublebuffer = (uint8_t)db,
							.depth = depth_stencil[ds][0],
							.stencil = depth_stencil[ds][1],
							.samples = sample_counts[sc],
							.srgb = (uint8_t)srgb,
						};
						n++;
					}
				}
			}
		}
	}
}

// the visual's first config
static const struct config *plain(uint32_t visual) {
	make_configs();
	return visual == XS_VISUAL_32 ? &configs[CONFIGS / 2] : &configs[0];
}

static void string_reply(struct xs_client *client, const char *string) {
	size_t len = strlen(string) + 1; // the nul counts
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, 0);
	xout32(&out, (uint32_t)len);
	xout_zero(&out, 16);
	xout_bytes(&out, string, len);
	xs_reply_send(client, &out);
}

static void query_server_string(struct xs_client *client, const struct xin *in) {
	switch (xin32(in, 8)) {
	case GLX_VENDOR:
		string_reply(client, "SGI");
		break;
	case GLX_VERSION:
		string_reply(client, "1.4");
		break;
	case GLX_EXTENSIONS:
		string_reply(client, extensions);
		break;
	case GLX_VENDOR_NAMES_EXT:
		// glvnd loads libGLX_<this> for the screen
		string_reply(client, client->xserver->glx_vendor);
		break;
	default:
		string_reply(client, "");
		break;
	}
}

// GetVisualConfigs: one per visual, its plain one. 18 values in a fixed
// order, then attribute pairs
static void get_visual_configs(struct xs_client *client) {
	static const uint32_t pairs[][2] = {
		{0x20, 0x8000}, // GLX_VISUAL_CAVEAT_EXT: none
		{0x22, 0x8002}, // GLX_X_VISUAL_TYPE: true color
		{100000, 0}, // GLX_SAMPLE_BUFFERS
		{100001, 0}, // GLX_SAMPLES
	};
	const struct config *visuals[] = {plain(XS_VISUAL_24), plain(XS_VISUAL_32)};
	uint32_t props = 18 + 2 * (uint32_t)(sizeof(pairs) / sizeof(pairs[0]));
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, 2);
	xout32(&out, props);
	xout_zero(&out, 16);
	for (size_t i = 0; i < 2; i++) {
		const struct config *c = visuals[i];
		const uint32_t fixed[18] = {
			c->visual, 4 /* TrueColor */, 1 /* rgba */, 8, 8, 8, c->alpha,
			0, 0, 0, 0, // accum
			c->doublebuffer, 0 /* stereo */, c->buffer, c->depth, c->stencil,
			0 /* aux */, 0 /* level */,
		};
		for (int j = 0; j < 18; j++) {
			xout32(&out, fixed[j]);
		}
		for (size_t j = 0; j < sizeof(pairs) / sizeof(pairs[0]); j++) {
			xout32(&out, pairs[j][0]);
			xout32(&out, pairs[j][1]);
		}
	}
	xs_reply_send(client, &out);
}

// GetFBConfigs: every config as attribute pairs
static void get_fb_configs(struct xs_client *client) {
	enum { ATTRIBS = 25 };
	make_configs();
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, CONFIGS);
	xout32(&out, ATTRIBS);
	xout_zero(&out, 16);
	for (size_t i = 0; i < CONFIGS; i++) {
		const struct config *c = &configs[i];
		const uint32_t attribs[ATTRIBS][2] = {
			{0x800b, c->visual}, // GLX_VISUAL_ID
			{0x8013, c->id}, // GLX_FBCONFIG_ID
			{0x8011, 1}, // GLX_RENDER_TYPE: rgba
			{0x8010, 1 | 2 | 4}, // GLX_DRAWABLE_TYPE: window, pixmap, pbuffer
			{0x8012, 1}, // GLX_X_RENDERABLE
			{0x22, 0x8002}, // GLX_X_VISUAL_TYPE: true color
			// slow or not, Xorg's rule: none. visual_select_group 0 for all
			{0x20, 0x8000}, // GLX_CONFIG_CAVEAT: none
			{0x8028, 0}, // GLX_VISUAL_SELECT_GROUP_SGIX
			{4, 1}, // GLX_RGBA
			{2, c->buffer}, // GLX_BUFFER_SIZE
			{3, 0}, // GLX_LEVEL
			{5, c->doublebuffer}, // GLX_DOUBLEBUFFER
			{6, 0}, // GLX_STEREO
			{7, 0}, // GLX_AUX_BUFFERS
			{8, 8}, // GLX_RED_SIZE
			{9, 8}, // GLX_GREEN_SIZE
			{10, 8}, // GLX_BLUE_SIZE
			{11, c->alpha}, // GLX_ALPHA_SIZE
			{12, c->depth}, // GLX_DEPTH_SIZE
			{13, c->stencil}, // GLX_STENCIL_SIZE
			{0x23, 0x8000}, // GLX_TRANSPARENT_TYPE: none
			{100000, c->samples ? 1 : 0}, // GLX_SAMPLE_BUFFERS
			{100001, c->samples}, // GLX_SAMPLES
			{0x20b2, c->srgb}, // GLX_FRAMEBUFFER_SRGB_CAPABLE_EXT
			{0x8060, 0x8063}, // GLX_SWAP_METHOD_OML: undefined
		};
		for (int j = 0; j < ATTRIBS; j++) {
			xout32(&out, attribs[j][0]);
			xout32(&out, attribs[j][1]);
		}
	}
	xs_reply_send(client, &out);
}

#define BAD_CONTEXT (XS_GLX_FIRST_ERROR + 0)
#define BAD_DRAWABLE (XS_GLX_FIRST_ERROR + 2)

// ---- contexts and drawables: just their configs ----

enum {
	OBJECT_CONTEXT,
	OBJECT_DRAWABLE, // a GLXWindow, GLXPixmap, or an X window made current
};

struct glx_object {
	struct wl_list link; // flame_xserver.glx_objects
	uint32_t id;
	struct xs_client *owner;
	int kind;
	uint32_t config;
	uint32_t drawable; // drawables: the X window or pixmap
};

static struct glx_object *object_find(struct flame_xserver *xserver, uint32_t id, int kind) {
	struct glx_object *o;
	wl_list_for_each(o, &xserver->glx_objects, link) {
		if (o->id == id && o->kind == kind) {
			return o;
		}
	}
	return NULL;
}

static void object_free(struct glx_object *o) {
	wl_list_remove(&o->link);
	free(o);
}

static void object_new(struct xs_client *client, uint32_t id, int kind, uint32_t config,
		uint32_t drawable, uint8_t minor) {
	if (!xs_id_available(client, id)) {
		xs_error(client, XS_ERROR_ID_CHOICE, id, XS_GLX_OPCODE, minor);
		return;
	}
	struct glx_object *o = calloc(1, sizeof(*o));
	if (!o) {
		xs_error(client, XS_ERROR_ALLOC, id, XS_GLX_OPCODE, minor);
		return;
	}
	*o = (struct glx_object){.id = id, .owner = client, .kind = kind, .config = config,
		.drawable = drawable};
	wl_list_insert(&client->xserver->glx_objects, &o->link);
}

static uint32_t config_of_visual(uint32_t visual) {
	return plain(visual == XS_VISUAL_32 ? XS_VISUAL_32 : XS_VISUAL_24)->id;
}

// a context goes current on an X window with no GLXWindow: like Xorg, the
// window gets a drawable with the context's config
static void made_current(struct xs_client *client, uint32_t drawable, uint32_t context) {
	struct flame_xserver *xserver = client->xserver;
	struct glx_object *c = object_find(xserver, context, OBJECT_CONTEXT);
	if (!c || !drawable || object_find(xserver, drawable, OBJECT_DRAWABLE)) {
		return;
	}
	if (!xserver_window(xserver, drawable) && !xs_pixmap_find(xserver, drawable)) {
		return;
	}
	struct glx_object *o = calloc(1, sizeof(*o));
	if (o) {
		*o = (struct glx_object){.id = drawable, .owner = client, .kind = OBJECT_DRAWABLE,
			.config = c->config, .drawable = drawable};
		wl_list_insert(&xserver->glx_objects, &o->link);
	}
}

bool xs_glx_id_in_use(struct flame_xserver *xserver, uint32_t id) {
	struct glx_object *o;
	wl_list_for_each(o, &xserver->glx_objects, link) {
		// an X window's own id isn't one more resource
		if (o->id == id && o->id != o->drawable) {
			return true;
		}
	}
	return false;
}

size_t xs_glx_ids(struct flame_xserver *xserver, uint32_t base, uint32_t *out, size_t max) {
	size_t len = 0;
	struct glx_object *o;
	wl_list_for_each(o, &xserver->glx_objects, link) {
		if (o->id != o->drawable && (o->id & ~0x1fffffu) == base && (!out || len < max)) {
			if (out) {
				out[len] = o->id;
			}
			len++;
		}
	}
	return len;
}

void xs_glx_drawable_gone(struct flame_xserver *xserver, uint32_t drawable) {
	struct glx_object *o, *tmp;
	wl_list_for_each_safe(o, tmp, &xserver->glx_objects, link) {
		if (o->kind == OBJECT_DRAWABLE && o->drawable == drawable) {
			object_free(o);
		}
	}
}

void xs_glx_client_gone(struct flame_xserver *xserver, struct xs_client *client) {
	struct glx_object *o, *tmp;
	wl_list_for_each_safe(o, tmp, &xserver->glx_objects, link) {
		if (!client || o->owner == client) {
			object_free(o);
		}
	}
}

// what Xorg says about a window or pixmap: its size, screen, config. GL
// drivers that render in the client ask before drawing into one (nvidia's
// won't make it current without)
static void get_drawable_attributes(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t id = xin32(in, 4);
	struct glx_object *glx = object_find(xserver, id, OBJECT_DRAWABLE);
	uint32_t x_id = glx ? glx->drawable : id;
	struct xs_window *window = xserver_window(xserver, x_id);
	struct xs_pixmap *pixmap = window ? NULL : xs_pixmap_find(xserver, x_id);
	if (!window && !pixmap) {
		xs_error(client, BAD_DRAWABLE, id, XS_GLX_OPCODE, 29);
		return;
	}
	uint32_t width = window ? window->width : pixmap->width;
	uint32_t height = window ? window->height : pixmap->height;
	uint32_t depth = window ? window->depth : pixmap->depth;
	uint32_t config = glx ? glx->config
		: config_of_visual(depth == 32 ? XS_VISUAL_32 : XS_VISUAL_24);
	// what Xwayland says, in its order. nvidia's driver won't draw into a
	// drawable it isn't told the type of
	const uint32_t attribs[][2] = {
		{0x20d4, 0}, // GLX_Y_INVERTED_EXT
		{0x801d, width}, // GLX_WIDTH
		{0x801e, height}, // GLX_HEIGHT
		{0x800c, 0}, // GLX_SCREEN
		{0x20d6, 0x20dd}, // GLX_TEXTURE_TARGET_EXT: GLX_TEXTURE_RECTANGLE_EXT
		{0x801f, 0}, // GLX_EVENT_MASK
		{0x8013, config}, // GLX_FBCONFIG_ID
		{0x20f5, 0}, // GLX_STEREO_TREE_EXT
		{0x8010, window ? 1 : 2}, // GLX_DRAWABLE_TYPE: window, pixmap
	};
	size_t count = sizeof(attribs) / sizeof(attribs[0]);
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, (uint32_t)count);
	xout_zero(&out, 20);
	for (size_t i = 0; i < count; i++) {
		xout32(&out, attribs[i][0]);
		xout32(&out, attribs[i][1]);
	}
	xs_reply_send(client, &out);
}

void xs_glx_request(struct xs_client *client, const struct xin *in) {
	struct xout out;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 5: // MakeCurrent: drawable, context. any tag will do
	case 26: // MakeContextCurrent: tag, drawable, read drawable, context
		if (minor == 5) {
			made_current(client, xin32(in, 4), xin32(in, 8));
		} else {
			made_current(client, xin32(in, 8), xin32(in, 16));
			made_current(client, xin32(in, 12), xin32(in, 16));
		}
		xs_reply_begin(client, &out, 0);
		xout32(&out, 1);
		xs_reply_send(client, &out);
		break;
	case 3: // CreateContext: context, visual
		object_new(client, xin32(in, 4), OBJECT_CONTEXT, config_of_visual(xin32(in, 8)), 0,
			minor);
		break;
	case 24: // CreateNewContext: context, fbconfig
	case 34: // CreateContextAttribsARB: context, fbconfig (0: no config)
		object_new(client, xin32(in, 4), OBJECT_CONTEXT, xin32(in, 8), 0, minor);
		break;
	case 4: { // DestroyContext
		struct glx_object *o = object_find(client->xserver, xin32(in, 4), OBJECT_CONTEXT);
		if (!o) {
			xs_error(client, BAD_CONTEXT, xin32(in, 4), XS_GLX_OPCODE, minor);
		} else {
			object_free(o);
		}
		break;
	}
	case 13: // CreateGLXPixmap: screen, visual, pixmap, glx pixmap
		object_new(client, xin32(in, 16), OBJECT_DRAWABLE, config_of_visual(xin32(in, 8)),
			xin32(in, 12), minor);
		break;
	case 22: // CreatePixmap: screen, fbconfig, pixmap, glx pixmap
	case 31: // CreateWindow: screen, fbconfig, window, glx window
		object_new(client, xin32(in, 16), OBJECT_DRAWABLE, xin32(in, 8), xin32(in, 12), minor);
		break;
	case 15: // DestroyGLXPixmap
	case 23: // DestroyPixmap
	case 32: { // DestroyWindow
		struct glx_object *o = object_find(client->xserver, xin32(in, 4), OBJECT_DRAWABLE);
		if (o && o->id != o->drawable) {
			object_free(o);
		}
		break;
	}
	case 6: // IsDirect: rendering happens in the client
		xs_reply_begin(client, &out, 0);
		xout8(&out, 1);
		xs_reply_send(client, &out);
		break;
	case 7: // QueryVersion
		xs_reply_begin(client, &out, 0);
		xout32(&out, 1);
		xout32(&out, 4);
		xs_reply_send(client, &out);
		break;
	case 14:
		get_visual_configs(client);
		break;
	case 29:
		get_drawable_attributes(client, in);
		break;
	case 17: // VendorPrivateWithReply: nothing private here
	case 25: // QueryContext: no attributes
		xs_reply_begin(client, &out, 0);
		xout32(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 18: // QueryExtensionsString
		string_reply(client, extensions);
		break;
	case 19:
		query_server_string(client, in);
		break;
	case 21:
		get_fb_configs(client);
		break;
	// rendering (indirect, nobody does), swaps, waits, pbuffers, client
	// info: accepted, nothing to keep. GL happens in the client
	case 1: case 2: case 8: case 9: case 10: case 11: case 12: case 16: case 20: case 27:
	case 28: case 30: case 33: case 35:
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_GLX_OPCODE, minor);
		break;
	}
}
