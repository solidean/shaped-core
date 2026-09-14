#pragma once

// Single include gate for the metal-cpp headers plus the shared string and error helpers.
// metal TUs include this, not <Metal/Metal.hpp> directly.

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <clean-core/common/utility.hh> // cc::invoke, cc::forward
#include <clean-core/error/result.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/backends/metal/fwd.hh> // where every backend type is declared, autorelease_scope included
#include <shaped-graphics/fwd.hh>                // also what puts the bare sized aliases in scope inside sg

#include <mutex>

/// `cc::mutex`'s shape, held by a lock that is real whether or not this build has threads.
///
/// **Metal's completion handlers run on a dispatch queue Apple owns, and `SC_THREADS=OFF` does not reach it.**
/// `cc::mutex` compiles its lock away without threads, which is correct for state only sg's own code touches — and
/// wrong for anything a `MTL4CommitFeedback` handler writes, because the handler is a real thread either way.
/// A singlethreaded build is where that bites first and hardest: the residency set reports it as
/// "residency sets do not support concurrent write operations" and aborts, having lost a lock that was never there.
///
/// So this is for exactly the state a Metal callback mutates, and `cc::mutex` stays right for everything else.
/// See libs/graphics/shaped-graphics/backends/metal/readme.md.
template <class T>
struct sg::backend::metal::callback_mutex
{
    callback_mutex() = default;
    explicit callback_mutex(T value) : _value(cc::move(value)) {}

    template <class F>
    auto lock(F&& f)
    {
        auto const guard = std::lock_guard(_mutex);
        return cc::invoke(cc::forward<F>(f), _value);
    }

private:
    T _value = {};
    std::mutex _mutex;
};

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

/// The resource options every sg buffer is created with.
///
/// **Private storage**, because sg exposes no host-visible buffers at all — a host↔device transfer is a globally
/// managed staging path rather than a mapping on the resource.
/// On unified memory that costs nothing a caller could observe, and it keeps the resource in whatever layout the GPU
/// prefers.
///
/// **Untracked**, which is the Metal 4 half of the decision and the more consequential one.
/// Metal's default is to hazard-track a resource for you and insert the synchronization it infers — which is exactly
/// the work sg's access model already did, from declarations a driver cannot see.
/// Leaving it on would mean paying for both and letting the driver's conservative answer win.
/// It is also what makes a barrier the backend emits meaningful rather than advisory.
inline constexpr MTL::ResourceOptions k_buffer_options
    = MTL::ResourceStorageModePrivate | MTL::ResourceHazardTrackingModeUntracked;

/// Switch Metal's API validation layer on for this process, and make a violation abort rather than log.
///
/// **Call it from `main`, before any Metal call.**
/// The layer is configured entirely through the environment — there is no API for it — and the framework reads those
/// variables when it first initializes, so this works by setting them early rather than by asking Metal anything.
/// Neither variable is overwritten where one is already set, so a developer can pick a different mode from the shell.
///
/// **Why a test binary wants the abort.** Metal delivers a validation message to stderr and to nothing else; there is
/// no callback of the kind the dx12 and vulkan backends install, so a message cannot be attributed to the test that
/// provoked it or turned into a failed CHECK.
/// Left at its default the message is a line in a log nobody reads and the suite stays green, which is precisely how
/// the dx12 backend accumulated roughly 680 unnoticed messages.
/// `assert` mode instead ends the process, so dev.py reports the binary as failed and the log names the last test that
/// ran.
/// Coarser attribution than the other two backends have, and a real gate rather than none.
///
/// Never call this from a shipping application: it is a development gate, and the layer costs real time.
/// See libs/graphics/shaped-graphics/backends/metal/readme.md.
void arm_validation_layer();

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
