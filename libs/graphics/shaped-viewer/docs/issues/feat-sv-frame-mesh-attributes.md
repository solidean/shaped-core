---
created: 2026-10-29
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/scene/scene_item.hh
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/resources/gpu_resource_manager.cc
  - test: "sv - a frame mesh traces in the frame it is placed, and keeps the image only while its content holds"
---

# Frame meshes: attributes uploaded for the frame too

`mesh_lifetime::frame` uploads a mesh's geometry inline for one frame, but its attributes and textures still go through the managers.
`acquire_frame_scene_item` acquires each per-vertex or per-triangle attribute from the attribute manager, which streams it.
So new attribute content, a per-triangle color that changes with the topology for one, shades the real geometry through the fallback hit group until the stream lands.
Measured nowhere yet; seen as a frame or two of fallback shading after a change of topology, by the reasoning above rather than a capture.

What it needs:
- the attribute buffers uploaded inline into transient buffers too, like the geometry, and deduplicated by content within the epoch;
- a parameter block that names them by this epoch's bindless indices, which today is built per `instance_id` from resident `attribute_id`s (`build_instance_parameters`);
- the trace hash keyed by the attributes' content in place of those indices, as it already is for the geometry.

The parameter block is the hard part: it is persistent and content-keyed on `parameter_key`, which folds in attribute ids.
A frame mesh's block would have to be rebuilt each epoch, or carry the attribute indices outside the block.

Textures can stay resident: they rarely change per frame.
