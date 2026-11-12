---
created: 2026-11-11
parent: epic-sv-baking
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/shader_generator.cc
  - file: libs/graphics/shaped-viewer/shaders/sgl/tracer_wavefront.sgl
---

# Named output channels beside the image, accumulated like it

A traced view writes radiance, plus the albedo and normal guides its denoiser reads.
A bake wants any channel a caller names, at the primary hit, averaged over the jittered samples so it is filtered as the image is.

- **Built-in channels**: height (the hit's position along the view), the shading and geometric normals, instance id and a per-instance random, position.
- **Material channels**: any attribute of the hit's material by name, evaluated by the generated material code as shading evaluates it.
  The recursion this serves: a material baked from a model shaded with other materials carries their attributes into its own.
- **Integrator channels**, from extra rays at the primary hit:
  - ambient occlusion, cosine-weighted, out to a caller's reach
  - the bent normal
  - horizon angles in a few directions, which `feat-sv-relief-self-shadowing` reads back as a material's self-shadowing
- **A channel is a name and a format**, so a new one is a name and the code that writes it, not a new pipeline.
- **Supersampling for free**: each channel accumulates per pixel as the radiance does.
  A normal's spread within a pixel is the roughness a consumer folds in, as a mip chain does.

`feat-sv-settled-render` returns them beside the image.

## Done, 2026-11-12

- `render_settings::channels`, bits of `sv::render_channel`: position with coverage in .w, view depth, the geometric and shading normals, base color.
  Then material (roughness, metalness, micro-occlusion, opacity), and the instance's index and random.
- The wavefront tracer sums them at every primary hit into `wavefront_state::channel_sums`, counted by the tally's guide count, and restarts them with it.
- `sv::render_settled` reads them back as `settled_image::channels`, each a float4 mean a pixel.
- Tested in headless-viewer-test: a floor seen straight down by an orthographic view, every channel's value known.

## Left

- **Integrator channels**: occlusion, the bent normal and horizons, from extra rays at the primary hit, capped at a reach.
- **Material channels by name**: any attribute of the hit's material, which needs the generated material code per permutation.
- **The other renderers**: the megakernel and the hybrid write no channels; a layer asking for them there gets none, silently.
