#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/types.hh>
#include <shaped-graphics/views.hh>     // render_target_view — the acquire_backbuffer result
#include <typed-geometry/linalg/vec.hh> // tg::vec2i — the swapchain resolution

#include <memory>

namespace sg
{
/// How present() paces frames against the display's vertical blank.
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
};

/// A window presentation surface: a chain of back buffers you render into and hand to the display.
/// Abstract — a backend subclasses it; obtain one from ctx.create_swapchain(...). Auto-resizes to its
/// window (acquire_backbuffer resizes the chain to the current client size before returning a target).
///
/// Per-frame use: acquire_backbuffer() -> render into the returned target -> present(). Exactly one
/// present() must follow each successful acquire_backbuffer().
class swapchain : public std::enable_shared_from_this<swapchain>
{
public:
    virtual ~swapchain();

    /// The current back buffer as a render target, resizing the chain to the window first if it changed.
    /// Render into it this frame, then call present() exactly once. Throws sg::device_lost_exception if the
    /// device was lost.
    [[nodiscard]] virtual render_target_view acquire_backbuffer() = 0;

    /// Hands the back buffer acquired this frame to the display. Call exactly once after each successful
    /// acquire_backbuffer(). Throws sg::device_lost_exception if the device was lost.
    virtual void present() = 0;

    /// Current back-buffer resolution in pixels, {width, height} — tracks auto-resize.
    [[nodiscard]] virtual tg::vec2i get_size() const = 0;
    [[nodiscard]] virtual int get_width() const = 0;
    [[nodiscard]] virtual int get_height() const = 0;

    // Creation parameters, fixed for the swapchain's lifetime.
    [[nodiscard]] void* get_native_window_handle() const { return _desc.native_window_handle; }
    [[nodiscard]] int get_buffer_count() const { return _desc.buffer_count; }
    [[nodiscard]] pixel_format get_format() const { return _desc.format; }
    [[nodiscard]] sg::present_mode get_present_mode() const { return _desc.present_mode; }
    [[nodiscard]] bool is_hdr_enabled() const { return _desc.enable_hdr; }
    [[nodiscard]] swapchain_description const& get_description() const { return _desc; }

    /// Asserts `desc` satisfies the contract: non-null handle, buffer_count >= 2, renderable format. Runs
    /// from the constructor; a backend also calls it at the top of its create path so a bad desc asserts
    /// before any fallible GPU work.
    static void validate_description(swapchain_description const& desc);

protected:
    explicit swapchain(swapchain_description const& desc);

    swapchain_description _desc;
};
} // namespace sg
