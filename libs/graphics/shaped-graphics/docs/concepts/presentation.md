# Concept: presentation (swapchain)

A [`swapchain`](../../src/shaped-graphics/present/swapchain.hh) is a chain of back buffers you render into and hand to a window.
`ctx.create_swapchain({.window = …})` returns one; `acquire_backbuffer()` gives this frame's target, and `ctx.submit_command_list_and_present(sc, cmd)` hands it to the display.

Two decisions shape the type, and both are about *not* special-casing presentation.
A back buffer is an ordinary texture as far as the rest of sg is concerned, and the present handshake is split so it never costs a command list of its own.

## The acquired view is the source of truth, not the swapchain

`swapchain` deliberately exposes **no size getter**.
`acquire_backbuffer()` resizes the chain to the window's current client area before handing a buffer back, so any size read off the swapchain could be one acquire out of date.
The returned `render_target_view`'s `width()` / `height()` are this frame's authoritative resolution, and everything sized against the frame should be sized against those.

A **headless** chain is fixed at its `headless_extent` and never resizes, so the acquired view's size is the one it was created with.
The size getter is still absent there, because a caller should read the frame's resolution the same way whichever kind of chain it holds.

The resize is checked **at most once per epoch**.
Resizing drains the GPU, so an unbounded check would let `acquire_backbuffer` stall — or advance an epoch — under a caller that only asked for a render target.
One check per epoch bounds that to the frame boundary the caller already owns.

A minimized or zero-size window keeps the last valid extent rather than resizing to nothing, since a zero-size chain is not creatable.

## Back buffers are ordinary textures

Each back buffer is wrapped as a **borrowed** `raw_texture` — the chain owns the storage, the wrapper only references it.
That is what lets a back buffer ride the normal path.
The [barriers](barriers.md) system tracks its layout like any other texture, and a [render-target view](views.md) is built from it the usual way.
[`cmd.raster.render_to`](raster-pipeline.md) then binds it with no presentation-specific branch.

The consequence to know is lifetime.
Borrowed storage is released synchronously when the wrapper dies, so the swapchain waits for the GPU to finish every queued present before releasing back buffers — on resize and on destruction alike.

## The present handshake is split around the submit

Presenting needs the back buffer transitioned to the present layout, which is GPU work, which needs a command list.
Rather than record a one-off list for that transition, `swapchain` splits the handshake into two protected steps that `context` drives:

- `record_present_transition(cmd)` — records the transition onto the caller's **still-open** list, before it is submitted;
- `present()` — queues the present, after that list has been submitted.

`ctx.submit_command_list_and_present` is the only caller, and it runs them around its `submit_command_list`.
So the transition costs nothing beyond the barrier it would need anyway, and the frame is still exactly one submit.

**Exactly one present per successful acquire.** Acquiring twice without presenting, or presenting without acquiring, asserts.

## Back-buffer reuse rides its own fence

Reuse is gated by a **present fence separate from the epoch fence**.
A present signals it, and the next acquire of that same buffer index waits for the value its previous frame recorded.

The two timelines answer different questions: [epochs](epochs.md) ask "has the GPU finished with this resource", the present fence asks "has the display finished with this buffer index".
A swapchain's `buffer_count` is the natural pipelining depth to pass `ctx.advance_epoch`, which is where the two meet.

## Presenting with no window

`headless_extent` presents at a fixed size with no window, and states both facts in one field.

That is deliberate: a handle is required exactly when the extent is unset, and ignored when it is set.
So a headless chain carrying a stale window handle is **unrepresentable** rather than merely invalid.
`is_windowed()` is the question everything else asks.

**Headless present is complete-the-frame-and-rotate.**
The chain cycles, and the presented buffer stays readable until its epoch retires.
That is what makes it pixel-verifiable, which is the entire reason for having it — a null presentation target would only prove the calls did not crash.

The two backends reach it differently, and the difference is worth knowing:

- **vulkan** creates a real `VkSwapchainKHR` over a `VK_EXT_headless_surface`, so every step the windowed path takes is taken here too.
  The acquire returns an index, the submit waits on the acquire semaphore, and the present waits on the render-finished one.
  What headless removes is only the display.
- **dx12** has no such surface: DXGI needs a real presentation target, so a headless chain is `buffer_count` ordinary render-target textures and a present that signals the fence and rotates the index.
  An emulation, and enough for the contract above.

## The window handle is platform-tagged

`swapchain_description::window` is an `sg::native_window`: a `window_platform` tag plus three slots — a `display`, a `handle` and a `window_id`.
Which slots are filled is the tag's business, and `is_valid()` states it per platform: win32 fills the handle, xlib and xcb a display plus the id, wayland a display plus the handle.
[`sr::window::native_window()`](../../../shaped-rendering/src/shaped-rendering/window.hh) is the supported producer.

A tag rather than a `void*` because a windowed Vulkan swapchain off Windows needs two values, not one — X11 a display plus an XID, wayland a display plus a surface.
It is also what lets a backend say *which* window system it cannot serve, instead of failing on a pointer it cannot interpret.

**Which platforms a build can actually serve is decided at compile time.**
A `VK_USE_PLATFORM_*` define pulls in that windowing system's own headers, so the Vulkan backend probes for them and compiles in the surface calls it found.
`vulkan_context::is_window_platform_supported` then answers for the instance as well — the extension has to be there too.
A platform this build has no surface call for reports that by name rather than crashing, and headless presentation is unaffected either way.

The rest of the description is fixed for the swapchain's lifetime.
`buffer_count` (at least 2), a renderable `format`, a `present_mode`, and an `enable_hdr` request.
`assert_valid()` checks the contract at the entry point, so a bad description asserts before any fallible GPU work.

## dx12

An `IDXGISwapChain3` in the **flip-discard** model, created on the context's direct queue.
The flip model forbids a multisampled back buffer, so the chain is always single-sampled — render multisampled offscreen and resolve.
DXGI's Alt+Enter fullscreen handling is suppressed (`DXGI_MWA_NO_ALT_ENTER`); the app owns presentation.

- **Present mode** is the sync interval: `vsync` → 1, `immediate` → 0.
  Tearing needs *both* adapter support (`DXGI_FEATURE_PRESENT_ALLOW_TEARING`) and `immediate`.
  Once negotiated, the `ALLOW_TEARING` flag must be passed at creation, on every `Present`, and on every `ResizeBuffers`.
- **HDR is best-effort.** `rgba16_float` asks for scRGB, any other wide format for HDR10, and an unsupported color space silently leaves the surface SDR.
- **Back buffers** come from `GetBuffer` and are wrapped as borrowed `dx12_texture`s with `texture_usage::render_target`.
  The swapchain keeps no RTV — the render pass creates one on demand from the `render_target_view` that acquire hands out.
- **The transition** goes through `dx12_command_list::transition_texture_to(tex, texture_layout::present)`.
  So it is computed from whatever layout the frame left the buffer in, and is a no-op when it is already there.
  It also leaves `present` as the canonical layout for the next frame.
- **Resize** waits on the present fence, releases the wrappers, calls `ResizeBuffers` and rebuilds.
  `ResizeBuffers` requires zero outstanding back-buffer references, which is exactly what the wait plus release provide.
- **Device loss** during acquire, present or resize marks the context lost and throws `sg::device_lost_exception`; other failures throw a generic `sg::exception` carrying the `HRESULT`.


## Deferred

See [TODO](../TODO.md) for the full list.
The ones worth knowing while using this:

- **deeper HDR** — metadata and tone-mapping beyond selecting a color space;
- **exclusive fullscreen** and **multi-window**.

## See also

- [swapchain.hh](../../src/shaped-graphics/present/swapchain.hh) — the description, the type, and the present handshake.
- [context](context.md) — the frame loop this sits in, and where `create_swapchain` lives among the other creates.
- [epochs](epochs.md) — the other timeline, and why `buffer_count` is the throttle to pass `advance_epoch`.
- [barriers](barriers.md) — the layout tracking the present transition goes through.
- [views](views.md) — the `render_target_view` acquire returns.
