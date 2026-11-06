# shaped-xr cheat sheet

sg rendering in a headset through OpenXR.
Namespace `sxr`; headers included by full path from `src/`.
[readme.md](readme.md) is the front door; [docs/design.md](docs/design.md) has the decisions.

How to read this: each block leads with the include; one symbol per line with a trailing comment giving the return type or the intuition.
Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

---

**Recording domain:** `sxr`.

**Early stage.** System, session, frames with depth, passthrough and actions exist; drawing controllers and Android are planned.

## System and context

```cpp
#include <shaped-xr/xr.hh>
sxr::system::try_create({.application_name, .api = sxr::graphics_api::vulkan, .wants = {sxr::capability::passthrough}})  // -> result<unique_ptr<system>>
//   fails naming why: no loader in the build, no active runtime, no binding, no headset (FORM_FACTOR_UNAVAILABLE)
sys->create_context()                              // -> result<context_handle>, on the runtime's GPU and to its spec
sys->runtime_name(); sys->system_name(); sys->api();
sys->offers(sxr::capability::environment_depth)   // -> bool: wanted, listed, and the headset says it has it
sys->create_session(*ctx, {.near_distance = 0.05f, .far_distance = 100.0f, .blend = sxr::blend_mode::alpha_blend})  // -> result<unique_ptr<session>>
//   ctx must come from create_context and outlive the session; the system outlives every session
SXR_HAS_OPENXR                                      // 1/0: whether the build has the loader at all
```

## Drawing the controllers

```cpp
#include <shaped-xr/controller_routine.hh>
lib.add_package(sxr::shader_package());            // the application's shader library must mount these
sxr::controller_poses(*session)                    // -> vector<controller_pose>: tracked hands, from `grip` and `aim`
sxr::controller_routine::execute(pass, frame, hands)  // -> sg::routine_outcome; inside a view_count = 2 pass with frame.depth
//   declines until built: asking registers it, ctx.routines.tick() builds it; prewarm(ctx, frame.color.format()) asks early
```

## What a runtime offers

```cpp
#include <shaped-xr/runtime_report.hh>
sxr::inspect_runtime()                             // -> result<runtime_report>; needs a runtime, no headset, no graphics
r.runtime_name; r.runtime_version; r.system_name   // system_name empty: no headset connected
r.extensions                                       // every extension listed; a listed one need not work
r.features                                         // {name, is_supported}: what the system itself answers
```

`uv run dev.py example xr-probe` prints it.

## The frame loop

```cpp
while (session->poll_events())                     // false once the runtime wants the app to quit
    if (auto f = session->wait_frame())            // blocks on the runtime's pacing; none while not running
        session->submit_frame(cmd_or_null);        // EVERY waited frame is submitted, rendered or not
session->is_running(); session->is_focused();
session->blend()                                  // -> blend_mode actually in effect; opaque where what was asked for did not come up
sys->offers_blend(sxr::blend_mode::additive)      // -> bool, before a session exists
session->availability_of(sxr::capability::anchors) // -> availability: unsupported, needs_permission, denied, available
f.should_render                                    // false: draw nothing, still submit_frame(nullptr)
f.views[i].camera_to_world                         // tg::rigid_transform3f, tg's frame (LH, +y up, +z forward)
f.views[i].projection                              // LH zero-to-one: depth 0 at near, 1 at far
f.views[i].world_to_clip()                         // tg::mat4f, projection * inverse pose
f.color; f.depth                                   // sg::texture_2d_array, views.size() layers: view i into layer i
// render both in one pass: render_to({..., .view_count = 2}) with a `view_count = 2` SGL pipeline and @view_index
// read a frame back with create_session(*ctx, {.readable_color = true}) and a download before submit_frame
// clear color to transparent where blend() is not opaque: the frame meets the room by its premultiplied alpha
```

## Actions

```cpp
create_session(*ctx, {.actions = sxr::standard_controller_actions()})  // grip, aim, trigger, squeeze, thumbstick, thumbstick_click,
                                                   //   primary (A/X), secondary (B/Y), menu, haptic; Quest Touch + khr/simple
sxr::action_description{.name, .label, .kind = sxr::action_kind::scalar, .is_per_hand = true, .bindings = {{profile, path}}}
//   name: lowercase, digits, - _ . ; a per-hand binding starts /user/hand/left/ or /user/hand/right/; vibration binds /output/
session->pressed("primary", sxr::hand::right)     // bool;   released while unfocused or unbound
session->scalar("trigger", sxr::hand::left)        // f32 0..1
session->vector2("thumbstick", sxr::hand::left)    // tg::vec2f
session->pose("grip", sxr::hand::left)             // optional<rigid_transform3f>, tg's frame, at the frame's display time
session->vibrate("haptic", sxr::hand::left, amplitude, seconds)
//   synced by wait_frame; an unknown name asserts
```

Gotchas: the color format is the runtime's choice (sRGB 8-bit rgba or bgra), so build the pipeline for `f.color.format()`.
On dx12 `f.color` is sg's own image, copied into the runtime's at submit; on vulkan it is the runtime's.
