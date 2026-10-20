#include <string.h>

#include "xserver/internal.h"

// just enough GLX for apps that insist on a GL visual before doing anything
// (steam's main process asserts without one). it says the vendor is mesa, so
// glvnd picks mesa's software rasterizer, which renders on the cpu and
// presents with PutImage, which nothing here draws. contexts, windows and
// buffers are accepted and forgotten

#define GLX_VENDOR 1
#define GLX_VERSION 2
#define GLX_EXTENSIONS 3
#define GLX_VENDOR_NAMES_EXT 0x20f6

static const char extensions[] = "GLX_ARB_create_context GLX_ARB_create_context_profile "
	"GLX_ARB_get_proc_address GLX_ARB_multisample GLX_EXT_libglvnd GLX_EXT_visual_info "
	"GLX_EXT_visual_rating GLX_SGIX_fbconfig";

// one config per visual: 8 bit rgb, double buffered, 24 bit depth, 8 bit
// stencil. what mesa's software driver has, so it keeps them
struct config {
	uint32_t visual;
	uint32_t alpha;
	uint32_t depth; // buffer bits
	uint32_t id;
};

static const struct config configs[] = {
	{XS_VISUAL_24, 0, 24, 1},
	{XS_VISUAL_32, 8, 32, 2},
};

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
		string_reply(client, "Mesa Project and SGI");
		break;
	case GLX_VERSION:
		string_reply(client, "1.4");
		break;
	case GLX_EXTENSIONS:
		string_reply(client, extensions);
		break;
	case GLX_VENDOR_NAMES_EXT:
		// glvnd loads libGLX_mesa for this screen
		string_reply(client, "mesa");
		break;
	default:
		string_reply(client, "");
		break;
	}
}

// GetVisualConfigs: 18 values in a fixed order, then attribute pairs
static void get_visual_configs(struct xs_client *client) {
	static const uint32_t pairs[][2] = {
		{0x20, 0x8000}, // GLX_VISUAL_CAVEAT_EXT: none
		{0x22, 0x8002}, // GLX_X_VISUAL_TYPE: true color
		{100000, 0}, // GLX_SAMPLE_BUFFERS
		{100001, 0}, // GLX_SAMPLES
	};
	uint32_t props = 18 + 2 * (uint32_t)(sizeof(pairs) / sizeof(pairs[0]));
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, sizeof(configs) / sizeof(configs[0]));
	xout32(&out, props);
	xout_zero(&out, 16);
	for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
		const struct config *c = &configs[i];
		const uint32_t fixed[18] = {
			c->visual, 4 /* TrueColor */, 1 /* rgba */, 8, 8, 8, c->alpha,
			0, 0, 0, 0, // accum
			1 /* double buffer */, 0 /* stereo */, c->depth, 24 /* depth */, 8 /* stencil */,
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
	enum { ATTRIBS = 22 };
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, sizeof(configs) / sizeof(configs[0]));
	xout32(&out, ATTRIBS);
	xout_zero(&out, 16);
	for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
		const struct config *c = &configs[i];
		const uint32_t attribs[ATTRIBS][2] = {
			{0x800b, c->visual}, // GLX_VISUAL_ID
			{0x8013, c->id}, // GLX_FBCONFIG_ID
			{0x8011, 1}, // GLX_RENDER_TYPE: rgba
			{0x8010, 1 | 2 | 4}, // GLX_DRAWABLE_TYPE: window, pixmap, pbuffer
			{0x8012, 1}, // GLX_X_RENDERABLE
			{0x22, 0x8002}, // GLX_X_VISUAL_TYPE: true color
			{0x20, 0x8000}, // GLX_CONFIG_CAVEAT: none
			{4, 1}, // GLX_RGBA
			{2, c->depth}, // GLX_BUFFER_SIZE
			{3, 0}, // GLX_LEVEL
			{5, 1}, // GLX_DOUBLEBUFFER
			{6, 0}, // GLX_STEREO
			{7, 0}, // GLX_AUX_BUFFERS
			{8, 8}, // GLX_RED_SIZE
			{9, 8}, // GLX_GREEN_SIZE
			{10, 8}, // GLX_BLUE_SIZE
			{11, c->alpha}, // GLX_ALPHA_SIZE
			{12, 24}, // GLX_DEPTH_SIZE
			{13, 8}, // GLX_STENCIL_SIZE
			{0x23, 0x8000}, // GLX_TRANSPARENT_TYPE: none
			{100000, 0}, // GLX_SAMPLE_BUFFERS
			{100001, 0}, // GLX_SAMPLES
		};
		for (int j = 0; j < ATTRIBS; j++) {
			xout32(&out, attribs[j][0]);
			xout32(&out, attribs[j][1]);
		}
	}
	xs_reply_send(client, &out);
}

void xs_glx_request(struct xs_client *client, const struct xin *in) {
	struct xout out;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 5: // MakeCurrent
	case 26: // MakeContextCurrent: any tag will do
		xs_reply_begin(client, &out, 0);
		xout32(&out, 1);
		xs_reply_send(client, &out);
		break;
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
	case 17: // VendorPrivateWithReply: nothing private here
	case 25: // QueryContext: no attributes
	case 29: // GetDrawableAttributes: none known
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
	// contexts, drawables, swaps, client info: accepted, nothing to keep
	case 1: case 2: case 3: case 4: case 8: case 9: case 10: case 11: case 12: case 13:
	case 15: case 16: case 20: case 22: case 23: case 24: case 27: case 28: case 30:
	case 31: case 32: case 33: case 34: case 35:
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_GLX_OPCODE, minor);
		break;
	}
}
