#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-shader-compiler-msl/fwd.hh>

/// Reading a shader's bindings out of its MSL text.
///
/// A metallib records no reflection a tool can read without a GPU, and Metal's own reflection is a by-product of
/// building a pipeline state — which needs a device, a vertex layout for a raster stage, and is unavailable outright
/// for the `[[visible]]` functions a miss or closest-hit shader compiles to.
/// So the text is what we read, and it is the only source that works for every stage on every host.
///
/// The subset understood is the one our shaders are written in, and anything outside it is an error naming the
/// declaration rather than a guess:
///
///   - an argument buffer is a struct whose members carry `[[id(n)]]`, bound as a whole at `[[buffer(N)]]`;
///     N is the binding group and n is the binding index, which is the layout metal_common.hh fixes
///   - a `constant T&` at `[[buffer(4)]]`, one past `sg::reserved_binding_group`, is the inline-constants block, and it
///     reflects with no group and no space
///   - any other `[[buffer]]`, `[[texture]]` or `[[sampler]]` on the entry point itself is an error, since the backend
///     binds nothing there
///   - `constant T&` and `constant T*` are uniform buffers, `device T*` is a structured buffer, and a `const` pointee
///     makes it readonly
///   - `texture*<...>` is a texture, readwrite when its access is `read_write` or `write`
///   - `sampler` is a sampler, and an acceleration structure is one of the `raytracing::` handles
///   - a member declared `T name[k]` is an array binding of count k, which occupies k consecutive indices
///
/// **Every `device T*` is a structured buffer.** MSL writes a raw byte-addressed buffer the same way it writes a
/// structured one, so the two cannot be told apart from the text, and structured is what our shaders use.
/// A shader needing a raw buffer is a reason to grow this rule, not to guess between them.

namespace ssc::msl::impl
{
/// What the text said about one entry point.
struct reflection
{
    cc::vector<sg::binding> bindings;

    /// From `#pragma sc numthreads x y z` in the lines directly above the entry point's signature, and absent when the
    /// shader does not state one there.
    /// MSL has no `[numthreads]` of its own, so a kernel that depends on its threadgroup shape has to say so here.
    cc::optional<sg::compute_dimensions> workgroup_size;
};

/// Reads `entry_point`'s bindings out of `source`.
///
/// Fails when the text declares no such entry point, when it declares one with a qualifier `stage` does not imply,
/// or when a binding is of a kind sg has no `binding_type` for.
[[nodiscard]] cc::result<reflection> reflect(cc::string_view source, cc::string_view entry_point, sg::shader_stage stage);

/// The MSL keyword an entry point of this stage must be declared with, or an error for a stage Metal has no shape for.
/// Compute and raygen are both `kernel`, since Metal schedules no raygen of its own and the kernel runs the traversal.
/// The other four ray-tracing stages are `[[visible]]` functions, which is what a shader table links.
[[nodiscard]] cc::result<cc::string_view> entry_qualifier_for(sg::shader_stage stage);
} // namespace ssc::msl::impl
