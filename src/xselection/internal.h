#ifndef FLAME_XSELECTION_INTERNAL_H
#define FLAME_XSELECTION_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

#include "xselection/xselection.h"

// between xselection.c (the clipboard) and xdnd.c (drag and drop), nobody
// else includes this

// the wayland mime type an X target stands for, NULL when it isn't data
const char *xsel_mime_of(struct flame_xselection *xselection, uint32_t target);
bool xsel_offers(struct wl_array *mime_types, const char *mime);
// mime and its X target, appended to the two parallel arrays
bool xsel_add_offer(struct wl_array *mimes, struct wl_array *targets, const char *mime,
	uint32_t target);
uint32_t xsel_target_of(struct wl_array *mimes, struct wl_array *targets, const char *mime);
// an X owner's selection as target, into fd (owned from here)
void xsel_fetch(struct flame_xselection *xselection, uint32_t selection, uint32_t target,
	int fd);
// an X app's request, answered from a wayland source offering mimes: send
// hands the source a mime type and the fd to write it to
void xsel_answer(struct flame_xselection *xselection, struct xs_selection_request *request,
	struct wl_array *mimes, void (*send)(void *source, const char *mime, int fd),
	void *source);

// ---- xdnd.c ----

bool xsel_dnd_init(struct flame_xselection *xselection);
void xsel_dnd_finish(struct flame_xselection *xselection);
// XdndSelection's owner changed: an X app started or ended a drag. false:
// not XdndSelection
bool xsel_dnd_owner_changed(struct flame_xselection *xselection, uint32_t selection,
	bool x_owned);
// a request for XdndSelection, answered from the wayland drag. false: not it
bool xselection_dnd_request(struct flame_xselection *xselection,
	struct xs_selection_request *request);

#endif
