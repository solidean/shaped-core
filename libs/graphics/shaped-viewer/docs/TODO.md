# shaped-viewer TODO

Running list of known follow-ups.
Bigger design intent lives in [structure.md](structure.md).

- Define the dev-friendly renderer/scene API once shaped-rendering provides enough of the underlying render routines.
- **`per_edge` attributes need an edge table on `triangle_geometry`.**
  The enumerator exists and `mesh_attribute::create` rejects it; what is missing is the numbering — the edges themselves (each naming its two vertices) plus each triangle's three edge indices.
  That table also decides whether opposite half-edges share one entry, which is the real design question.
- **Nothing renders an `sv::mesh` yet.** The vocabulary exists (geometry, attributes, parameters, textures, flags); a view still holds `scene_item` ids.
  Rendering one needs the material definitions the `material_id` names, plus attribute upload.
  The bridge for the geometry half is `triangle_data::from(triangle_geometry)`, which keeps the manager's content key.
- Plan the RTX / ray-tracing path against the shaped-graphics backend capabilities as they land.
- Grow the [cheat-sheet](../cheat-sheet.md) + [structure](structure.md) as the renderer takes shape.
- **`mesh_is_indexed` belongs on the mesh, not the frame.**
  Geometry layout is a per-BLAS property.
  It rides in `frame_constants_gpu` / `pt_frame_constants_gpu` only because the trace binds one mesh per view.
  That is the same reason `Vertices` / `Indices` / `Materials` are single global bindings.
  Fold it into the per-instance mesh descriptor table the "one mesh per view" seam wants anyway, indexed by `InstanceID()` and carrying each mesh's vertex/index range or bindless handles.
  Moving the flag alone would not help: a per-instance flag over a still-global vertex buffer is no more correct.
  The DXR-native alternative is per-geometry data in the hit-group shader record via a local root signature, which specializes the `[branch]` in `mesh.hlsli` away.
  It needs local-root-signature support in sg's shader table first.
