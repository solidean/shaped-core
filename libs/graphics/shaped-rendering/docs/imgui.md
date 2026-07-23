# Dear ImGui in shaped-rendering

The **renderer half** of a Dear ImGui backend, drawn entirely through `sg`.
Upstream's own `backends/` are not vendored — nothing here reaches for D3D12 or Vulkan.

ImGui is vendored in-tree at `extern/imgui/`, tracking the **docking** branch at a pinned release tag
(see `extern/imgui/vendor-imgui.py`).
`shaped-rendering` links it `PUBLIC`, so an application includes `<imgui/imgui.h>` and calls `ImGui::Begin(...)` directly.

## The vendored bundle

`extern/imgui/` is a small bundle, all compiled into one `imgui` target.
Every header is namespaced under `imgui/`, so includes read `<imgui/imgui.h>`, `<imgui/implot.h>`, `<imgui/imguizmo.h>` (lowercased for consistency).

- **Dear ImGui** — the core, pinned at a docking-branch release tag.
- **ImPlot** — immediate-mode plotting. Pinned at a `master` commit: its latest release (v0.16) predates ImGui 1.92's draw/texture API, and only `master` tracks it.
- **ImGuizmo** — 3D manipulation gizmos. Pinned at a `master` commit (it cuts no release tags).

Neither addon has an `sr` wrapper yet — they are there to include and draw through the same imgui frame.

shaped-code's own additions live in `extern/imgui/shaped/imgui/`, outside `include/` and `src/` so a re-vendor leaves them alone:

- `<imgui/imgui_fwd.hh>` — forward declarations, for a header that only *names* an imgui type.
- `<imgui/imgui_config.hh>` — imgui's injected user config (`IMGUI_USER_CONFIG`). Adds the implicit `ImVec2`/`ImVec4` ↔ `tg::vec2f`/`tg::vec4f` casts. Baked into every translation unit, and includes nothing — tg is only forward-declared, so the imgui target never depends upward on typed-geometry.
- `<imgui/imgui_sc.hh>` — the shaped-code interop umbrella. Pulls in both sides (the `impl/` headers under it); include one directly for just one side:
  - clean-core: `ImGui::TextUnformatted(cc::string_view)` displays zero-copy through imgui's `[begin, end)` overload; `ImGui::InputText(label, cc::string*, …)` (plus the multiline and hint variants) edits a `cc::string`, growing and shrinking it as the user types — the imgui_stdlib `std::string` contract.
  - typed-geometry: the definitions behind the `ImVec2`/`ImVec4` ↔ `tg` casts declared in `imgui_config.hh`.

  Header-only, so the interop stays available to any imgui consumer without the imgui target depending upward on clean-core or typed-geometry.

## Using it

```cpp
#include <shaped-rendering/all.hh>

// Once, at startup: register sr's shaders so its routines have something to compile.
slib::shader_library lib;
lib.add_compiler(slib::create_dxc_compiler().value());
lib.add_package(sr::shader_package());
lib.start_hot_reload();

auto imgui = sr::imgui_context::create();

// Per frame:
wsys->poll_events();
imgui.process_events(*wsys);          // feed this frame's input
imgui.begin_frame(*win, dt);          // size from the window; hands it imgui's text-input intent
ImGui::ShowDemoWindow();
imgui.end_frame();

// imgui owns this window — render the whole frame (main + viewports) and present, in one call.
sr::render_imgui(imgui, ctx, swapchain);
```

To composite imgui over your own rendering instead — imgui as an overlay, not the window's whole content —
skip `render_imgui` and draw it into your own pass:

```cpp
auto cmd = ctx.create_command_list();
{
    auto pass = cmd->raster.render_to({.color_targets = {backbuffer.preserved()}});
    sr::imgui_routine::execute(pass, ImGui::GetDrawData());  // reads the target's format + size off the scope
}
ctx.submit_command_list(cc::move(cmd));
```

`render_imgui` is a thin facade over `execute` + the viewport calls;
it owns the main present, which is why the overlay case uses the primitives directly.
See the multi-viewport section for the two viewport calls it sequences for you.

## Two types

- **`sr::imgui_routine`** — the renderer.
  A render routine that owns the atlas textures and a pipeline per target format, records the draws, and holds a swapchain per secondary viewport.
- **`sr::imgui_context`** — the platform half: the `ImGuiContext`, the frame bracket, the renderer capability flags, input, cursor, clipboard, and the platform viewport callbacks.

The split is imgui's own platform-backend / renderer-backend line, not an arbitrary one.

Everything the routine mutates — atlas textures, pipelines, and the shader-derived layouts — lives in a single `cc::mutex<state>`, locked once at the top of `execute()`.
That is the contract `sg::render_routine` sets out: `acquire()` hands the same instance to every caller on the context, so the routine guards its own state.
One mutex over all of it keeps the rule checkable by inspection — see [shaped-graphics/docs/render-routines.md](../../shaped-graphics/docs/render-routines.md#threading).

Note what is *not* rebuilt on reload: `init_declare` clears the pipelines and layouts sitting right next to the texture registry, and deliberately leaves the registry alone.
The atlas has nothing to do with our shaders, and a test pins that.

This frame's geometry is deliberately *not* in there.
It comes from the transient scope and lives on the stack for one `execute()`, which is what makes the call re-entrant across viewports —
with multi-viewport imgui calls it once per viewport per frame, and geometry cached on the routine would have each viewport overwrite the last one's.

## The 1.92 texture protocol

This is the part that is not optional and not obvious.

Since ImGui 1.92 the font atlas is **not** built once and uploaded once.
Glyphs rasterize on demand, so the atlas grows and is patched at runtime.
A backend sets `ImGuiBackendFlags_RendererHasTextures` and drains `ImDrawData::Textures` every frame,
servicing `WantCreate` / `WantUpdates` / `WantDestroy` and reporting back through `SetTexID` / `SetStatus`.
Skipping any of it wedges the atlas.

`sr::impl::imgui_texture_registry` implements it. Three things worth knowing:

- Uploads go through **`ctx.upload`**, the async copy queue, not a command list.
  A font atlas is bulk asset data, and a later command list that samples the texture waits on the copy automatically.
  That is why the registry needs only a context; the command list is what the *geometry* upload and the draws need.
- `ctx.upload` is fire-and-forget and holds its pin until the copy runs, so each upload owns its bytes.
  That costs nothing extra: ImGui may free `ImTextureData`'s pixels once the status goes `OK`, and the partial-update path has to repack out of the atlas pitch anyway.
- `SetStatus(Destroyed)` **turns back into `WantCreate`** while the pixels are still around (see `imgui.h`).
  Destroy is not terminal: a texture the backend reports destroyed can be asked for again, so the registry must be ready to recreate one it just dropped.

## Constraints

- **The target must not be an sRGB format.** ImGui's vertex colors and atlas are already sRGB-encoded 8-bit; a `*_unorm_srgb` target would encode them a second time.
  Bind a non-srgb view of the same resource instead.
  The routine asserts rather than compensating in the shader — silently undoing something the caller did not ask for is worse than refusing.
- **A rendering scope must be open.** `execute()` records the geometry upload and the draws onto the same command list, inside the caller's scope.
  That is fine here because sg's dx12 backend opens a scope with `OMSetRenderTargets` rather than the D3D12 Render Passes API, which is what would forbid a copy inside it.
- **Record imgui from one thread.** The state is mutex-guarded, so two threads cannot corrupt it —
  but the routine is a per-context singleton holding *this frame's* geometry, so two threads drawing imgui against one context would fight over it.

## Input

`process_events` maps `sr::input_event` onto imgui's event queue. Keyboard, text, mouse buttons, motion and
wheel all go through; `sr::scancode` to `ImGuiKey` and the mouse-button reorder live in
`impl/imgui_input_translation.hh` and are unit-tested there.

Three things are worth knowing:

- **imgui's mouse-button order is not sr's.** sr is left / middle / right, imgui is left / right / middle.
  Passing the enum through unconverted swaps middle and right, and nothing looks wrong until someone middle-clicks.
- **Feeding only queues; `NewFrame` commits.** So `process_events` must run before `begin_frame`, and reading `io` for what you just fed only works after the frame has begun.
- **A wheel event deliberately carries no position.**
  imgui already has the cursor from the motion events, and with `ConfigInputTrickleEventQueue` (imgui's default) a position change followed by a wheel is split across two frames —
  restating it would delay every scroll by a frame.
  A *button* event does send its position, because there the trickle is the point: the click lands at the position it happened at.

Ask `wants_keyboard()` / `wants_mouse()` before acting on the same input yourself, or a camera will spin while the user drags a slider.

## Cursors and the clipboard

`begin_frame(window&, dt)` also hands imgui the platform bits that are not input:

- **The pointer shape**, through `window_system::set_cursor` — so a resize handle shows a resize cursor and a text field an I-beam.
  Only taken over while `wants_mouse()` is true, so an application drawing its own cursor over the 3D view is not fought over every frame.
  `ImGuiMouseCursor_None` hides the pointer instead of picking a shape.
- **The clipboard**, through `ImGuiPlatformIO::Platform_{Get,Set}ClipboardTextFn` pointed at the window system, so ctrl+C and ctrl+V in a text field reach the real one.
  Installed on the first window-driven frame rather than at `create()`, because that is the first point a `window_system` is in reach.

Both act on the frame just ended, like the text-input handover: imgui decides hover at `NewFrame` from the previous frame's layout, so a cursor change lands two frames after the pointer moves.
That is what every imgui backend does and is invisible at frame rate.

## Multi-viewport

An imgui window dragged outside the main OS window becomes its own OS window.
It is **opt-in**, because turning it on changes the contract for everything else:

```cpp
auto imgui = sr::imgui_context::create({.enable_viewports = true});
```

If you use `render_imgui`, it does all of this for you and you can skip the rest of this section.
Driving the viewports by hand (the compositing path) is two calls in the frame loop, after the main window's draw is recorded but **before** its present:

```cpp
imgui.update_viewports();                   // opens / moves / closes the OS windows
sr::imgui_routine::render_viewports(*ctx);  // draws and presents each, own swapchain per viewport

ctx->submit_command_list_and_present(*sc, cc::move(cmd));   // main window last
```

The order is not cosmetic.
Moving an OS window and presenting its new content are separate events, and in between the window displays content drawn for where it used to be.
Put a vsync-blocking main present in that gap and it becomes a full frame: everything inside a window being dragged — dock markers most visibly — lags the window by however far it moved.
This is the ordering imgui's own examples use, and the order `render_imgui` runs them in.

**Both changes are load-bearing, and neither is optional once the flag is on.**

- **Every imgui coordinate becomes desktop-space.**
  Mouse positions and `ImGui::SetNextWindowPos` are no longer relative to the main window, because imgui hit-tests a position against every viewport's rectangle to decide which one the pointer is over.
  `process_event` translates by the event's window position, which is why an `input_event` must name its window.
  Code that hardcodes `SetNextWindowPos(ImVec2(0, 0))` has to offset by `ImGui::GetMainViewport()->Pos` instead.
- **`update_viewports` must run every frame.**
  Skipping it does not merely leave viewport windows unmoved: imgui only hit-tests the mouse against viewports `UpdatePlatformWindows` has published, so *nothing hovers at all*.
  That failure looks nothing like its cause, so `begin_frame` asserts on it.

The split follows imgui's own: `sr::imgui_context` owns the platform callbacks (`Platform_CreateWindow` and friends, each one line of `sr::window`), and `sr::imgui_routine` owns the swapchains.
Of imgui's renderer callbacks only `Renderer_DestroyWindow` is registered —
sr drives the drawing itself in `render_viewports` rather than through `ImGui::RenderPlatformWindowsDefault`,
so submission stays where the caller can see it, but imgui is still the only thing that knows when a viewport dies.

A viewport's swapchain is created the first frame it appears and resizes itself from its window, so `Renderer_SetWindowSize` has nothing to do.
They are `bgra8_unorm`, independent of the main swapchain's format — imgui's colors are already sRGB-encoded, so an sRGB target would encode them twice.

`draw_data->DisplayPos` is what makes this work in the routine, and it was folded into the ortho projection and the scissor math before viewports existed.
It is pinned end-to-end by `sr::imgui_routine - a non-zero display pos shifts what lands on the target`:
arithmetic being right is not the same as it reaching the draw, and until viewports landed nothing exercised a non-zero value.

### Known upstream issue

Dragging a detached viewport window over another makes the dock-target markers drawn inside it lag the window, settling as soon as it stops.
That is an ImGui docking-branch bug, not ours — it reproduces with upstream's own `example_win32_directx12` at the tag we vendor, with none of our code involved.
See [TODO.md](TODO.md) for what was ruled out and how.

## Not wired yet

- **`ImDrawCmd::UserCallback`.** Skipped.
  Supporting it also means supporting `ImDrawCallback_ResetRenderState`.
  No ImGui core path emits one.
