#pragma once

// Single include gate for the metal-cpp headers plus the shared string and error helpers.
// metal TUs include this, not <Metal/Metal.hpp> directly.

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <clean-core/error/result.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/backends/metal/fwd.hh> // where every backend type is declared, autorelease_scope included
#include <shaped-graphics/fwd.hh>                // also what puts the bare sized aliases in scope inside sg

namespace sg::backend::metal
{
/// The OS version this backend refuses below, and the reason the whole backend is Metal 4.
///
/// Metal 4 is the generation that made barriers explicit, gave a command buffer its own allocator, and replaced
/// `useResource` with residency sets — which is what lets sg's access model, epoch-recycled allocators and per-list
/// touched-resource lists map across rather than be emulated.
/// A Metal 3 path would be a second recording backend behind the same context, exercised on nobody's machine.
///
/// Stated as a hard floor rather than probed per capability, and refused by name: see
/// libs/graphics/shaped-graphics/docs/writing-a-backend.md.
inline constexpr int k_required_macos_major = 26;

} // namespace sg::backend::metal

/// An autorelease pool scoped to a block, because metal-cpp's `alloc`-less factory methods hand back autoreleased objects.
///
/// Every Metal entry point that returns an object we do not own returns an autoreleased one, and without a pool in
/// scope those accumulate until the process exits — which in a test binary that creates and destroys devices reads as
/// a leak of the device itself.
/// So one of these opens at the top of any scope that calls into Metal and is not already inside somebody else's pool:
/// context creation, a frame, a command-list submit.
class sg::backend::metal::autorelease_scope
{
public:
    autorelease_scope() : _pool(NS::AutoreleasePool::alloc()->init()) {}
    ~autorelease_scope() { _pool->release(); }

    autorelease_scope(autorelease_scope const&) = delete;
    autorelease_scope& operator=(autorelease_scope const&) = delete;

private:
    NS::AutoreleasePool* _pool;
};

namespace sg::backend::metal
{

/// An autoreleased NS::String over `text`, for a label or a name Metal takes.
/// The bytes are copied, so `text` need not outlive the call.
[[nodiscard]] NS::String* ns_string(cc::string_view text);

/// An NS::String's UTF-8 bytes as a cc::string; empty for a null string.
[[nodiscard]] cc::string to_string(NS::String const* string);

/// The message half of `metal_error`: what `error` said, or a note that it said nothing.
[[nodiscard]] cc::string describe_error(NS::Error const* error, cc::string_view what);

/// Builds a cc::result error from a failed Metal call that reported an NS::Error, recording the call site (not this helper).
/// `error` may be null, which is what a call that failed without saying why hands back.
[[nodiscard]] inline auto metal_error(NS::Error const* error,
                                      cc::string_view what,
                                      cc::source_location site = cc::source_location::current())
{
    return cc::error(describe_error(error, what), site);
}
} // namespace sg::backend::metal
