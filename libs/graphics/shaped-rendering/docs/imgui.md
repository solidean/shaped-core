# Dear ImGui in shaped-rendering

The **renderer half** of a Dear ImGui backend, drawn entirely through `sg`.
Upstream's own `backends/` are not vendored — nothing here reaches for D3D12 or Vulkan.

ImGui is vendored in-tree at `extern/imgui/`, tracking the **docking** branch at a pinned release tag
(see `extern/imgui/vendor-imgui.py`).
`shaped-rendering` links it `PUBLIC`, so an application includes `<imgui.h>` and calls `ImGui::Begin(...)` directly.

## Using it

```cpp
#include <shaped-rendering/all.hh>

// Once, at startup: register sr's shaders so its routines have something to compile.
slib::shader_library lib;
lib.add_compiler(slib::create_dxc_compiler().value());
lib.add_package(sr::shader_package());
lib.start_hot_reload();

auto imgui = sr::imgui_context::create();
auto renderer = sr::imgui_renderer::create(ctx);

// Per frame:
imgui.begin_frame({.display_size = tg::vec2i(width, height), .delta_time = dt});
ImGui::ShowDemoWindow();
imgui.end_frame();

auto* const draw_data = ImGui::GetDrawData();
auto cmd = ctx.create_command_list();

renderer.prepare(*cmd, draw_data);                  // BEFORE the rendering scope
{
    auto pass = cmd->raster.render_to({.color_targets = {backbuffer.preserved()}});
    renderer.render(*cmd, draw_data, {.target_format = fmt, .target_size = size});
}
ctx.submit_command_list(cc::move(cmd));
```

## Why it is three types, not one routine

`sg::render_routine` exists for state **derived from shaders** — layouts and pipelines, rebuilt whenever
shaders reload, handed out as a `const&`.
ImGui's GPU textures have a lifetime driven by ImGui, not by our shaders, so putting them in a routine
would mean a routine whose state must survive exactly the event a routine is built to re-run on.

So the split follows the lifetimes:

- **`sr::imgui_renderer`** — an ordinary object the application owns. Holds the atlas textures and this
  frame's geometry. This is where everything with a lifetime lives.
- **`sr::imgui_draw_routine`** — a plain render routine. Holds the binding group layout, the pipeline
  layout, and a pipeline per target format. Reads everything else out of its `params`.
- **`sr::imgui_context`** — the platform half: the `ImGuiContext`, the frame bracket, and the
  `TODO(windowing)` markers for input and multi-viewport.

The sg render-routine API needed no change to accommodate this.
That the routine could stay `const` is the sign the split is in the right place.

## The 1.92 texture protocol

This is the part that is not optional and not obvious.

Since ImGui 1.92 the font atlas is **not** built once and uploaded once.
Glyphs rasterize on demand, so the atlas grows and is patched at runtime.
A backend sets `ImGuiBackendFlags_RendererHasTextures` and drains `ImDrawData::Textures` every frame,
servicing `WantCreate` / `WantUpdates` / `WantDestroy` and reporting back through `SetTexID` / `SetStatus`.
Skipping any of it wedges the atlas.

`sr::impl::imgui_texture_registry` implements it. Three things worth knowing:

- Uploads go through **`ctx.upload`**, the async copy queue, not a command list. A font atlas is bulk asset
  data, and a later command list that samples the texture waits on the copy automatically. That is why
  `service_requests` needs only a context — the command list `prepare()` takes is for the *geometry*.
- `ctx.upload` is fire-and-forget and holds its pin until the copy runs, so each upload owns its bytes.
  That costs nothing extra: ImGui may free `ImTextureData`'s pixels once the status goes `OK`, and the
  partial-update path has to repack out of the atlas pitch anyway.
- `SetStatus(Destroyed)` **turns back into `WantCreate`** while the pixels are still around
  (see `imgui.h`). Destroy is not terminal, and `release_textures` relies on exactly that to make a later
  frame rebuild.

## Constraints

- **The target must not be an sRGB format.** ImGui's vertex colors and atlas are already sRGB-encoded
  8-bit; a `*_unorm_srgb` target would encode them a second time. Bind a non-srgb view of the same
  resource instead. The routine asserts rather than compensating in the shader — silently undoing
  something the caller did not ask for is worse than refusing.
- **`prepare()` must run before the caller opens its rendering scope.** The geometry upload is a copy, and
  a copy inside a rendering scope is invalid. `render()` asserts that `prepare()` ran for this frame.
- **Drive it from one thread.** Both calls carry per-frame state.

## Not wired yet

- **Input.** `imgui_context` publishes only size and timestep, so the UI renders but does not respond.
  The windowing layer fills in the `io.Add*Event` feed, cursors and clipboard — see the `TODO(windowing)`
  block in `imgui_context.cc` for the specific list.
- **Multi-viewport.** `ImGuiConfigFlags_ViewportsEnable` is off. It needs the platform layer to create OS
  windows and a swapchain per viewport. Docking itself works today and needs none of that.
  The draw routine already folds `draw_data->DisplayPos` into its projection and scissor math, which is
  the piece that would otherwise have to change.
- **`ImDrawCmd::UserCallback`.** Skipped. Supporting it also means supporting
  `ImDrawCallback_ResetRenderState`. No ImGui core path emits one.
