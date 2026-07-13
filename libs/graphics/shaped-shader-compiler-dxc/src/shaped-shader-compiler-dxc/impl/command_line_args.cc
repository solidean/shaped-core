#include <shaped-shader-compiler-dxc/impl/command_line_args.hh>
#include <shaped-shader-compiler-dxc/impl/dxc_common.hh>

namespace ssc::dxc::impl
{
namespace
{
/// Profile prefix for a stage, or nullptr if we don't emit that stage yet. Compute is wired
/// end-to-end; vertex/fragment map too (their pipelines aren't in sg yet, but the profile is valid).
/// The six ray-tracing stages all target the `lib` profile (a single-entry DXIL library).
[[nodiscard]] char const* stage_prefix(sg::shader_stage s)
{
    switch (s)
    {
    case sg::shader_stage::compute:
        return "cs";
    case sg::shader_stage::vertex:
        return "vs";
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
    // No default: -Wswitch forces a new stage (geometry, mesh, ...) to be handled here rather than
    // silently falling through. A stage we don't emit a profile for yet lands here.
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
