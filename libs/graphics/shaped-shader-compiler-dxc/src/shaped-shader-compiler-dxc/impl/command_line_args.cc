#include <shaped-shader-compiler-dxc/impl/command_line_args.hh>
#include <shaped-shader-compiler-dxc/impl/dxc_common.hh>

namespace ssc::dxc::impl
{
namespace
{
/// Profile prefix for a stage, or nullptr if we do not emit that stage yet.
/// Compute and the raster stages map to their own profile; the six ray-tracing stages all target the `lib` profile, a single-entry DXIL library.
[[nodiscard]] char const* stage_prefix(sg::shader_stage s)
{
    switch (s)
    {
    case sg::shader_stage::compute:
        return "cs";
    case sg::shader_stage::vertex:
        return "vs";
    case sg::shader_stage::tessellation_control:
        return "hs"; // hull
    case sg::shader_stage::tessellation_evaluation:
        return "ds"; // domain
    case sg::shader_stage::geometry:
        return "gs";
    case sg::shader_stage::fragment:
        return "ps";
    // Ray-tracing stages all target a single-entry DXIL library.
    case sg::shader_stage::raygen:
    case sg::shader_stage::closest_hit:
    case sg::shader_stage::any_hit:
    case sg::shader_stage::miss:
    case sg::shader_stage::intersection:
    case sg::shader_stage::callable:
        return "lib";
    }
    // No default: -Wswitch forces a new stage (mesh, amplification, ...) to be handled here rather than silently falling through.
    // A stage we do not emit a profile for yet lands here.
    return nullptr;
}

[[nodiscard]] char const* model_suffix(shader_model m)
{
    switch (m)
    {
    case shader_model::sm_6_0:
        return "6_0";
    case shader_model::sm_6_1:
        return "6_1";
    case shader_model::sm_6_2:
        return "6_2";
    case shader_model::sm_6_3:
        return "6_3";
    case shader_model::sm_6_4:
        return "6_4";
    case shader_model::sm_6_5:
        return "6_5";
    case shader_model::sm_6_6:
        return "6_6";
    case shader_model::sm_6_7:
        return "6_7";
    case shader_model::sm_6_8:
        return "6_8";
    }
    return "6_8";
}

[[nodiscard]] wchar_t const* optimization_flag(optimization_level o)
{
    switch (o)
    {
    case optimization_level::disabled:
        return L"-Od";
    case optimization_level::level_0:
        return L"-O0";
    case optimization_level::level_1:
        return L"-O1";
    case optimization_level::level_2:
        return L"-O2";
    case optimization_level::level_3:
        return L"-O3";
    }
    return L"-O3";
}

/// `-T <prefix>_<model>`. Fails if the stage has no profile prefix.
[[nodiscard]] cc::result<cc::string> target_profile(shader_description const& desc)
{
    char const* prefix = stage_prefix(desc.stage);
    if (prefix == nullptr)
        return cc::error(
            cc::format("shaped-shader-compiler-dxc: shader_stage {} has no DXC profile yet", int(desc.stage)));
    // Ray-tracing (`lib`) targets require shader model 6.3 (DXR 1.0) at minimum.
    if (sg::is_raytracing_stage(desc.stage) && desc.model < shader_model::sm_6_3)
        return cc::error(cc::format("shaped-shader-compiler-dxc: ray-tracing stage {} requires shader model >= 6.3",
                                    int(desc.stage)));
    return cc::string(cc::format("{}_{}", prefix, model_suffix(desc.model)));
}

/// Defines (`-D NAME[=VAL]`) + raw extra args — shared by compile and preprocess.
void append_defines_and_extra(arg_storage& a, compile_options const& opts)
{
    for (auto const& d : opts.defines)
    {
        a.emplace_back(L"-D");
        a.emplace_back(to_wide(d));
    }
    for (auto const& e : opts.extra_args)
        a.emplace_back(to_wide(e));
}
} // namespace

cc::result<arg_storage> build_compile_args(shader_description const& desc, compile_options const& opts)
{
    auto profile = target_profile(desc);
    CC_RETURN_IF_ERROR(profile);

    arg_storage a;
    a.emplace_back(L"-E");
    a.emplace_back(to_wide(desc.entry_point));
    a.emplace_back(L"-T");
    a.emplace_back(to_wide(profile.value()));
    a.emplace_back(optimization_flag(opts.optimization));

    // SPIR-V is one flag plus a target environment.
    // Deliberately no -fvk-*-shift: HLSL's four register classes are mapped by [[vk::binding(N, set)]] annotations in
    // the shader source instead, so the set and binding a module declares are the ones its author wrote.
    // See libs/graphics/shaped-graphics/docs/shaders.md for the authoring rule that implies.
    if (opts.target == compile_target::spirv)
    {
        a.emplace_back(L"-spirv");
        a.emplace_back(L"-fspv-target-env=vulkan1.3");
    }
    if (opts.debug_info)
    {
        a.emplace_back(L"-Zi");
        a.emplace_back(L"-Qembed_debug");
    }
    if (opts.warnings_as_errors)
        a.emplace_back(L"-WX");
    append_defines_and_extra(a, opts);
    return a;
}

cc::result<arg_storage> build_preprocess_args(shader_description const& desc, compile_options const& opts)
{
    auto profile = target_profile(desc);
    CC_RETURN_IF_ERROR(profile);

    arg_storage a;
    a.emplace_back(L"-P"); // preprocess only; DXC emits the flattened source to DXC_OUT_HLSL
    a.emplace_back(L"-T");
    a.emplace_back(to_wide(profile.value()));

    // The target reaches preprocessing because DXC defines `__spirv__` under -spirv, and a shader written for both
    // backends forks on it — a `[[vk::push_constant]]` block has no DXIL spelling, and a `register(b0)` one is not
    // sg's inline constants under Vulkan.
    // slib flattens the source once per target and compiles that, so a preprocess run that did not know the target
    // would resolve the fork the wrong way and the compile would never see the branch it needed.
    if (opts.target == compile_target::spirv)
        a.emplace_back(L"-spirv");

    // Defines steer #if branches during preprocessing, so they belong here; opt/-WX/debug do not.
    append_defines_and_extra(a, opts);
    return a;
}

cc::string join_args(arg_storage const& args)
{
    std::wstring joined;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (i != 0)
            joined += L' ';
        joined += args[i];
    }
    return from_wide(joined.c_str());
}
} // namespace ssc::dxc::impl
