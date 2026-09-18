#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-shader-library/fwd.hh>

/// What a WGSL module declares at module scope: its one entry point, and the resources it binds.
///
/// WGSL has no reflection of its own, and a WebGPU bind group layout needs more than HLSL reflection gives — a storage texture's format, each binding's stages.
/// So slib reads the declarations directly, and never needs to look inside a function body.
struct slib::wgsl_declarations
{
    sg::shader_stage stage = sg::shader_stage::compute;
    cc::string entry_point;

    /// `@workgroup_size` of a compute entry point, with omitted axes as 1.
    cc::optional<sg::compute_dimensions> workgroup_size;

    /// Every `@group @binding` resource, stamped visible to `stage`.
    ///
    /// **Group 3 is sg's reserved group**, and reads differently from groups 0 to 2:
    ///   - `@group(3) @binding(0) var<uniform>` is the pipeline's inline-constants block.
    ///     It is reported the way SPIR-V reports a push-constant block: a `uniform_buffer` with no group index.
    ///   - `@group(3) @binding(k)` with k ≥ 1 is a register-bound static sampler, reported as a sampler at group 3 and index k - 1.
    ///     That is the `bound_sampler` binding a WebGPU backend places back at k.
    ///   - anything else there is refused.
    cc::vector<sg::binding> bindings;
};

namespace slib
{
/// Reads a WGSL module's declarations.
///
/// **A module has exactly one entry point** (`@vertex`, `@fragment` or `@compute`), and a second one is refused.
/// A declaration parser cannot see which entry point reads which binding, and WebGPU refuses a binding marked visible to a stage its limits do not allow.
/// So a raster pipeline is a vertex module and a fragment module, whose bindings `sg::merge_bindings` unions.
///
/// Refused rather than reported as something close: binding arrays (WebGPU core has none), `texture_external`, an `override`-sized workgroup, and a storage texture format sg has no pixel format for.
/// A sampled float texture is reported `filterable_float`, since WGSL cannot say otherwise; a caller binding an unfilterable format changes it.
/// A multisampled one is the exception, reported `unfilterable_float` because WebGPU never filters it.
/// Every `sampler` is reported `filtering` for the same reason, and a caller pairing one with a depth or unfilterable texture changes it to `non_filtering`.
/// Module-scope declarations may come in any order, so a const may be declared below the count or attribute that names it.
[[nodiscard]] cc::result<wgsl_declarations> parse_wgsl_declarations(cc::string_view source);
} // namespace slib
