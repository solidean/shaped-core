---
created: 2026-11-12
refs:
  - file: libs/graphics/shaped-viewer/tests/final-render-benchmark.cc
  - symbol: sv::impl::capture_session::is_share_spent
closed: 2026-11-12
resolution: fixed
---

# A budgeted sequence capture once stalled inside a frame, past its hard timeout

One release run of the final-render benchmark stopped writing at frame 245 of 600, a minute in, and spun for half an hour at about 0.7 of a core.
The per-frame hard timeout never fired, so the loop was stuck inside one frame rather than in the capture's settle rule.
The likely waits there are `report_capture_write`, once 16 images are in flight, and the epoch wait at the frame's end; either spins pumping work until what it waits for lands.
Three later runs of the same clip passed, two on release and one on `relwithdebinfo` under `--watchdog`, so it is intermittent.
That run was the first after the noise readback became one in flight at a time; earlier runs requested one every frame.
A frame stuck that way escapes every timeout, so the fix includes a wait that gives up, not only the cause.

## Resolution

The cause was a GPU hang, which a chain of denoised viewers on one context then reproduced every few runs.
A capture's drain had stopped cutting the temporal history, so the temporal member carried a history across textures replaced in place; the clean checkout never did, and never hung.
A drain now cuts the history on a run's first frame, which is where an asset's loads and encodes land (`capture_session::drain_cuts_history`).
The spin rather than an error came from `report_capture_write`, which waited without bound for a readback a hung device never delivers; it now gives an image up after 30 seconds.
Afterwards three chains of eight denoised viewers and ten budgeted clips ran clean.
