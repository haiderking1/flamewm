#include "protocols/globals.h"

#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>

#include "core/server.h"

bool globals_init(struct flame_server *server) {
	struct wl_display *display = server->display;

	// wl_compositor itself comes from render/, it's tied to our renderer
	bool ok = wlr_subcompositor_create(display)
		// clipboard, primary selection, clipboard managers
		&& wlr_data_device_manager_create(display)
		&& wlr_primary_selection_v1_device_manager_create(display)
		&& wlr_data_control_manager_v1_create(display)
		&& wlr_ext_data_control_manager_v1_create(display, 1)
		// output info, scaling, timing
		&& wlr_xdg_output_manager_v1_create(display, server->outputs.layout)
		&& wlr_viewporter_create(display)
		&& wlr_fractional_scale_manager_v1_create(display, 1)
		&& wlr_single_pixel_buffer_manager_v1_create(display)
		&& wlr_presentation_create(display, server->backend.wlr, 2)
		// grim and friends. the ext one lets the client pick a format and
		// stride it can use: wlr-screencopy hands out what the gpu reads
		// fastest, 24 bit on nvidia, and a row that isn't whole words
		// (outputs not a multiple of 4 wide) breaks pixman, grim with it
		&& wlr_screencopy_manager_v1_create(display)
		&& wlr_ext_image_copy_capture_manager_v1_create(display, 1)
		&& wlr_ext_output_image_capture_source_manager_v1_create(display, 1);

	if (!ok) {
		wlr_log(WLR_ERROR, "failed to create wayland globals");
		return false;
	}

	// same idea as xdg-decoration in shell/, for gtk3 and older qt
	struct wlr_server_decoration_manager *kde_decoration =
		wlr_server_decoration_manager_create(display);
	if (!kde_decoration) {
		wlr_log(WLR_ERROR, "failed to create kde server decoration manager");
		return false;
	}
	wlr_server_decoration_manager_set_default_mode(kde_decoration,
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	return true;
}
