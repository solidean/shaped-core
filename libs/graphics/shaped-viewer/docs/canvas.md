# Drawings and the canvas

2D vector content in shaped-viewer: text, icons, arrows, annotations, drawn exactly at any size through Slug.
It follows the mesh principle: content is built once, uploaded once, and instanced as often as a frame wants.
sr's half — the routine, the atlas, the job draw — is [slug.md](../../shaped-rendering/docs/slug.md).

## The model

- **`sv::drawing`** is one graphic: a letter, a logo, an arrow.
  It is ordered filled layers, each a closed outline with a fill rule and a colour, in its own y-down units, and holds nothing GPU-side.
- **`sv::drawing_set`** is the value a caller builds and keeps, the 2D counterpart of `sv::mesh`.
  It is an ordered list of drawings, hashed as a whole, and immutable once placed: a changed set is a new hash and a new upload.
  `drawing_set::add` returns a **`sv::drawing_id`**, the drawing's index in its set.
- **An instance** places one drawing of a set: a position, two free vectors spanning the plane it lands on, and a scale.
  A drawing point (x, y) lands at `at + scale * (x * x_axis + y * y_axis)`.
  The vectors' lengths and angle are stretch and shear, which is what lets one arrow drawing, built for an edge of length 1, follow every edge of a mesh.
  An instance can tint, multiplying the drawing's own colours.
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
  That depth is the first sample's, reprojected to the pixel centre on the plane it hit, so a drawing lying on a face tests equal to the face rather than flickering with the jitter.
  A drawing that has to belong to the surface it lies on — lit, shadowed, curved with it — is a decal, which is traced.

## Residency

A set is acquired whole, through the frame's resource manager, into `sv::drawing_manager` beside `mesh_manager`.
On a miss its outlines are compiled on the CPU and its shapes and records land in the manager's atlas; a hit is a pointer compare against the set's cache slot.
The atlas is the manager's private storage, as a texture's memory is the texture manager's.

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

Each layer's instances become one **render job**: one draw call, through `sr::slug_routine`'s job form.
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
| atlas pages when one fills, and freeing on eviction | [planned] |
| text: `sv::font`, the system UI font by default, kerned multi-line layout, `add_text` on canvas and scene | [done] |
| strokes, with joins, caps and dashes, as filled outlines; shapes; nested drawings | [done] |
| annotations: a 2D box placed near a 3D anchor, a marker, a leader, hidden-line occlusion | [planned] |
| view titles, drawn by default and opted out per leaf | [planned] |
| decals on traced surfaces, projector first | [planned] |
| SVG | deferred |
