#include "known_issues.hh"

#include <clean-core/common/assertf.hh>
#include <shaped-graphics/context/adapter_info.hh>
#include <shaped-graphics/context/capabilities.hh>
#include <shaped-graphics/context/context.hh>

using namespace cc::primitive_defines;

namespace
{
constexpr u32 vendor_amd = 0x1002;
constexpr u32 vendor_nvidia = 0x10de;
constexpr u32 vendor_intel = 0x8086;

[[nodiscard]] bool is_hardware(sg::context const& ctx, u32 vendor)
{
    auto const& adapter = ctx.metrics.adapter();
    return adapter.vendor_id == vendor && !adapter.is_software;
}

/// Whether a dx12 driver version is at most `last`.
/// Only sound on dx12, whose backend writes `driver_version` as `a.b.c.d` itself; any other spelling reads as at most,
/// so an unexpected one skips rather than runs a test known to fail.
[[nodiscard]] bool dx12_driver_at_most(sg::context const& ctx, u64 const (&last)[4])
{
    u64 parts[4] = {};
    auto part = 0;
    for (auto const c : cc::string_view(ctx.metrics.adapter().driver_version))
    {
        if (c == '.' && ++part == 4)
            return true;
        if (c == '.')
            continue;
        if (c < '0' || c > '9')
            return true;
        parts[part] = parts[part] * 10 + u64(c - '0');
    }
    for (auto i = 0; i < 4; ++i)
        if (parts[i] != last[i])
            return parts[i] < last[i];
    return true;
}

[[nodiscard]] bool is_intel_dx12(sg::context const& ctx)
{
    return ctx.backend() == sg::backend_kind::dx12 && is_hardware(ctx, vendor_intel);
}

constexpr sg::known_issue registry[] = {
    {.id = "intel-d3d12-view-instancing-draws-nothing",
     .symptom = "Intel's dx12 driver draws nothing for a view-instanced draw whose pixel stage ignores the view",
     .affects = is_intel_dx12},
    {.id = "intel-d3d12-hit-object-pipeline-removes-device",
     .symptom = "Intel's dx12 driver removes the device creating a ray-tracing pipeline with hit objects",
     .affects = [](sg::context const& ctx)
     { return is_intel_dx12(ctx) && ctx.supports(sg::feature::shader_execution_reordering); }},
    {.id = "intel-d3d12-cached-pso-draws-nothing",
     .symptom = "Intel's dx12 shader cache restores a raster pipeline as one that draws nothing, up to 32.0.101.9150",
     .affects = [](sg::context const& ctx) { return is_intel_dx12(ctx) && dx12_driver_at_most(ctx, {32, 0, 101, 9150}); }},
    {.id = "nvidia-mesh-groupshared-atomics-lost",
     .symptom = "NVIDIA's driver loses workgroup atomics of some mesh-shader invocations",
     .affects = [](sg::context const& ctx) { return is_hardware(ctx, vendor_nvidia); }},
    {.id = "radv-aco-scheduler-wrong-bc7-indices",
     .symptom = "RADV in Mesa 26.2.0-devel gives a spilling BC7 encoder from DXC's SPIR-V wrong indices",
     .affects =
         [](sg::context const& ctx)
     {
         auto const& adapter = ctx.metrics.adapter();
         return ctx.backend() == sg::backend_kind::vulkan && !ctx.prefers_native_spirv() && is_hardware(ctx, vendor_amd)
             && adapter.driver_name == "radv" && cc::string_view(adapter.driver_info).starts_with("Mesa 26.2.0-devel");
     }},
};
} // namespace

cc::span<sg::known_issue const> sg::known_issues()
{
    return registry;
}

sg::known_issue const* sg::known_issue_affecting(context const& ctx, cc::string_view id)
{
    for (auto const& issue : registry)
        if (issue.id == id)
            return issue.affects(ctx) ? &issue : nullptr;
    CC_ASSERTF(false, "no known issue is named '{}'", id);
    return nullptr;
}
