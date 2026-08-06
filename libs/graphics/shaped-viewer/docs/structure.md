# shaped-viewer structure (sv::)

The living roadmap for shaped-viewer.
Section headers carry a status tag: **[done]** / **[in progress]** / **[planned]**.
This document is design intent, not a guarantee of final API.

shaped-viewer now has a **first vertical slice**: a per-frame `viewer_definition` of views, each path-traced into a target texture and blitted into a window.
Raytracing-first — raster is a later fallback / special case.

## Goals

A professional, RTX-enabled visualization renderer with a dev-friendly API, built on shaped-rendering (`sr::`).
It is the top of the graphics stack and the intended home for Shaped Code's visualization needs — SOLIDEAN, internal tools, customer projects.

## Module layout

```text
viewer_definition / view / scene_item   [in progress]  the per-frame description (camera, items, settings, id, size)
resource managers (mesh / material)     [in progress]  strongly-typed ids -> GPU resources (BLAS built here); LRU budget + idle eviction
resource data (triangle / indexed / material)  [in progress]  what a caller uploads: a pinned_data payload + its cc::hash128 content key
pathtrace_routine                        [in progress]  the DXR GI trace the view_renderer drives: TLAS + dispatch_rays into a UAV target
pbr_raytrace_routine                     [in progress]  the flat single-bounce IBL DXR trace (SH environment), driven directly (not by the view_renderer)
view_renderer                            [in progress]  a render routine that orchestrates: resolve ids, upload constants, path-trace, open the scope, blit (via sr::blit_routine)
sv_shaders package                       [in progress]  raygen / miss+closest-hit (SH environment + PBR + GI), via slib
(the fullscreen blit now lives in shaped-rendering: sr::blit_routine + sr_shaders' blit.hlsl)
camera / controls                        [in progress]  dev-friendly pinhole camera; a fly controller lives in the test
materials / lighting                     [in progress]  one flat PBR material; the view holds typed light lists (area_lights) + an SH background; textures and more light kinds next
multi-view / multi-window compositing    [planned]      one view -> one window today; the seam exists (view_renderer blits the first view)
temporal accumulation                    [planned]      view_id keys the persistent-resource cache (empty payload now)
```

## Platform / backend status

Ray tracing runs on **dx12 + DXR** (Windows), hardware or WARP.
Vulkan RT is stubbed upstream in shaped-graphics, so rendering is Windows-only for now.
The whole sv API compiles everywhere, though: without a backend a routine simply acquires no shader and draws nothing.

## First library-extension seams (per the "living libraries" rule)

- **PBR/BRDF shading** is authored fresh in `shaders/pbr.hlsli`; a shared shader BRDF library is the natural home once a second consumer appears.
- **The id-pool now exists** as `sv::impl::lru_pool<Id, Record>` (budget + idle eviction, LRU).
  If a second library wants it, promoting a generational version into clean-core is the next step.
- **TLAS is rebuilt every frame**, since refit/update is not implemented in sg yet; `tlas_id` exists for a future prebuilt/persistent TLAS.
- **Texture download** exists in sg as `cmd.download.bytes_from_texture`, but the trace stays on the proven UAV-write-then-blit path.
  Pixel-level tests have not been written against it yet.
- **One mesh per view** — the trace binds the first item's vertex/material buffers; multi-mesh wants per-instance material/vertex indexing.
