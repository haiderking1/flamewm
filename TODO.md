# todo

## tearing for fullscreen games

a game frame goes out the moment it's done, no waiting for vblank. what
competitive players want.

- wayland: `wp_tearing_control_v1` (wlroots' `wlr_tearing_control_manager_v1`),
  the per surface async hint
- X: Present's `PresentOptionAsync`, what DXVK/VKD3D send with vsync off.
  flame's X already advertises it (`QueryCapabilities`) but xshell only
  treats it as "this msc", it still flips on vblank
- the flip: `tearing_page_flip` on the output state (wlroots 0.20), only for
  a fullscreen window that's the direct scanout candidate. committed when
  the app's buffer is ready, not at the scheduled render
- KMS can refuse async flips (kernel and driver, nvidia's included): fall
  back to a normal flip for that frame, no stall
- the cursor plane can't change in an async commit on some drivers
- a config switch (`allow_tearing`), off for anything but fullscreen

## VRR / adaptive sync

the screen waits for the frame instead of the frame waiting for the screen.
a big deal on a 165 panel whenever a game is under 165fps.

- `wlr_output_state_set_adaptive_sync_enabled`, only where
  `adaptive_sync_supported`, check `adaptive_sync_status` after
- on while a fullscreen game shows by default (desktop VRR makes cursor
  movement flicker on a lot of panels), config: off, fullscreen, always
- the render schedule stops pacing to a fixed vblank grid: commit when the
  frame is ready, the refresh range's max is the floor on frame time
- xshell's msc clock assumes a fixed period (`clock_period`), with VRR it
  has to follow the real flip timestamps
- low framerate compensation is the driver's, nothing for flame to do
