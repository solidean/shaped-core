#include "pipeline.hh"

#include <clean-core/common/assertf.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-shader-library/shader_asset.hh>

using namespace cc::primitive_defines;

namespace
{
using sg::raster_pipeline_description;
using slib::pipeline_setting;
using slib::setting_kind;

// ---- sg's enums, by the names the prelude mirrors them with ---------------------------------------------------------
//
// In enumerator order, so a name's position is its enumerator's value; each array ends at the enum's last enumerator,
// which the static_assert under it pins, so an enumerator added to sg fails here rather than going unmapped.

constexpr cc::string_view k_primitive_topology[]
    = {"point_list", "line_list", "line_strip", "triangle_list", "triangle_strip", "patch_list"};
static_assert(int(sg::primitive_topology::patch_list) + 1 == sizeof(k_primitive_topology) / sizeof(cc::string_view));

constexpr cc::string_view k_fill_mode[] = {"solid", "wireframe"};
static_assert(int(sg::fill_mode::wireframe) + 1 == sizeof(k_fill_mode) / sizeof(cc::string_view));

constexpr cc::string_view k_cull_mode[] = {"none", "front", "back"};
static_assert(int(sg::cull_mode::back) + 1 == sizeof(k_cull_mode) / sizeof(cc::string_view));

constexpr cc::string_view k_front_face[] = {"counter_clockwise", "clockwise"};
static_assert(int(sg::front_face::clockwise) + 1 == sizeof(k_front_face) / sizeof(cc::string_view));

constexpr cc::string_view k_compare_op[]
    = {"never", "less", "equal", "less_equal", "greater", "not_equal", "greater_equal", "always"};
static_assert(int(sg::compare_op::always) + 1 == sizeof(k_compare_op) / sizeof(cc::string_view));

constexpr cc::string_view k_stencil_op[]
    = {"keep", "zero", "replace", "increment_clamp", "decrement_clamp", "invert", "increment_wrap", "decrement_wrap"};
static_assert(int(sg::stencil_op::decrement_wrap) + 1 == sizeof(k_stencil_op) / sizeof(cc::string_view));

constexpr cc::string_view k_blend_factor[] = {"zero",      "one",
                                              "src_color", "one_minus_src_color",
                                              "dst_color", "one_minus_dst_color",
                                              "src_alpha", "one_minus_src_alpha",
                                              "dst_alpha", "one_minus_dst_alpha"};
static_assert(int(sg::blend_factor::one_minus_dst_alpha) + 1 == sizeof(k_blend_factor) / sizeof(cc::string_view));

constexpr cc::string_view k_blend_op[] = {"add", "subtract", "reverse_subtract", "min", "max"};
static_assert(int(sg::blend_op::max) + 1 == sizeof(k_blend_op) / sizeof(cc::string_view));

constexpr cc::string_view k_pixel_format[] = {
    "undefined",
    "r8_unorm",
    "r8_snorm",
    "r8_uint",
    "r8_sint",
    "rg8_unorm",
    "rg8_snorm",
    "rg8_uint",
    "rg8_sint",
    "rgba8_unorm",
    "rgba8_snorm",
    "rgba8_uint",
    "rgba8_sint",
    "rgba8_unorm_srgb",
    "bgra8_unorm",
    "bgra8_unorm_srgb",
    "r16_float",
    "r16_uint",
    "r16_sint",
    "rg16_float",
    "rg16_uint",
    "rg16_sint",
    "rgba16_float",
    "rgba16_uint",
    "rgba16_sint",
    "r32_float",
    "r32_uint",
    "r32_sint",
    "rg32_float",
    "rg32_uint",
    "rg32_sint",
    "rgba32_float",
    "rgba32_uint",
    "rgba32_sint",
    "rgb10a2_unorm",
    "rg11b10_float",
    "depth16_unorm",
    "depth32_float",
    "depth32_float_stencil8",
    "bc1_rgba_unorm",
    "bc1_rgba_unorm_srgb",
    "bc2_unorm",
    "bc2_unorm_srgb",
    "bc3_unorm",
    "bc3_unorm_srgb",
    "bc4_r_unorm",
    "bc4_r_snorm",
    "bc5_rg_unorm",
    "bc5_rg_snorm",
    "bc6h_rgb_ufloat",
    "bc6h_rgb_sfloat",
    "bc7_rgba_unorm",
    "bc7_rgba_unorm_srgb",
};
static_assert(int(sg::pixel_format::bc7_rgba_unorm_srgb) + 1 == sizeof(k_pixel_format) / sizeof(cc::string_view));

template <class E, isize N>
bool set_enum(E& field, cc::string_view const (&names)[N], pipeline_setting const& s)
{
    if (s.kind != setting_kind::enum_case)
        return false;
    for (auto i = isize(0); i < N; ++i)
        if (names[i] == s.enum_case)
        {
            field = E(i);
            return true;
        }
    return false;
}

bool set_bool(bool& field, pipeline_setting const& s)
{
    if (s.kind != setting_kind::boolean)
        return false;
    field = s.integer != 0;
    return true;
}

bool set_float(float& field, pipeline_setting const& s)
{
    if (s.kind != setting_kind::real && s.kind != setting_kind::integer)
        return false;
    field = s.kind == setting_kind::real ? float(s.real) : float(s.integer);
    return true;
}

template <class I>
bool set_int(I& field, pipeline_setting const& s)
{
    if (s.kind != setting_kind::integer)
        return false;
    field = I(s.integer);
    return true;
}

/// The blend of a target, switched on with sg's defaults the first time one of its fields is written.
sg::blend_state& engaged(sg::color_target_state& target)
{
    if (!target.blend.has_value())
        target.blend = sg::blend_state{};
    return target.blend.value();
}

bool set_channel(sg::color_target_state& target, sg::color_channel channel, pipeline_setting const& s)
{
    if (s.kind != setting_kind::boolean)
        return false;
    target.write_mask.set(channel, s.integer != 0);
    return true;
}

/// Writes one setting; `target` is the target a `color_targets.*` path names, and null for every other path.
using setter = bool (*)(raster_pipeline_description& d, sg::color_target_state* target, pipeline_setting const& s);

struct settable
{
    cc::string_view path;
    setter set;
};

/// Every leaf of the prelude's `raster_pipeline_description`, and `color_targets.*.blend` for `.none`.
// clang-format off
constexpr settable k_settable[] = {
    {"topology", [](auto& d, auto*, auto const& s) { return set_enum(d.topology, k_primitive_topology, s); }},
    {"patch_control_points", [](auto& d, auto*, auto const& s) { return set_int(d.patch_control_points, s); }},
    {"rasterization.fill", [](auto& d, auto*, auto const& s) { return set_enum(d.rasterization.fill, k_fill_mode, s); }},
    {"rasterization.cull", [](auto& d, auto*, auto const& s) { return set_enum(d.rasterization.cull, k_cull_mode, s); }},
    {"rasterization.front", [](auto& d, auto*, auto const& s) { return set_enum(d.rasterization.front, k_front_face, s); }},
    {"rasterization.depth_clip_enabled", [](auto& d, auto*, auto const& s) { return set_bool(d.rasterization.depth_clip_enabled, s); }},
    {"rasterization.depth_bias", [](auto& d, auto*, auto const& s) { return set_float(d.rasterization.depth_bias, s); }},
    {"rasterization.depth_bias_slope", [](auto& d, auto*, auto const& s) { return set_float(d.rasterization.depth_bias_slope, s); }},
    {"rasterization.depth_bias_clamp", [](auto& d, auto*, auto const& s) { return set_float(d.rasterization.depth_bias_clamp, s); }},
    {"depth_stencil.depth_test", [](auto& d, auto*, auto const& s) { return set_bool(d.depth_stencil.depth_test, s); }},
    {"depth_stencil.depth_write", [](auto& d, auto*, auto const& s) { return set_bool(d.depth_stencil.depth_write, s); }},
    {"depth_stencil.depth_compare", [](auto& d, auto*, auto const& s) { return set_enum(d.depth_stencil.depth_compare, k_compare_op, s); }},
    {"depth_stencil.stencil_test", [](auto& d, auto*, auto const& s) { return set_bool(d.depth_stencil.stencil_test, s); }},
    {"depth_stencil.stencil_read_mask", [](auto& d, auto*, auto const& s) { return set_int(d.depth_stencil.stencil_read_mask, s); }},
    {"depth_stencil.stencil_write_mask", [](auto& d, auto*, auto const& s) { return set_int(d.depth_stencil.stencil_write_mask, s); }},
    {"depth_stencil.stencil_front.fail", [](auto& d, auto*, auto const& s) { return set_enum(d.depth_stencil.stencil_front.fail, k_stencil_op, s); }},
    {"depth_stencil.stencil_front.depth_fail", [](auto& d, auto*, auto const& s) { return set_enum(d.depth_stencil.stencil_front.depth_fail, k_stencil_op, s); }},
    {"depth_stencil.stencil_front.pass", [](auto& d, auto*, auto const& s) { return set_enum(d.depth_stencil.stencil_front.pass, k_stencil_op, s); }},
    {"depth_stencil.stencil_front.compare", [](auto& d, auto*, auto const& s) { return set_enum(d.depth_stencil.stencil_front.compare, k_compare_op, s); }},
    {"depth_stencil.stencil_back.fail", [](auto& d, auto*, auto const& s) { return set_enum(d.depth_stencil.stencil_back.fail, k_stencil_op, s); }},
    {"depth_stencil.stencil_back.depth_fail", [](auto& d, auto*, auto const& s) { return set_enum(d.depth_stencil.stencil_back.depth_fail, k_stencil_op, s); }},
    {"depth_stencil.stencil_back.pass", [](auto& d, auto*, auto const& s) { return set_enum(d.depth_stencil.stencil_back.pass, k_stencil_op, s); }},
    {"depth_stencil.stencil_back.compare", [](auto& d, auto*, auto const& s) { return set_enum(d.depth_stencil.stencil_back.compare, k_compare_op, s); }},
    {"color_targets.*.format", [](auto&, auto* t, auto const& s) { return set_enum(t->format, k_pixel_format, s); }},
    {"color_targets.*.blend", [](auto&, auto* t, auto const& s) { t->blend = {}; return s.kind == setting_kind::none; }},
    {"color_targets.*.blend.color.source", [](auto&, auto* t, auto const& s) { return set_enum(engaged(*t).color.source, k_blend_factor, s); }},
    {"color_targets.*.blend.color.target", [](auto&, auto* t, auto const& s) { return set_enum(engaged(*t).color.target, k_blend_factor, s); }},
    {"color_targets.*.blend.color.op", [](auto&, auto* t, auto const& s) { return set_enum(engaged(*t).color.op, k_blend_op, s); }},
    {"color_targets.*.blend.alpha.source", [](auto&, auto* t, auto const& s) { return set_enum(engaged(*t).alpha.source, k_blend_factor, s); }},
    {"color_targets.*.blend.alpha.target", [](auto&, auto* t, auto const& s) { return set_enum(engaged(*t).alpha.target, k_blend_factor, s); }},
    {"color_targets.*.blend.alpha.op", [](auto&, auto* t, auto const& s) { return set_enum(engaged(*t).alpha.op, k_blend_op, s); }},
    {"color_targets.*.write_mask.r", [](auto&, auto* t, auto const& s) { return set_channel(*t, sg::color_channel::r, s); }},
    {"color_targets.*.write_mask.g", [](auto&, auto* t, auto const& s) { return set_channel(*t, sg::color_channel::g, s); }},
    {"color_targets.*.write_mask.b", [](auto&, auto* t, auto const& s) { return set_channel(*t, sg::color_channel::b, s); }},
    {"color_targets.*.write_mask.a", [](auto&, auto* t, auto const& s) { return set_channel(*t, sg::color_channel::a, s); }},
    {"depth_stencil_format", [](auto& d, auto*, auto const& s) { return set_enum(d.depth_stencil_format, k_pixel_format, s); }},
    {"sample_count", [](auto& d, auto*, auto const& s) { return set_int(d.sample_count, s); }},
};
// clang-format on

constexpr cc::string_view k_settable_paths[] = {
    "topology",
    "patch_control_points",
    "rasterization.fill",
    "rasterization.cull",
    "rasterization.front",
    "rasterization.depth_clip_enabled",
    "rasterization.depth_bias",
    "rasterization.depth_bias_slope",
    "rasterization.depth_bias_clamp",
    "depth_stencil.depth_test",
    "depth_stencil.depth_write",
    "depth_stencil.depth_compare",
    "depth_stencil.stencil_test",
    "depth_stencil.stencil_read_mask",
    "depth_stencil.stencil_write_mask",
    "depth_stencil.stencil_front.fail",
    "depth_stencil.stencil_front.depth_fail",
    "depth_stencil.stencil_front.pass",
    "depth_stencil.stencil_front.compare",
    "depth_stencil.stencil_back.fail",
    "depth_stencil.stencil_back.depth_fail",
    "depth_stencil.stencil_back.pass",
    "depth_stencil.stencil_back.compare",
    "color_targets.*.format",
    "color_targets.*.blend",
    "color_targets.*.blend.color.source",
    "color_targets.*.blend.color.target",
    "color_targets.*.blend.color.op",
    "color_targets.*.blend.alpha.source",
    "color_targets.*.blend.alpha.target",
    "color_targets.*.blend.alpha.op",
    "color_targets.*.write_mask.r",
    "color_targets.*.write_mask.g",
    "color_targets.*.write_mask.b",
    "color_targets.*.write_mask.a",
    "depth_stencil_format",
    "sample_count",
};
static_assert(sizeof(k_settable_paths) / sizeof(cc::string_view) == sizeof(k_settable) / sizeof(settable));

constexpr cc::string_view k_color_targets = "color_targets.";
} // namespace

cc::span<cc::string_view const> slib::settable_paths()
{
    return k_settable_paths;
}

cc::span<cc::string_view const> slib::enum_case_names(cc::string_view sg_enum)
{
    if (sg_enum == "primitive_topology")
        return k_primitive_topology;
    if (sg_enum == "fill_mode")
        return k_fill_mode;
    if (sg_enum == "cull_mode")
        return k_cull_mode;
    if (sg_enum == "front_face")
        return k_front_face;
    if (sg_enum == "compare_op")
        return k_compare_op;
    if (sg_enum == "stencil_op")
        return k_stencil_op;
    if (sg_enum == "blend_factor")
        return k_blend_factor;
    if (sg_enum == "blend_op")
        return k_blend_op;
    if (sg_enum == "pixel_format")
        return k_pixel_format;
    return {};
}

cc::result<cc::unit, cc::string> slib::apply_settings(sg::raster_pipeline_description& desc,
                                                      cc::span<pipeline_setting const> settings,
                                                      cc::span<cc::string_view const> targets)
{
    for (auto const& s : settings)
    {
        // What the host states arrives as an open part, after the settings.
        if (s.kind == setting_kind::host)
            continue;

        // `color_targets.<target>.rest` is found as `color_targets.*.rest`, on that target.
        auto key = cc::string(s.path);
        auto* target = static_cast<sg::color_target_state*>(nullptr);
        if (s.path.starts_with(k_color_targets))
        {
            auto const rest
                = s.path.subview({.offset = k_color_targets.size(), .size = s.path.size() - k_color_targets.size()});
            auto const dot = rest.find('.');
            auto const name = dot < 0 ? rest : rest.subview({.offset = 0, .size = dot});
            auto index = isize(-1);
            for (auto i = isize(0); i < targets.size(); ++i)
                if (targets[i] == name)
                    index = i;
            if (index < 0 || index >= desc.color_targets.size())
                return cc::error(cc::format("{}: the pipeline has no target {}", s.path, name));
            target = &desc.color_targets[index];
            key = cc::format("{}*{}", k_color_targets,
                             dot < 0 ? cc::string_view() : rest.subview({.offset = dot, .size = rest.size() - dot}));
        }

        auto is_set = false;
        auto is_known = false;
        for (auto const& entry : k_settable)
            if (entry.path == key)
            {
                is_known = true;
                is_set = entry.set(desc, target, s);
            }
        if (!is_known)
            return cc::error(cc::format("{}: sg's raster_pipeline_description has no such field", s.path));
        if (!is_set)
            return cc::error(cc::format("{}: the field cannot take this value", s.path));
    }
    return cc::unit{};
}

namespace
{
/// The paths a definition leaves to the host: those whose last setting is `.host`.
cc::vector<cc::string_view> open_paths_of(slib::pipeline_definition const& d)
{
    auto result = cc::vector<cc::string_view>();
    auto const settings = d.settings;
    for (auto i = isize(0); i < settings.size(); ++i)
    {
        auto is_last = true;
        for (auto j = i + 1; j < settings.size(); ++j)
            is_last = is_last && settings[j].path != settings[i].path;
        if (is_last && settings[i].kind == setting_kind::host)
            result.push_back(settings[i].path);
    }
    return result;
}
} // namespace

cc::shared_async<sg::raster_pipeline_description> slib::describe_raster_pipeline(sg::context* ctx,
                                                                                 pipeline_definition const* definition,
                                                                                 cc::vector<open_part> open,
                                                                                 pipeline_customize customize)
{
    auto const& d = *definition;
    CC_ASSERTF(d.vertex != nullptr && *d.vertex != nullptr,
               "{}'s {}: its package was never added to a shader library, so it has no shaders", d.file, d.name);

    auto desc = raster_pipeline_description();
    desc.layout = d.acquire_layout(*ctx);
    desc.vertex_shader = co_await (*d.vertex)->acquire(*ctx);
    if (d.pixel != nullptr)
        desc.fragment_shader = co_await (*d.pixel)->acquire(*ctx);
    desc.vertex_input = d.vertex_input();
    desc.target_set = cc::string(d.target_set);
    for (auto i = isize(0); i < d.targets.size(); ++i)
        desc.color_targets.push_back({});

    auto const applied = apply_settings(desc, d.settings, d.targets);
    CC_ASSERTF(applied.has_value(), "{}'s {}: {}", d.file, d.name, applied.has_value() ? cc::string() : applied.error());

    // Each open part as the setting it stands for, with the host's value in place of `.host`.
    for (auto const path : open_paths_of(d))
    {
        auto const* part = static_cast<open_part const*>(nullptr);
        for (auto const& p : open)
            if (p.path == path)
                part = &p;
        CC_ASSERTF(part != nullptr, "{}'s {} leaves {} to the host, and the acquire does not state it", d.file, d.name,
                   path);
        auto setting = pipeline_setting{.path = path, .kind = setting_kind::integer, .integer = part->value};
        if (!path.ends_with("sample_count"))
        {
            auto const names = enum_case_names("pixel_format");
            CC_ASSERTF(part->value > 0 && part->value < names.size(), "{}'s {}: {} is stated as no format", d.file,
                       d.name, path);
            setting = {.path = path, .kind = setting_kind::enum_case, .enum_case = names[part->value]};
        }
        pipeline_setting const one[] = {setting};
        auto const stated = apply_settings(desc, one, d.targets);
        CC_ASSERTF(stated.has_value(), "{}'s {}: {}", d.file, d.name, stated.has_value() ? cc::string() : stated.error());
    }

    if (customize)
        customize(desc);
    co_return desc;
}

sg::async_raster_pipeline slib::acquire_raster_pipeline(sg::context* ctx,
                                                        pipeline_definition const* definition,
                                                        cc::vector<open_part> open,
                                                        pipeline_customize customize)
{
    auto const desc = co_await describe_raster_pipeline(ctx, definition, cc::move(open), cc::move(customize));
    co_return co_await ctx->cached.acquire_raster_pipeline(desc);
}
