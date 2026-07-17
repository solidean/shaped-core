#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/types.hh>
#include <shaped-graphics/views.hh> // render_target_view — the acquire_backbuffer result

#include <memory>

namespace sg
{
/// How presentation paces frames against the display's vertical blank.
enum class present_mode : u8
{
    vsync,     ///< wait for vblank — no tearing, capped to the refresh rate (DX12 sync interval 1)
    immediate, ///< present as soon as ready — may tear, uncapped (DX12 sync interval 0 + allow-tearing)
};

/// How a swapchain is created. Defaults describe a plain double-buffered vsync surface.
struct swapchain_description
{
    /// OS window to present into — an HWND on Windows. Opaque here so sg core stays backend-agnostic; the
    /// backend reinterprets it. Must be non-null.
    void* native_window_handle = nullptr;

    /// Number of back buffers in the flip chain; must be >= 2. Also the natural pipelining depth a windowed
    /// renderer passes to ctx.advance_epoch.
    int buffer_count = 2;

    /// Back-buffer texel format; must be a color (renderable) format — usually bgra8_unorm, or a wide format
    /// (rgba16_float / rgb10a2_unorm) with enable_hdr.
    pixel_format format = pixel_format::bgra8_unorm;

    /// Frame pacing (see present_mode). Qualified type name avoids the member/type name clash.
    sg::present_mode present_mode = sg::present_mode::vsync;

    /// Request an HDR colorspace on the surface — best-effort, pair with a wide `format`.
    bool enable_hdr = false;

    /// Whether the contract holds: non-null handle, buffer_count >= 2, renderable format. The
    /// non-asserting counterpart of assert_valid().
    [[nodiscard]] bool is_valid() const;

    /// Asserts the contract (see is_valid) with a per-invariant message. Runs from swapchain's
    /// constructor; a backend also calls it at the top of its create path so a bad desc asserts before
    /// any fallible GPU work.
    void assert_valid() const;
};

/// A window presentation surface: a chain of back buffers you render into and hand to the display.
/// Abstract — a backend subclasses it; obtain one from ctx.create_swapchain(...). Auto-resizes to its
/// window: acquire_backbuffer resizes the chain to the current client size (at most once per epoch).
///
/// Per-frame use: acquire_backbuffer() -> render into the returned target -> ctx.submit_command_list_and_
/// present(sc, cmd). The returned render_target_view is the source of truth for this frame's size
/// (rt.width()/rt.height()) — the swapchain intentionally exposes no size getter, since a later acquire may
/// resize under you.
class swapchain : public std::enable_shared_from_this<swapchain>
{
public:
    virtual ~swapchain();

    /// The current back buffer as a render target, resizing the chain to the window first if it changed
    /// (checked at most once per epoch). Render into it this frame, then present it via
    /// ctx.submit_command_list_and_present. Throws sg::device_lost_exception if the device was lost. The
    /// returned view's width()/height() are this frame's authoritative resolution.
    [[nodiscard]] virtual render_target_view acquire_backbuffer() = 0;

    // Creation parameters, fixed for the swapchain's lifetime.
    [[nodiscard]] void* native_window_handle() const { return _desc.native_window_handle; }
    [[nodiscard]] int buffer_count() const { return _desc.buffer_count; }
    [[nodiscard]] pixel_format format() const { return _desc.format; }
    [[nodiscard]] sg::present_mode present_mode() const { return _desc.present_mode; }
    [[nodiscard]] bool is_hdr_enabled() const { return _desc.enable_hdr; }
    [[nodiscard]] swapchain_description const& description() const { return _desc; }

protected:
    explicit swapchain(swapchain_description const& desc);

    // The present handshake, driven by context::submit_command_list_and_present (a friend). Splitting it
    // around the submit lets the back-buffer's final layout transition ride the caller's already-open
    // command list instead of a wasteful one-off list.
    friend class context;

    /// Record the back buffer's transition to the present layout onto `cmd` (a no-op if it is already
    /// there). Called on the still-open command list before it is submitted.
    virtual void record_present_transition(command_list& cmd) = 0;

    /// Hand the acquired back buffer to the display (queue the present + signal reuse). Called after the
    /// transition-carrying command list has been submitted. Exactly one per successful acquire_backbuffer().
    virtual void present() = 0;

    swapchain_description _desc;
};
} // namespace sg
