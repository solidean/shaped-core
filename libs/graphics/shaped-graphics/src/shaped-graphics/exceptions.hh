#pragma once

#include <clean-core/error/result.hh> // cc::any_error
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh> // buffer_usage

namespace sg
{
/// Base of every shaped-graphics exception. Carries a formatted message; catch a concrete derived type
/// for structured context, or `sg::exception` for "any sg failure". These are thrown only by the
/// throwing create façades and the submit/advance path — the `try_*` surface never throws.
///
/// Example:
///   try { auto buf = ctx->persistent.create_raw_buffer(size, usage); ... }
///   catch (sg::device_lost_exception const&) { rebuild_context(); }
///   catch (sg::allocation_exception const& e) { shrink_and_retry(e.size_in_bytes()); }
class exception
{
public:
    explicit exception(cc::string message) : _message(cc::move(message)) {}
    virtual ~exception() = default;

    /// Human-readable description of the failure.
    [[nodiscard]] cc::string_view message() const { return _message; }

protected:
    cc::string _message;
};

/// The GPU device was lost (driver reset / TDR / removed adapter). Sticky: the owning context stays
/// lost once this fires — tear it down and recreate. Surfaced at submit / advance / fence waits and
/// from the throwing create façades; never through the `try_*` channel.
class device_lost_exception final : public exception
{
public:
    explicit device_lost_exception(cc::string_view reason)
      : exception(cc::format("graphics device lost: {}", reason)), _reason(reason)
    {
    }

    /// Backend-provided reason (e.g. the D3D12 device-removed reason).
    [[nodiscard]] cc::string_view reason() const { return _reason; }

private:
    cc::string _reason;
};

/// A GPU resource allocation failed (out of device memory, or a fixed heap / descriptor region is
/// exhausted). Recoverable in principle by a coarse handler that frees or resizes — the classic
/// bubble-up-to-a-budget failure. Carries the requested size and the underlying backend error.
class allocation_exception final : public exception
{
public:
    allocation_exception(cc::string what, isize size_in_bytes, cc::any_error const& error)
      : exception(cc::format("{} (requested {} bytes): {}", what, size_in_bytes, error.to_string())),
        _size_in_bytes(size_in_bytes)
    {
    }

    /// The size (in bytes) the failed request asked for; 0 when not size-driven.
    [[nodiscard]] isize size_in_bytes() const { return _size_in_bytes; }

private:
    isize _size_in_bytes = 0;
};

/// Building a binding_group_layout, pipeline_layout, or compute / raster / raytracing pipeline failed — a
/// shader/root-signature/PSO compile or create error the driver reported. Carries the pipeline's entry point for context.
class pipeline_creation_exception final : public exception
{
public:
    pipeline_creation_exception(cc::string_view entry_point, cc::any_error const& error)
      : exception(cc::format("pipeline creation failed (entry point '{}'): {}", entry_point, error.to_string())),
        _entry_point(entry_point)
    {
    }

    /// Entry point of the shader whose pipeline failed to build (empty if not applicable).
    [[nodiscard]] cc::string_view entry_point() const { return _entry_point; }

private:
    cc::string _entry_point;
};

/// Creating a swapchain failed — a bad window handle, an unsupported surface format, or a DXGI/driver
/// error creating the flip chain. Recoverable in principle (fix the window / format and retry). Carries
/// the underlying backend error.
class swapchain_creation_exception final : public exception
{
public:
    explicit swapchain_creation_exception(cc::any_error const& error)
      : exception(cc::format("swapchain creation failed: {}", error.to_string()))
    {
    }
};

/// Instantiating a binding_group against its layout failed: a bound view names no binding, a view's
/// access/shape doesn't match its binding, or a declared binding was left unprovided. A caller-side
/// mistake in wiring views to a layout (as opposed to running out of GPU memory). Carries the message
/// the backend produced (which names the offending binding).
class binding_group_exception final : public exception
{
public:
    explicit binding_group_exception(cc::any_error const& error)
      : exception(cc::format("binding_group creation failed: {}", error.to_string()))
    {
    }
};
} // namespace sg
