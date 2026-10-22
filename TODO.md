# todo

## tearing and vrr on more hardware

both are in (see README, tearing and vrr), tested headless and nested.
what only a real screen shows:

- which drivers take async flips with a wait fence, and with the cursor
  plane untouched (amdgpu, i915): frame timing's `tearing` count vs `flips`
  says how many went through. nvidia takes them all (rtx 3070, a dx12 game
  through proton: 355 fps on 165Hz, scanned out, none refused)
- vrr switching without a modeset on each driver: the log says once if it
  won't switch
- xshell's msc clock counts a vrr flip as however many max-rate refreshes
  fit since the last one: monotonic, fifo games pace right, but an app
  measuring its own frame times from msc sees bigger steps than vblanks
