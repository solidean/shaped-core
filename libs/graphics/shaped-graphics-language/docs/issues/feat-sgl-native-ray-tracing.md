---
created: 2026-11-06
parent: epic-sgl-native-ir-targets
---

# The native layer lowers ray queries and ray-tracing stages

Ray queries first, then the pipeline stages, then what the text route cannot reach: motion traces through VK_NV_ray_tracing_motion_blur and SER written directly.

## Measured on ray queries

On 2026-11-07, on the Arc B580 and over the repo's SGL, DXC 1.9.2602:

- **Every vertex and pixel entry point lowers on both writers**, `trace_pixel_ps` among them: 82 of 82.
  Compute went from 93 to 105 of 115 on DXIL, and from 98 to 110 on SPIR-V.
  Each passes DXC's validator and `spirv-val`.
- **sg's ray query tests pass on `dx12-native-dxil` and `vulkan-native-spirv`**, with vulkan's validation and sync validation on.
- **The mixed test holds a read of every value a query answers to DXC's, within 1e-4**, on dx12 and vulkan.
  It is [`mixed-route-query-test.cc`](../../../shaped-graphics/tests/raster/mixed-route-query-test.cc).
  A compute stage and a query in a loop compare stage against stage, and a pixel stage in each pairing with its vertex stage.
  It can fail: taking the next row of a transform in lowering logged 60 failed checks.
- **Two shapes DXC's optimizer hid from the validator came up**, and both are general.
  A `select` must not read `undef`, so `flatten_branches` takes the other way's value.
  An operation declared and left with no call is refused, so `remove_unused` drops its declaration.

Not done here: a query with opacity micromaps, and `@first_instance`-style reads nothing asks for, such as a query's own flags or t_min.
No GPU but the B580 ran this, and the Agility runtime did not.
