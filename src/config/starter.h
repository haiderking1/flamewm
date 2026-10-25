#ifndef FLAME_CONFIG_STARTER_H
#define FLAME_CONFIG_STARTER_H

// the first time flame runs for someone: their flame.lua written (the bar in
// it when flame's is installed), or an init.lua from before moved to
// flame.lua. nothing when there's one already
void config_first_start(void);

#endif
