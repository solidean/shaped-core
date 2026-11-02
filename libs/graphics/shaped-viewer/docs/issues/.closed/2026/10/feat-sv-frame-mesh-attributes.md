---
created: 2026-10-29
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/scene/scene_item.hh
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/resources/gpu_resource_manager.cc
  - test: "sv - a frame mesh traces in the frame it is placed, and keeps the image only while its content holds"
closed: 2026-10-30
resolution: fixed
---

# Frame meshes: attributes uploaded for the frame too

`mesh_lifetime::frame` uploads a mesh's geometry inline for one frame, but its attributes and textures still go through the managers.
`acquire_frame_scene_item` acquires each per-vertex or per-triangle attribute from the attribute manager, which streams it.
So new attribute content, a per-triangle color that changes with the topology for one, shades the real geometry through the fallback hit group until the stream lands.
Measured nowhere yet; seen as a frame or two of fallback shading after a change of topology, by the reasoning above rather than a capture.

The fallback is `view_renderer`'s `shade_fallback`, set while `attributes_resident(item.instance)` is false;
`attribute_manager::acquire` creates the persistent buffer and its descriptor at once, but fills it through the background stream, `pending` until that settles.

What it needs:
- the attribute buffers uploaded inline into transient buffers too, like the geometry, found again within the epoch by the content hash a `mesh_attribute` already carries,
  so a mesh several views acquire uploads once; that hash is needed anyway for the trace hash, so the lookup costs nothing extra;
- a parameter block that names them by this epoch's bindless indices, which today is built per `instance_id` from resident `attribute_id`s (`build_instance_parameters`);
- the trace hash keyed by the attributes' content in place of those indices, as it already is for the geometry.

The parameter block is the hard part: it is persistent and content-keyed on `parameter_key`, which folds in attribute ids.
A frame mesh's block would have to be rebuilt each epoch, or carry the attribute indices outside the block.

A simpler path, proposed 2026-10-30 and not yet tried: keep the persistent buffer and its id, so the parameter block stays as it is,
and record a frame mesh's attribute upload into the frame's command list rather than the stream, so it is resident the frame it is placed.
The renderer holds that command list where it uploads the frame geometry; the attribute bytes would travel on the scene item as the geometry does.
It leaves frame attributes in the attribute manager's budget until evicted, as today.

Textures can stay resident: they rarely change per frame.

Done 2026-10-30: the epoch's frame meshes are a map keyed by their geometry's content hash, so a re-acquire is O(1) rather than a scan of the epoch's meshes.

## Resolution

Fixed 2026-10-30 by the simpler path, the attribute bytes held by the manager rather than carried on the scene item:
`attribute_manager::acquire_inline` creates the persistent buffer and keeps the bytes, and `record_inline_uploads`, called at the start of `resolve_scene`,
writes them into the command list that draws the mesh and marks them complete.
Measured by "sv - a frame mesh's new attributes are resident in the frame it is placed, never streamed", new per-triangle colors each frame for 8 frames:
streamed, the mesh was shaded by the fallback in all 8 of its frames, so content new every frame never got its material; inline, in none.
The test checks residency, which is what the renderer gates the fallback on, not the pixels; it relies on the command list ordering the upload before later reads.
Not done: the transient-buffer route with a per-epoch parameter block, unneeded for this; frame attributes stay in the attribute budget until evicted.
