#include "pipeline.hh"

#include <clean-core/common/assertf.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics-language/driver/describe.hh>
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
/// The paths `settings` leaves to the host: those whose last setting is `.host`.
cc::vector<cc::string_view> open_paths_of(cc::span<pipeline_setting const> settings)
{
    auto result = cc::vector<cc::string_view>();
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

/// A string that lives as long as the process, so a setting read from a reloaded source is viewed like a baked one.
/// A reload adds a handful and most repeat, so they are kept rather than freed.
cc::string_view interned(cc::string_view text)
{
    static auto strings = cc::mutex<cc::vector<cc::unique_ptr<cc::string>>>();
    return strings.lock(
        [&](cc::vector<cc::unique_ptr<cc::string>>& all) -> cc::string_view
        {
            for (auto const& s : all)
                if (*s == text)
                    return *s;
            all.push_back(cc::make_unique<cc::string>(text));
            return *all.back();
        });
}

cc::string value_text(pipeline_setting const& s)
{
    switch (s.kind)
    {
    case setting_kind::boolean:
        return s.integer != 0 ? cc::string("true") : cc::string("false");
    case setting_kind::integer:
        return cc::format("{}", s.integer);
    case setting_kind::real:
        return cc::format("{}", s.real);
    case setting_kind::enum_case:
        return cc::format(".{}", s.enum_case);
    case setting_kind::host:
        return cc::string(".host");
    case setting_kind::none:
        return cc::string(".none");
    }
    return {};
}

/// The frozen part as `key = value` lines: what the host's own code is built against, and what a reload may not move.
struct frozen_part
{
    cc::vector<cc::string> keys;
    cc::vector<cc::string> values;

    void add(cc::string key, cc::string value)
    {
        keys.push_back(cc::move(key));
        values.push_back(cc::move(value));
    }
};

template <class Names>
cc::string joined(Names const& names)
{
    auto out = cc::string();
    for (auto const& n : names)
    {
        if (!out.empty())
            out += ", ";
        out += cc::string_view(n);
    }
    return out;
}

frozen_part frozen_of(cc::string joined_layout,
                      cc::string_view inline_constants,
                      cc::string_view vertex_input,
                      cc::string_view target_struct,
                      cc::string joined_targets,
                      cc::span<pipeline_setting const> settings)
{
    auto part = frozen_part();
    part.add("layout", cc::move(joined_layout));
    part.add("inline constants", cc::string(inline_constants));
    part.add("vertex input", cc::string(vertex_input));
    part.add("target set", cc::string(target_struct));
    part.add("targets", cc::move(joined_targets));
    // The formats and the sample count, by their last setting.
    for (auto i = isize(0); i < settings.size(); ++i)
    {
        auto const& s = settings[i];
        auto const is_frozen
            = s.path.ends_with(".format") || s.path == "depth_stencil_format" || s.path == "sample_count";
        auto is_last = true;
        for (auto j = i + 1; j < settings.size(); ++j)
            is_last = is_last && settings[j].path != s.path;
        if (is_frozen && is_last)
            part.add(cc::string(s.path), value_text(s));
    }
    return part;
}

/// What moved from `built` to `now`, one line each; empty where nothing did.
cc::string moved(frozen_part const& built, frozen_part const& now)
{
    auto out = cc::string();
    auto const value_in = [](frozen_part const& p, cc::string_view key) -> cc::string
    {
        for (auto i = isize(0); i < p.keys.size(); ++i)
            if (p.keys[i] == key)
                return p.values[i];
        return cc::string("<unset>");
    };
    for (auto const* side : {&built, &now})
        for (auto const& key : side->keys)
        {
            auto const was = value_in(built, key);
            auto const is = value_in(now, key);
            if (was != is && !out.contains(cc::format("{}:", key)))
                out.appendf("{}: {} -> {}\n", key, was, is);
        }
    return out;
}

/// One declared pipeline's reload state, kept for the life of the process like the definition it belongs to.
struct live_pipeline
{
    slib::pipeline_definition const* definition = nullptr;
    /// The stages' generations the configuration was last read at.
    u64 vertex_generation = 0;
    u64 pixel_generation = 0;
    slib::pipeline_configuration configuration;

    /// The last pipeline that built for a context and its open parts; weak, so the host's holding it is what keeps it.
    struct built
    {
        sg::context const* ctx = nullptr;
        cc::vector<slib::open_part> open;
        std::weak_ptr<sg::raster_pipeline const> pipeline;
    };
    cc::vector<built> last_good;
};

cc::mutex<cc::vector<cc::unique_ptr<live_pipeline>>>& live_pipelines()
{
    static auto all = cc::mutex<cc::vector<cc::unique_ptr<live_pipeline>>>();
    return all;
}

bool same_open(cc::span<slib::open_part const> a, cc::span<slib::open_part const> b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (a[i].path != b[i].path || a[i].value != b[i].value)
            return false;
    return true;
}

sg::raster_pipeline_handle last_good_of(slib::pipeline_definition const* d,
                                        sg::context const* ctx,
                                        cc::span<slib::open_part const> open)
{
    return live_pipelines().lock(
        [&](cc::vector<cc::unique_ptr<live_pipeline>>& all) -> sg::raster_pipeline_handle
        {
            for (auto const& live : all)
                if (live->definition == d)
                    for (auto const& b : live->last_good)
                        if (b.ctx == ctx && same_open(b.open, open))
                            return b.pipeline.lock();
            return nullptr;
        });
}

void remember_good(slib::pipeline_definition const* d,
                   sg::context const* ctx,
                   cc::span<slib::open_part const> open,
                   sg::raster_pipeline_handle const& pipeline)
{
    live_pipelines().lock(
        [&](cc::vector<cc::unique_ptr<live_pipeline>>& all)
        {
            for (auto const& live : all)
                if (live->definition == d)
                {
                    for (auto& b : live->last_good)
                        if (b.ctx == ctx && same_open(b.open, open))
                        {
                            b.pipeline = pipeline;
                            return;
                        }
                    auto kept = cc::vector<slib::open_part>();
                    kept.push_back_range(open);
                    live->last_good.push_back({.ctx = ctx, .open = cc::move(kept), .pipeline = pipeline});
                    return;
                }
        });
}
} // namespace

slib::pipeline_configuration slib::configuration_of(pipeline_definition const& d)
{
    auto const vertex_generation = d.vertex != nullptr && *d.vertex != nullptr ? (*d.vertex)->generation() : 0;
    auto const pixel_generation = d.pixel != nullptr && *d.pixel != nullptr ? (*d.pixel)->generation() : 0;

    // What is known now, and whether a reload moved a stage since it was read.
    auto needs_read = false;
    auto current = live_pipelines().lock(
        [&](cc::vector<cc::unique_ptr<live_pipeline>>& all) -> pipeline_configuration
        {
            for (auto const& live : all)
                if (live->definition == &d)
                {
                    needs_read
                        = live->vertex_generation != vertex_generation || live->pixel_generation != pixel_generation;
                    return live->configuration;
                }
            // The build's own settings, read at the generations the stages have now.
            auto baked = pipeline_configuration();
            baked.settings.push_back_range(d.settings);
            baked.latest.push_back_range(d.settings);
            all.push_back(cc::make_unique<live_pipeline>(live_pipeline{.definition = &d,
                                                                       .vertex_generation = vertex_generation,
                                                                       .pixel_generation = pixel_generation,
                                                                       .configuration = baked}));
            return baked;
        });
    if (!needs_read)
        return current;

    // A stage reloaded, so the source may say something new: described outside the lock, since it checks the whole file.
    auto next = current;
    auto const source = (*d.vertex)->read_source();
    auto const described
        = source.has_value()
            ? sgl::describe({.source = source.value(), .source_name = d.file})
            : cc::result<sgl::module_description, cc::string>(cc::error(cc::string("the source is gone")));
    auto const* found = static_cast<sgl::described_pipeline const*>(nullptr);
    if (described.has_value())
        for (auto const& p : described.value().pipelines)
            if (p.name == d.name)
                found = &p;

    if (found == nullptr)
        CC_LOG_WARNING("{}'s pipeline {} keeps its configuration, since the reloaded source does not state it: {}",
                       d.file, d.name,
                       described.has_value() ? cc::string("no pipeline of that name") : described.error());
    else
    {
        next.latest.clear();
        for (auto const& s : found->settings)
            next.latest.push_back({.path = interned(s.path),
                                   .kind = setting_kind(s.kind),
                                   .integer = s.integer,
                                   .real = s.real,
                                   .enum_case = interned(s.enum_case)});

        auto const built = frozen_of(joined(d.layout), d.inline_constants, d.vertex_input_name, d.target_struct,
                                     joined(d.targets), d.settings);
        auto const now = frozen_of(joined(found->layout), found->inline_constants, found->vertex_input,
                                   found->target_set, joined(found->targets), next.latest);
        next.frozen_moved = moved(built, now);
        if (next.frozen_moved.empty())
            next.settings = next.latest;
        else
            CC_LOG_WARNING("{}'s pipeline {} keeps its last good build: the reloaded source moves what the host was "
                           "built against\n{}",
                           d.file, d.name, next.frozen_moved);
    }

    live_pipelines().lock(
        [&](cc::vector<cc::unique_ptr<live_pipeline>>& all)
        {
            for (auto const& live : all)
                if (live->definition == &d)
                {
                    live->configuration = next;
                    live->vertex_generation = vertex_generation;
                    live->pixel_generation = pixel_generation;
                }
        });
    return next;
}

namespace
{
/// The description a definition states with `settings`, once its stages compiled for `ctx`.
/// `use_latest` builds with the source's newest settings rather than the last ones that match the build.
cc::shared_async<sg::raster_pipeline_description> describe_with(sg::context* ctx,
                                                                slib::pipeline_definition const* definition,
                                                                cc::vector<slib::open_part> open,
                                                                slib::pipeline_customize customize,
                                                                bool use_latest)
{
    auto const& d = *definition;
    CC_ASSERTF(d.vertex != nullptr && *d.vertex != nullptr,
               "{}'s {}: its package was never added to a shader library, so it has no shaders", d.file, d.name);

    // The stages first: awaiting them is what promotes a reload, which the configuration is then read against.
    auto desc = raster_pipeline_description();
    desc.layout = d.acquire_layout(*ctx);
    desc.vertex_shader = co_await (*d.vertex)->acquire(*ctx);
    if (d.pixel != nullptr)
        desc.fragment_shader = co_await (*d.pixel)->acquire(*ctx);
    desc.vertex_input = d.vertex_input();
    desc.target_set = cc::string(d.target_set);
    for (auto i = isize(0); i < d.targets.size(); ++i)
        desc.color_targets.push_back({});

    auto const configuration = slib::configuration_of(d);
    auto const& settings = use_latest ? configuration.latest : configuration.settings;
    auto const applied = slib::apply_settings(desc, settings, d.targets);
    CC_ASSERTF(applied.has_value(), "{}'s {}: {}", d.file, d.name, applied.has_value() ? cc::string() : applied.error());

    // Each open part as the setting it stands for, with the host's value in place of `.host`.
    for (auto const path : open_paths_of(settings))
    {
        auto const* part = static_cast<slib::open_part const*>(nullptr);
        for (auto const& p : open)
            if (p.path == path)
                part = &p;
        CC_ASSERTF(part != nullptr, "{}'s {} leaves {} to the host, and the acquire does not state it", d.file, d.name,
                   path);
        auto setting = pipeline_setting{.path = path, .kind = setting_kind::integer, .integer = part->value};
        if (!path.ends_with("sample_count"))
        {
            auto const names = slib::enum_case_names("pixel_format");
            CC_ASSERTF(part->value > 0 && part->value < names.size(), "{}'s {}: {} is stated as no format", d.file,
                       d.name, path);
            setting = {.path = path, .kind = setting_kind::enum_case, .enum_case = names[part->value]};
        }
        pipeline_setting const one[] = {setting};
        auto const stated = slib::apply_settings(desc, one, d.targets);
        CC_ASSERTF(stated.has_value(), "{}'s {}: {}", d.file, d.name, stated.has_value() ? cc::string() : stated.error());
    }

    if (customize)
        customize(desc);
    co_return desc;
}
} // namespace

cc::shared_async<sg::raster_pipeline_description> slib::describe_raster_pipeline(sg::context* ctx,
                                                                                 pipeline_definition const* definition,
                                                                                 cc::vector<open_part> open,
                                                                                 pipeline_customize customize)
{
    return describe_with(ctx, definition, cc::move(open), cc::move(customize), false);
}

sg::async_raster_pipeline slib::acquire_raster_pipeline(sg::context* ctx,
                                                        pipeline_definition const* definition,
                                                        cc::vector<open_part> open,
                                                        pipeline_customize customize)
{
    auto const desc = co_await describe_with(ctx, definition, open, cc::move(customize), false);

    // A frozen part that moved means the host's own code no longer fits the source, so what built last is kept.
    if (!configuration_of(*definition).frozen_moved.empty())
        if (auto kept = last_good_of(definition, ctx, open); kept != nullptr)
            co_return kept;

    auto const built = ctx->cached.acquire_raster_pipeline(desc);
    co_await cc::async_settled(built);
    if (auto const* const pipeline = built->try_value(); pipeline != nullptr)
    {
        remember_good(definition, ctx, open, *pipeline);
        co_return *pipeline;
    }
    co_return co_await built;
}

sg::async_raster_pipeline slib::acquire_latest_raster_pipeline(sg::context* ctx,
                                                               pipeline_definition const* definition,
                                                               cc::vector<open_part> open,
                                                               pipeline_customize customize)
{
    auto const desc = co_await describe_with(ctx, definition, cc::move(open), cc::move(customize), true);
    co_return co_await ctx->cached.acquire_raster_pipeline(desc);
}
