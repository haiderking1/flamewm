#ifndef FLAME_CORE_PRIORITY_H
#define FLAME_CORE_PRIORITY_H

// flame's thread ahead of every normal process: realtime round robin at the
// lowest realtime priority. a compositor parked by the kernel for a couple
// of ms in the middle of a frame misses its vblank, under load that happens
// a few times a second. sway and kwin do the same. apps flame starts don't
// inherit it. without the right (RLIMIT_RTPRIO, CAP_SYS_NICE) it falls back
// to the best nice it may have, else stays as it is
void priority_raise(void);

#endif
