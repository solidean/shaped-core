#pragma once

#include <clean-core/error/optional.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/present/native_window.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/views.hh> // render_target_view — the acquire_backbuffer result
#include <shaped-graphics/types.hh>
#include <typed-geometry/linalg/vec.hh>

/// How presentation paces frames against the display's vertical blank.
enum class sg::present_mode : sg::u8
{
    vsync,     ///< wait for vblank — no tearing, capped to the refresh rate (DX12 sync interval 1)
    immediate, ///< present as soon as ready — may tear, uncapped (DX12 sync interval 0 + allow-tearing)
};

/// How a swapchain is created.
/// Defaults describe a plain double-buffered vsync surface.
struct sg::swapchain_description
{
    /// The OS window to present into, named in its own windowing system's terms — see sg::native_window.
    /// Must name a window unless `headless_extent` is set.
    native_window window;

    /// Present with no window, at this fixed size.
    ///
    /// One field states both facts, which is what makes a headless chain with a stale window handle unrepresentable
    /// rather than merely invalid: a handle is required exactly when this is unset, and ignored when it is set.
    ///
    /// A headless chain still cycles: `present()` completes the frame and rotates to the next back buffer, and the
    /// one just presented stays readable until its epoch retires.
    /// That is what makes it pixel-verifiable — the point of having it — where a null presentation target would only
    /// prove the calls did not crash.
    ///
    /// It also never resizes, since there is no window to follow, so the extent here is every frame's extent.
    ///
    /// A `tg::vec2i` rather than a size type, matching `render_target_view::size()` — which is what a caller compares
    /// this against, and the only sizes typed-geometry has today.
    cc::optional<tg::vec2i> headless_extent = {};

    /// Whether this chain presents to a window rather than headlessly.
    [[nodiscard]] bool is_windowed() const { return !headless_extent.has_value(); }

    /// Number of back buffers in the flip chain; must be >= 2.
    /// Also the natural pipelining depth a windowed renderer passes to ctx.advance_epoch.
    int buffer_count = 2;

    /// Back-buffer texel format; must be a color (renderable) format.
    /// Usually bgra8_unorm, or a wide format — rgba16_float / rgb10a2_unorm — with enable_hdr.
    pixel_format format = pixel_format::bgra8_unorm;

    /// Frame pacing (see present_mode). Qualified type name avoids the member/type name clash.
    sg::present_mode present_mode = sg::present_mode::vsync;

    /// Request an HDR colorspace on the surface — best-effort, pair with a wide `format`.
    bool enable_hdr = false;

    /// Whether the contract holds: non-null handle, buffer_count >= 2, renderable format.
    /// The non-asserting counterpart of assert_valid().
    [[nodiscard]] bool is_valid() const;

    /// Asserts the contract (see is_valid) with a per-invariant message.
    /// Runs from swapchain's constructor, and a backend also calls it at the top of its create path so a bad desc asserts before any fallible GPU work.
    void assert_valid() const;
};

/// A presentation surface: a chain of back buffers you render into and hand to the display.
/// Abstract — a backend subclasses it; obtain one from ctx.create_swapchain(...).
/// A windowed chain auto-resizes: acquire_backbuffer resizes to the current client size, at most once per epoch.
/// A headless one is fixed at its `headless_extent` and never resizes.
///
/// Per-frame use: acquire_backbuffer() -> render into the returned target -> ctx.submit_command_list_and_present(sc, cmd).
/// The returned render_target_view is the source of truth for this frame's size (rt.width() / rt.height()).
/// The swapchain intentionally exposes no size getter, since a later acquire may resize under you.
class sg::swapchain : public std::enable_shared_from_this<swapchain>
{
public:
    virtual ~swapchain();

    /// The current back buffer as a render target, resizing the chain to the window first if it changed (checked at most once per epoch, and never for a headless chain).
    /// Render into it this frame, then present it via ctx.submit_command_list_and_present.
    /// The returned view's width() / height() are this frame's authoritative resolution.
    /// Throws sg::device_lost_exception if the device was lost.
    [[nodiscard]] virtual render_target_view acquire_backbuffer() = 0;

    // Creation parameters, fixed for the swapchain's lifetime.
    [[nodiscard]] native_window const& window() const { return _desc.window; }
    [[nodiscard]] int buffer_count() const { return _desc.buffer_count; }
    [[nodiscard]] pixel_format format() const { return _desc.format; }
    [[nodiscard]] sg::present_mode present_mode() const { return _desc.present_mode; }
    [[nodiscard]] bool is_hdr_enabled() const { return _desc.enable_hdr; }
    [[nodiscard]] bool is_windowed() const { return _desc.is_windowed(); }
    [[nodiscard]] swapchain_description const& description() const { return _desc; }

    /// Tell the chain the window's current client size, in pixels.
    ///
    /// Only wayland needs this, and there it is not optional: its surface has no size of its own, so nothing else can
    /// tell the chain the window grew.
    /// Elsewhere the surface reports its own extent and this is ignored, which is what lets a caller call it
    /// unconditionally rather than branching on the platform.
    ///
    /// Call it before `acquire_backbuffer`, which is where the value is consumed — the resize itself still happens at
    /// most once per epoch, so calling this every frame costs nothing.
    /// A zero or negative component is ignored, so a minimized window needs no special case.
    void set_window_size(tg::vec2i size) { _requested_size = size; }

protected:
    explicit swapchain(swapchain_description const& desc);

    // The present handshake, driven by context::submit_command_list_and_present (a friend).
    // Splitting it around the submit lets the back-buffer's final layout transition ride the caller's already-open command list instead of a wasteful one-off list.
    friend class context;

    /// Record the back buffer's transition to the present layout onto `cmd`, a no-op if it is already there.
    /// Called on the still-open command list before it is submitted.
    virtual void record_present_transition(command_list& cmd) = 0;

    /// Hand the acquired back buffer to the display: queue the present and signal reuse.
    /// Called after the transition-carrying command list has been submitted.
    /// Exactly one per successful acquire_backbuffer().
    virtual void present() = 0;

    swapchain_description _desc;

    /// The client size the application last pushed, seeded from `_desc.window.client_size`.
    /// Only a backend whose surface defers reads it; see set_window_size.
    tg::vec2i _requested_size = tg::vec2i(0, 0);
};
