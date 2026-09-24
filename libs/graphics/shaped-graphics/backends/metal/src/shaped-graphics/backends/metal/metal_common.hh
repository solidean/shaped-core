#pragma once

// Single include gate for the metal-cpp headers plus the shared string and error helpers.
// metal TUs include this, not <Metal/Metal.hpp> directly.

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <clean-core/common/utility.hh> // cc::invoke, cc::forward
#include <clean-core/error/result.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/atomic.hh>
#include <shaped-graphics/backends/metal/fwd.hh> // where every backend type is declared, autorelease_scope included
#include <shaped-graphics/fwd.hh>                // also what puts the bare sized aliases in scope inside sg

#include <mutex> // pipeline_compilation_lock, which serializes the MTL4 compiler

/// The newest direct-queue submission that named a resource, or 0 when none has.
///
/// **Two queues, two directions, two stamps.**
/// A command list defers behind the transfers in flight for the resources it touches, which the transfer system's own
/// map answers; this is the other direction, and it is what an off-frame transfer waits on before it copies.
/// Without it an async download of a buffer a list has just filled reads whatever was there before.
///
/// Held by `metal_buffer` and `metal_texture` alike, since the hazard is the resource's, not the kind's.
/// What one resource still owes on each off-frame transfer queue, as the values a waiter must reach.
/// Zero on a side means nothing of that direction is outstanding.
///
/// Two, because uploads and downloads run on queues of their own and one shared event cannot take signals from both:
/// they complete independently, so a later value can land first and drive the event backwards.
struct sg::backend::metal::pending_transfers
{
    u64 upload = 0;
    u64 download = 0;

    [[nodiscard]] bool any() const { return upload > 0 || download > 0; }
};

struct sg::backend::metal::submission_stamp
{
    [[nodiscard]] u64 get() const { return _value.load(cc::memory_order_acquire); }

    /// Raises the stamp to `value`, never lowering it.
    /// Lists on different threads may stamp out of order, and the wait has to cover the newest of them.
    void raise(u64 value)
    {
        auto previous = _value.load(cc::memory_order_relaxed);
        while (previous < value
               && !_value.compare_exchange_weak(previous, value, cc::memory_order_release, cc::memory_order_relaxed))
        {
            // The CAS refreshes `previous` on every failure, so the loop ends as soon as someone stamped higher.
        }
    }

private:
    cc::atomic<u64> _value = {0};
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

/// The process-wide lock every `MTL4Compiler` pipeline build is taken under.
///
/// **Concurrent MTL4 pipeline compilation corrupts a lock inside the driver.**
/// Two threads in `newRenderPipelineState` — on *different* compilers, from different contexts — abort in
/// `_os_unfair_lock_corruption_abort` under `AGXG16GFamilyCompiler`, roughly one run in five on an M4 under macOS 26.
/// It is the driver rather than the validation layer: it reproduces with `MTL_DEBUG_LAYER=0`.
///
/// Process-wide rather than per context, because the state being corrupted is the device's and a Mac hands the same
/// device to every context that asks.
/// The cost is that two threads building pipelines wait for each other, which is the same shape as a shader cache
/// miss and far cheaper than the alternative.
[[nodiscard]] std::mutex& pipeline_compilation_lock();

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

/// The MSL buffer index sg's inline constants arrive at, one past the group a pipeline layout cannot reach.
///
/// Metal has no root constants and no push constants: everything a shader reads is an address in the argument table.
/// So a `set_inline_constants` call stages the block and binds its address here, and MSL declares the block as an
/// ordinary `constant T& name [[buffer(4)]]`.
/// SPIRV-Cross emits a push-constant block as a buffer whose index the caller chooses, so this is that choice.
inline constexpr int k_inline_constants_buffer_index = sg::reserved_binding_group + 1;

/// The MSL buffer index vertex-input slot 0 arrives at; slot `n` is this plus `n`.
///
/// A vertex buffer is bound through the argument table here, not through a `setVertexBuffer` the MTL4 render encoder
/// does not have — so an input slot needs a buffer index of its own, above everything the bind path can name.
/// `MTLVertexBufferLayoutDescriptor` at this index is what the vertex descriptor declares, and the two halves have to
/// agree: the pipeline's layout index and the address the draw binds.
inline constexpr int k_vertex_buffer_base_index = k_inline_constants_buffer_index + 1;

/// The buffer slots one command list's argument table holds: the groups, the reserved one, inline constants, and
/// every vertex-input slot.
///
/// Metal allows 31, so this is sg's budget rather than the API's — and the static_assert below is what says so.
inline constexpr int k_argument_table_buffer_count = k_vertex_buffer_base_index + sg::max_vertex_buffers;

static_assert(k_argument_table_buffer_count <= 31,
              "the metal argument table has 31 buffer slots, and sg's budget "
              "no longer fits");

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

/// Builds the MTL::Library holding `shader`, from either of the two formats metal accepts.
///
/// `metal_lib` is AIR in a container, loaded as it stands.
/// `msl` is source, which the driver compiles here — the arm that exists because producing a metallib needs Apple's
/// separately-installed Metal toolchain while the driver's compiler ships with the OS.
/// `what` names the shader in the error, e.g. "vertex" or "compute_pipeline".
[[nodiscard]] cc::result<MTL::Library*> library_from_shader(MTL::Device* device,
                                                            sg::compiled_shader const& shader,
                                                            cc::string_view what);

/// Builds a cc::result error from a failed Metal call that reported an NS::Error, recording the call site (not this helper).
/// `error` may be null, which is what a call that failed without saying why hands back.
[[nodiscard]] inline auto metal_error(NS::Error const* error,
                                      cc::string_view what,
                                      cc::source_location site = cc::source_location::current())
{
    return cc::error(describe_error(error, what), site);
}
} // namespace sg::backend::metal
