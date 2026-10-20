#ifndef FLAME_INPUT_FAKE_H
#define FLAME_INPUT_FAKE_H

#include <stdbool.h>

struct flame_input;
struct flame_fake_keyboard;

// input for tests, only with FLAME_TEST_INPUT=1 (flamectl key / pointer):
// a keyboard that joins like a real one
bool fake_keyboard_create(struct flame_input *input);
void fake_keyboard_destroy(struct flame_input *input);
// a key by its keysym name ("a", "Return", "super", "shift"). false when
// there's no such key or no fake keyboard
bool fake_key(struct flame_input *input, const char *name, bool pressed);

#endif
