# shaped-xr cheat sheet

sg rendering in a headset through OpenXR.
Namespace `xr`; headers included by full path from `src/`.
[readme.md](readme.md) is the front door; [docs/design.md](docs/design.md) has the decisions.

How to read this: each block leads with the include; one symbol per line with a trailing comment giving the return type or the intuition.
Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

---

**Recording domain:** `xr`.

**Early stage.** System, session, frames with depth, and passthrough exist; input and Android are planned.

## System and context

```cpp
#include <shaped-xr/xr.hh>
xr::system::try_create({.application_name, .api = xr::graphics_api::vulkan, .wants_passthrough})  // -> result<unique_ptr<system>>
//   fails naming why: no loader in the build, no active runtime, no binding, no headset (FORM_FACTOR_UNAVAILABLE)
sys->create_context()                              // -> result<context_handle>, on the runtime's GPU and to its spec
sys->runtime_name(); sys->system_name(); sys->api(); sys->offers_passthrough();
sys->create_session(*ctx, {.near_distance = 0.05f, .far_distance = 100.0f, .passthrough})  // -> result<unique_ptr<session>>
//   ctx must come from create_context and outlive the session; the system outlives every session
XR_HAS_OPENXR                                      // 1/0: whether the build has the loader at all
```

## The frame loop

```cpp
while (session->poll_events())                     // false once the runtime wants the app to quit
    if (auto f = session->wait_frame())            // blocks on the runtime's pacing; none while not running
        session->submit_frame(cmd_or_null);        // EVERY waited frame is submitted, rendered or not
session->is_running(); session->is_focused(); session->shows_passthrough();
f.should_render                                    // false: draw nothing, still submit_frame(nullptr)
f.views[i].camera_to_world                         // tg::rigid_transform3f, tg's frame (LH, +y up, +z forward)
f.views[i].projection                              // LH zero-to-one: depth 0 at near, 1 at far
f.views[i].world_to_clip()                         // tg::mat4f, projection * inverse pose
f.color; f.depth                                   // sg::texture_2d_array, views.size() layers: view i into layer i
// render both in one pass: render_to({..., .view_count = 2}) with a `view_count = 2` SGL pipeline and @view_index
// clear color to transparent where shows_passthrough(): the frame blends over the room by its alpha
```

Gotchas: the color format is the runtime's choice (sRGB 8-bit rgba or bgra), so build the pipeline for `f.color.format()`.
On dx12 `f.color` is sg's own image, copied into the runtime's at submit; on vulkan it is the runtime's.
