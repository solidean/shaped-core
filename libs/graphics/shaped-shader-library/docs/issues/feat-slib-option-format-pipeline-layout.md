---
created: 2026-10-17
parent: epic-slib-sgl-options-in-host-code
refs:
  - file: libs/graphics/shaped-shader-library/cmake/sgl_host_code.py
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/pipeline.hh
  - symbol: refuse_option_layouts
  - symbol: slib::pipeline_definition::acquire_layout
---

# A pipeline over a group whose image format names an option is refused

A generated raster or ray-tracing pipeline over a group whose image format names an SGL option is refused by `sgl_host_code.py`.
The group itself takes such a view format-erased, with its layout per set of values.
The pipeline cannot follow: `pipeline_definition::acquire_layout` takes the context alone, not the values.
