---
created: 2026-11-16
refs:
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_command_list.cc
  - issue: feat-sgl-tessellation-polyfill
---

# A polyfilled tessellation draw reserves its largest factor's scratch for every patch, gigabytes for a large draw

Each patch of a polyfilled tessellation draw owns the points, evaluated vertices and indices of the largest factor its control stage allows (`max_factor`, 64 by default).
A quad at 64 is 4225 points and 24 576 indices, about 280 KB a patch with a few words of vertex output, so 65 536 patches ask for about 18 GB.
That is computed from `raster_draw_tessellation_polyfilled`'s sizing, not measured; nothing refuses such a draw or reports it.

Seen 2026-11-16: the tessellation polyfill benchmark's 256 x 256 patches at factor 4, its control stage stating no `max_factor`.
It drove the M4 below dev.py's 2 GB free-memory floor, and the run was ended.
With `max_factor = 4` stated, the same run peaked at 173 MiB of RAM.

Wanted at least: a draw whose scratch passes a limit is refused with a feedback error naming `max_factor`, rather than allocated.
Better: the scratch sized by what the count pass found, which needs the draw split into chunks of patches, or a bound the host can know before the passes run.
