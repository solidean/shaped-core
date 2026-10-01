# Slug: shapes from their outlines

Slug draws shapes bounded by quadratic Bézier curves — glyphs, icons, any vector outline — directly from the curves on the GPU, with no image of the shape at any resolution.
It is Eric Lengyel's algorithm, after the reference shaders at github.com/EricLengyel/Slug, whose patent was dedicated to the public domain in March 2026.
This is the design, including the parts not built yet; [structure.md](structure.md#slug-in-progress) is the status.

Text is one producer of shapes and not the subject.
Nothing below the font reader may assume a shape came from a font, and the canvas that will expose this in shaped-viewer is a design of its own.

## How it works

Each shape is drawn by a pixel shader that casts one horizontal and one vertical ray through the sample and finds where each crosses the shape's curves.
It combines the two coverages into antialiased alpha.
Two lookup structures keep that cheap.

- **The curve texture** holds control points as half floats, two per texel, so a curve takes two texels and consecutive curves of a contour share one.
- **The band texture** splits the shape into horizontal and vertical bands, each listing only the curves that cross it, sorted so the loop stops at the first curve wholly behind the sample.

Both textures are 4096 texels wide, `rgba16_float` and `rg16_uint`: the formats the reference specifies.

A shape drawn on its own quad has the quad *dilated* by half a pixel after projection, so antialiased edges are never clipped, at any scale and under perspective.

## The pieces

```text
babel::font              babel-serializer  reads a font file into each format's own outline, transforming nothing
sr::slug_outline         shaped-rendering  closed contours of quadratic curves, in the shape's own units
sr::compile_slug_shape   shaped-rendering  outline -> curve and band tables, device-free
sr::slug_atlas           shaped-rendering  caller-owned textures many shapes share, plus a CPU copy of them
sr::slug_routine         shaped-rendering  draws shape instances from an atlas, one pipeline per (colour, depth) format
slug_coverage            SGL prelude       the coverage itself, which any pixel shader may call
sr::slug_font            shaped-rendering  a face's glyphs compiled on demand, and a one-line advance-only layout
```

**babel reads, and nothing else.**
`babel::font::face::outline` hands out a TrueType glyph as its points with their on-curve flags, as stored.
Implied on-curve midpoints, composite resolution and every other curve-building step belong to `sr::slug_outline_of`.

**Compilation rounds before it bands.**
Points are scaled by a power of two so the largest coordinate lands at or below 2048, where a half float holds every integer, then rounded to half floats.
The bands are built from the rounded points, so the sort the shader's early exit relies on is a sort of exactly what the GPU reads.
The compiler also follows the reference's recommendations:

- band counts that minimize the longest band;
- bands overlapping by 1/1024 of the shape's extent;
- no horizontal line in a horizontal band, and no vertical one in a vertical band;
- a straight line stored as the quadratic {p1, p2, p2}.

**The atlas is the caller's.**
Several coexist — one per font, per document — and a test builds one with no hidden state.
It is append-only: shapes are added on the CPU and `prepare` uploads what changed, growing a texture by reallocation.
A shape's band block stays on one row, and each curve run too, so no list needs the row wrap the shader allows.

**One instance per shape, 68 bytes.**
An instance places the shape's em space on the object's xy plane with a 2×2 basis and an origin, so a shape can be rotated or sheared per instance.
A draw's matrix then takes that plane anywhere, so the same instance lies flat on screen or on a model's face.

**The routine draws from what the caller holds.**
`execute` takes a buffer of instances and a range, so text that does not change uploads once and redraws free.
`prepare` plus `execute` over a span is the convenience on top, in imgui's shape: copies before the rendering scope opens, draws inside it.

**Colour is linear and premultiplied.**
An instance carries its colour as 8-bit sRGB, the vertex stage linearizes it, and output blends premultiplied — what shaped-viewer's targets hold.

**Depth from day one.**
A scope with a depth target draws depth-tested and never writes depth, so shapes lying on one surface layer in draw order.
A per-draw bias pulls shapes toward the camera, which is what keeps a label on a face from fighting the face.

**The fill rule is per shape, and the weight per draw, at runtime.**
SGL has no preprocessor, and both are a branch after the curve loops, uniform within a shape.

## A shape on any surface

`slug_coverage` lives in SGL's prelude rather than in the routine's shader, so a mesh's own pixel shader can draw a shape as part of its surface.
The mesh's vertices carry an em coordinate like a UV, and the pixel shader asks for the coverage at it.

```sgl sketch
let coverage = slug_coverage(decal.curves, decal.bands, p.em, decal.banding, decal.glyph, false)
```

The core takes the pixel footprint as an argument, and an overload takes it from `ddx` and `ddy`, so it must be called in uniform control flow.
A ray-traced hit has no derivatives, and will pass a footprint from its ray cone instead; that waits for shaped-viewer's tracer to move to SGL.

The prelude is a stopgap for SGL's missing `use`: the core moves out to a library file once a shader can import one.

## Using it

```cpp
auto font = sr::slug_font::load_system_ui_font().value();          // a TrueType font the OS ships
auto instances = cc::vector<sr::slug_instance>();
font.append_line(instances, "hello", tg::pos2f(24, 48), 32.0f, tg::vec4f(1, 1, 1, 1), tg::vec2f(1, 0), tg::vec2f(0, -1));

auto const prepared = sr::slug_routine::prepare(*cmd, font.atlas(), instances);   // before the scope
auto pass = cmd->raster.render_to({.color_targets = {target.preserved()}});
(void)sr::slug_routine::execute(pass, font.atlas(), prepared, {.object_to_clip = pixels_to_clip});
```

`graphics/slug-cube` draws labels on a cube's faces, a star from the cube's own shader, and a caption.
`shaped-viewer/text` draws over a traced scene through `sv::frame::draw_overlay`, the stopgap until the canvas exists.

## How it is held to the reference

- **Corpus tests** pin the pure helpers of the prelude — the root code, both root solves, the band wrap, the fill rules — on SGL's interpreter.
- **A C++ reference** of the whole pixel shader (`impl/slug_reference.hh`) reads the atlas's CPU copy, so compilation is tested with no device.
- **Readback** compares every pixel the routine draws against that reference, on every backend the tests run.

## Plan

```text
SGL: int2..4 / uint2..4 min, max, clamp; int2..4 abs      [done]
babel::font: TrueType glyf, cmap 4 and 12, hmtx            [done]
sr: outline, compilation, atlas, CPU reference             [done]
SGL prelude: slug_coverage, both overloads                 [done]
sr::slug_routine: quads, dilation, depth, both draw forms  [done]
sr::slug_font: glyphs on demand, one-line layout           [done]
examples: graphics/slug-cube, shaped-viewer/text           [done]
benchmark: runtime fill rule against nonzero-only          [planned]
babel::font: CFF / CFF2 charstrings, cubics split in sr    [planned]
atlas eviction                                             [planned]  rewrite band lists that point at moved curves
shapes on traced geometry                                  [planned]  ray-cone footprint, after the tracer moves to SGL
viewer depth for labels                                    [planned]  needs a primary-hit depth target from the trace
shaping and layout                                         [planned]  its own design, with the canvas
the canvas                                                 [planned]  its own design; replaces frame::draw_overlay
```
