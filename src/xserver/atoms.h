#ifndef FLAME_XSERVER_ATOMS_H
#define FLAME_XSERVER_ATOMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// interned names, server wide. atom n is names[n], 0 is None and the core
// protocol's predefined ones come first with their fixed numbers
struct xs_atoms {
	char **names;
	size_t len, cap;
};

enum {
	XS_ATOM_ATOM = 4,
	XS_ATOM_CARDINAL = 6,
	XS_ATOM_STRING = 31,
	XS_ATOM_WINDOW = 33,
	XS_ATOM_WM_NAME = 39,
	XS_ATOM_WM_CLASS = 67,
	XS_ATOM_WM_TRANSIENT_FOR = 68,
};

bool xs_atoms_init(struct xs_atoms *atoms);
void xs_atoms_finish(struct xs_atoms *atoms);

// 0 when it doesn't exist and create is false, or out of memory
uint32_t xs_atom_intern(struct xs_atoms *atoms, const char *name, size_t len, bool create);
// NULL for an atom nobody interned
const char *xs_atom_name(const struct xs_atoms *atoms, uint32_t atom);

#endif
