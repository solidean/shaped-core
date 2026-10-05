# Drawings and the canvas

2D vector content in shaped-viewer: text, icons, arrows, annotations, drawn exactly at any size through Slug.
It follows the mesh principle: content is built once, uploaded once, and instanced as often as a frame wants.
sr's half — the routine, the atlas, the job draw — is [slug.md](../../shaped-rendering/docs/slug.md).

## The model

- **`sv::drawing`** is one graphic: a letter, a logo, an arrow.
  It is ordered filled layers, each a closed outline with a fill rule and a color, in its own y-down units, and holds nothing GPU-side.
- **`sv::drawing_set`** is the value a caller builds and keeps, the 2D counterpart of `sv::mesh`.
  It is an ordered list of drawings, hashed as a whole, and immutable once placed: a changed set is a new hash and a new upload.
  `drawing_set::add` returns a **`sv::drawing_id`**, the drawing's index in its set.
- **An instance** places one drawing of a set: a position, two free vectors spanning the plane it lands on, and a scale.
  A drawing point (x, y) lands at `at + scale * (x * x_axis + y * y_axis)`.
  The vectors' lengths and angle are stretch and shear, which is what lets one arrow drawing, built for an edge of length 1, follow every edge of a mesh.
  An instance can tint, multiplying the drawing's own colors.
- **A lone drawing** can be instanced directly; sv makes it a one-element set behind the scenes.
- **A stroke is a layer too**: `drawing::add_stroke` expands it once, through `sr::stroke_outline`, into the outline of the area it covers.
  Its width is in the drawing's units, which on a canvas are pixels; its curves stay within 1/4096 of its extent, so a stroke is exact placed up to 4096 px across.
- **A drawing nests another** with `drawing::add_drawing`, copying its layers under a `frame_2d`: a symbol built once and stamped where it is needed.

## 2D and 3D

They differ in where they are added and in what depth means.

- **2D instances go on the canvas layer**, `view_ref::add_canvas`, in the view's logical pixels: y down, from the top-left or from any corner.
  Nothing occludes them, and later ones draw over earlier ones.
  A logical pixel is the window's content scale in texture pixels, so text keeps its physical size on a high-DPI display.
- **3D instances go on a scene**, `scene_ref::add_drawing`, in world units, seen through the view's camera.
  They are drawn after the trace, so traced geometry in front of them hides them, and they hide nothing traced: no shadow, not in a reflection.
  The trace writes its primary-hit depth for a layer that holds drawings, a fill pass turns it into a depth target, and the layer's drawings draw tested against it in a pass of their own.
  That depth is the first sample's, reprojected to the pixel center on the plane it hit, so a drawing lying on a face tests equal to the face rather than flickering with the jitter.
  A drawing that has to belong to the surface it lies on — lit, shadowed, curved with it — is a decal, which is traced.

## Decals

A decal is a drawing projected onto the traced surfaces of a scene, painted into their material: `scene_ref::add_decal(set, id, decal)`.
It is lit and shadowed with the surface, curves with it and shows in reflections, which a 3D drawing drawn over the trace cannot.

- **The projector is a box.** The drawing is placed on a plane as an instance is, and projected along `y_axis × x_axis` — away from a viewer reading it the right way round — to `depth` on either side.
- **Only the side facing the projector is painted**, judged by the normal the hit turned toward the ray, so it does not depend on how a mesh is wound.
  A surface met at a grazing angle fades out, rather than smearing the drawing along it.
- **It is paint.** At every hit `tracer.shade` tests the layer's decals before the material is read, and where a shape covers, the base takes its color and loses its metalness.
- **Its edges are antialiased by Slug's coverage** over the camera's pixel at the hit; past a bounce that footprint is too small, which the jitter makes up for.
- **Its shapes live in the drawing manager's decal atlas**, apart from the pages, since a trace binds one atlas as `slug.tables`.
  A set that does not fit empties it, unless a decal drew from it this frame; then the set draws nothing, and the manager says so once.

`examples/decals.cc` projects a badge over a metal ball onto the floor, and a target onto a cube's face.

## Annotations

An annotation is a label flat on screen, anchored at a point of a scene: `scene_ref::add_annotation(anchor, text, style)`.
It is text in a box, a marker on the anchor, and a leader from the marker to the box, all in logical pixels and drawn over the scene.

- **The plan places it every frame.**
  The layer keeps the anchor and the box's content, already resolved to atlas records; the plan projects the anchor through the view's camera and puts the box on its side.
  `automatic` puts it away from the view's center, so labels fan outward, and a box that would cross an edge is pushed back inside the margin.
  Nothing is compiled for a moving camera: the marker and the leader are a few fixed drawings, a disk, a ring, a unit segment and a dot, placed by their frames.
- **The GPU decides whether the anchor is hidden**, by Slug's frame probes against the trace's primary-hit depth at the anchor's pixel.
  One test per label, so it is never cut in half where it crosses a silhouette.
- **What a hidden anchor draws is a per-annotation policy.**
  `hidden_line` is the default: the box stays, the marker turns hollow and the leader dashed.
  `hide` drops the whole label, and `show` draws it as if the anchor were in view, for a point inside a part such as its center of mass.
- **Leaders are straight or elbowed**, with a width, a color and a dash pattern of their own, and another for the hidden look.

An anchor behind the camera or outside the view draws nothing.
Labels do not avoid each other yet; decluttering is its own feature, in [TODO.md](TODO.md).

## View titles

A layout leaf shows its first view's display name in a strip above it, 22 logical pixels tall.
Every leaf a caller adds has one unless `leaf_ref::title(false)` turns it off; the window's own single view has none, since the window's title bar names it.

- **The layout reserves the strip**, so a title never covers the image: the solver cuts it from the top of the leaf's rect, and fitting, hit-testing and the views' resolution follow the smaller rect.
  A pane lifted by a drag keeps the strip, since its hit region carries the whole leaf.
- **The name is set in glyphs before the plan**, which has no fonts.
  A leaf whose name is empty, or that has no font to set it in, reserves nothing, so a headless machine without a system font lays out as it always did.
- **All of a layout's titles are one 2D job** in its target, over a flat band per strip.

## Residency

A set is acquired whole, through the frame's resource manager, into `sv::drawing_manager` beside `mesh_manager`.
On a miss its outlines are compiled on the CPU and its shapes and records land in an atlas page; a hit is a pointer compare against the set's cache slot.
The pages are the manager's private storage, as a texture's memory is the texture manager's.

- **A page is an `sr::slug_atlas` capped at 512 rows**, and a set lives in exactly one: one that does not fit the newest page opens another.
- **Space comes back a page at a time.**
  Once there are four pages, the next opens by emptying the one drawn from longest ago; its sets leave the pool and are placed again the next time anything acquires them.
  Slug's atlas has no free list, so a page is the smallest unit that can be reclaimed without rewriting every band list that points at a moved curve.
- **A page drawn from this frame is never emptied**, since this frame's placements name its records.
  When every page is in use the manager opens one past the limit, and says so once.
- **A job draws once per run of placements sharing a page**, each draw from that page's atlas, so later drawings stay over earlier ones whichever pages they live in.

## Text

Text is drawings: a font's glyphs reach the GPU as drawing sets of a fixed 64 consecutive glyph ids, compiled the first time a string needs one.
A string is then one glyph instance per visible glyph.
`canvas_ref::add_text` and `scene_ref::add_text` lay the string out through `sr::layout_text`: kerned, broken at line breaks, wrapped and aligned.
They place it like any instance: on a canvas in logical pixels from any corner, in a scene on a plane.
A glyph's outline is y up and a drawing's coordinates are y down, so a glyph is placed with its y axis negated.
`sv::font` is a value keyed by the hash of its file, so two loads of one font share their glyph sets.
The default font is a sans-serif TrueType font the operating system ships.
shaped-core ships none, so captures with it differ between operating systems, and a platform without one draws no default text.

## Drawing

Each layer's instances become one **render job**, through `sr::slug_routine`'s job form: one draw call per run of placements sharing an atlas page.
A job is a frame per instance and a quad per (instance, shape layer), each quad naming an atlas record and a frame.
2D and 3D are separate jobs, since their view matrix and depth state differ.
A job's draw sits in its view target's pass at the layer's place, so a canvas draws over the scene below it and under whatever layer follows.

## Status

| piece | status |
|---|---|
| sr job draw: records in the atlas, frames and quads per job | [done] |
| `drawing`, `drawing_set`, `drawing_manager` | [done] |
| canvas layer and 2D instances | [done] |
| 3D instances, drawn over the trace and occluded by its primary-hit depth | [done] |
| atlas pages when one fills, and freeing on eviction | [done] |
| text: `sv::font`, the system UI font by default, kerned multi-line layout, `add_text` on canvas and scene | [done] |
| strokes, with joins, caps and dashes, as filled outlines; shapes; nested drawings | [done] |
| annotations: a 2D box placed near a 3D anchor, a marker, a leader, hidden-line occlusion | [done] |
| view titles, drawn by default and opted out per leaf | [done] |
| decals on traced surfaces, by a projector box | [done] |
| decals by a surface's own uv, text as a decal | [planned] |
| SVG | deferred |
