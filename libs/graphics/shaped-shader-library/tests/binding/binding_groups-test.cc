#include "binding_corpus.hh"

#include <clean-core/common/log.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/to_string.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-shader-library/binding/binding_groups.hh>

using namespace cc::primitive_defines;

namespace
{
struct expected_binding
{
    cc::string_view name;
    u32 index = 0;
    u32 count = 1;
    sg::binding_type type = sg::binding_type::uniform_buffer;
    cc::optional<sg::texture_view_dimension> dimension;
};

struct expected_group
{
    cc::string_view name;
    u32 group = 0;
    cc::vector<expected_binding> bindings;
};

/// One `static <name> <field>=<value>...` line: the sampler's state, rendered canonically.
struct expected_sampler
{
    cc::string_view name;
    cc::string fields; ///< the `key=value` words, space-joined, in the order the line gives them
};

struct corpus_case
{
    cc::string_view name;
    cc::string hlsl;
    cc::vector<expected_group> groups;
    cc::vector<expected_sampler> statics;
    cc::optional<cc::string_view> error; ///< set instead of `groups` when the snippet must be rejected
};

struct name_of_binding_type
{
    cc::string_view name;
    sg::binding_type value;
};

constexpr name_of_binding_type k_binding_types[] = {
    {"uniform_buffer", sg::binding_type::uniform_buffer},
    {"readonly_structured_buffer", sg::binding_type::readonly_structured_buffer},
    {"readwrite_structured_buffer", sg::binding_type::readwrite_structured_buffer},
    {"readonly_raw_buffer", sg::binding_type::readonly_raw_buffer},
    {"readwrite_raw_buffer", sg::binding_type::readwrite_raw_buffer},
    {"readonly_texture", sg::binding_type::readonly_texture},
    {"readwrite_texture", sg::binding_type::readwrite_texture},
    {"sampler", sg::binding_type::sampler},
    {"acceleration_structure", sg::binding_type::acceleration_structure},
};

struct name_of_dimension
{
    cc::string_view name;
    sg::texture_view_dimension value;
};

constexpr name_of_dimension k_dimensions[] = {
    {"tex_1d", sg::texture_view_dimension::tex_1d},
    {"tex_1d_array", sg::texture_view_dimension::tex_1d_array},
    {"tex_2d", sg::texture_view_dimension::tex_2d},
    {"tex_2d_array", sg::texture_view_dimension::tex_2d_array},
    {"tex_2d_ms", sg::texture_view_dimension::tex_2d_ms},
    {"tex_2d_ms_array", sg::texture_view_dimension::tex_2d_ms_array},
    {"tex_3d", sg::texture_view_dimension::tex_3d},
    {"cube", sg::texture_view_dimension::cube},
    {"cube_array", sg::texture_view_dimension::cube_array},
};

[[nodiscard]] cc::string_view name_of(sg::binding_type type)
{
    for (auto const& entry : k_binding_types)
        if (entry.value == type)
            return entry.name;
    return "<unknown>";
}

[[nodiscard]] cc::string_view name_of(cc::optional<sg::texture_view_dimension> dimension)
{
    if (!dimension.has_value())
        return "<none>";
    for (auto const& entry : k_dimensions)
        if (entry.value == dimension.value())
            return entry.name;
    return "<unknown>";
}

[[nodiscard]] cc::string_view name_of(sg::sampler_filter filter)
{
    return filter == sg::sampler_filter::nearest ? "nearest" : "linear";
}

[[nodiscard]] cc::string_view name_of(sg::sampler_address_mode mode)
{
    switch (mode)
    {
    case sg::sampler_address_mode::repeat:
        return "repeat";
    case sg::sampler_address_mode::mirror_repeat:
        return "mirror_repeat";
    case sg::sampler_address_mode::clamp_edge:
        return "clamp_edge";
    case sg::sampler_address_mode::clamp_border:
        return "clamp_border";
    case sg::sampler_address_mode::mirror_clamp_edge:
        return "mirror_clamp_edge";
    }
    return "<unknown>";
}

[[nodiscard]] cc::string_view name_of(sg::sampler_border_color color)
{
    switch (color)
    {
    case sg::sampler_border_color::transparent_black:
        return "transparent_black";
    case sg::sampler_border_color::opaque_black:
        return "opaque_black";
    case sg::sampler_border_color::opaque_white:
        return "opaque_white";
    }
    return "<unknown>";
}

[[nodiscard]] cc::string_view name_of(sg::compare_op op)
{
    switch (op)
    {
    case sg::compare_op::never:
        return "never";
    case sg::compare_op::less:
        return "less";
    case sg::compare_op::equal:
        return "equal";
    case sg::compare_op::less_equal:
        return "less_equal";
    case sg::compare_op::greater:
        return "greater";
    case sg::compare_op::not_equal:
        return "not_equal";
    case sg::compare_op::greater_equal:
        return "greater_equal";
    case sg::compare_op::always:
        return "always";
    }
    return "<unknown>";
}

/// Every field that differs from sg::sampler's default, in sg::sampler's own order.
///
/// One canonical rendering both halves of the corpus compare as text, so neither has to model the other's
/// value types — the Python side renders the same string out of the spellings it read.
[[nodiscard]] cc::string render(sg::sampler const& s)
{
    auto const defaults = sg::sampler();
    cc::string out;

    auto const add = [&](cc::string_view key, cc::string_view value)
    {
        if (!out.empty())
            out += ' ';
        out += cc::format("{}={}", key, value);
    };

    if (s.min_filter != defaults.min_filter)
        add("min_filter", name_of(s.min_filter));
    if (s.mag_filter != defaults.mag_filter)
        add("mag_filter", name_of(s.mag_filter));
    if (s.mip_filter != defaults.mip_filter)
        add("mip_filter", name_of(s.mip_filter));
    if (s.address_u != defaults.address_u)
        add("address_u", name_of(s.address_u));
    if (s.address_v != defaults.address_v)
        add("address_v", name_of(s.address_v));
    if (s.address_w != defaults.address_w)
        add("address_w", name_of(s.address_w));
    if (s.mip_lod_bias != defaults.mip_lod_bias)
        add("mip_lod_bias", cc::to_string(s.mip_lod_bias));
    if (s.max_anisotropy != defaults.max_anisotropy)
        add("max_anisotropy", cc::to_string(s.max_anisotropy));
    if (s.min_lod != defaults.min_lod)
        add("min_lod", cc::to_string(s.min_lod));
    if (s.max_lod != defaults.max_lod)
        add("max_lod", cc::to_string(s.max_lod));
    if (s.compare.has_value())
        add("compare", name_of(s.compare.value()));
    if (s.border_color != defaults.border_color)
        add("border_color", name_of(s.border_color));

    return out;
}

/// Splits `text` at newlines, keeping neither the newline nor a trailing carriage return.
[[nodiscard]] cc::vector<cc::string_view> lines_of(cc::string_view text)
{
    cc::vector<cc::string_view> lines;
    isize start = 0;
    while (start <= text.size())
    {
        auto end = text.find('\n', start);
        if (end < 0)
            end = text.size();

        auto line = text.subview({.start = start, .end = end});
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        lines.push_back(line);

        start = end + 1;
    }
    return lines;
}

/// Splits on whitespace — the corpus' expectation lines are `key=value` words and nothing else.
[[nodiscard]] cc::vector<cc::string_view> words_of(cc::string_view line)
{
    cc::vector<cc::string_view> words;
    isize i = 0;
    while (i < line.size())
    {
        while (i < line.size() && line[i] == ' ')
            ++i;
        auto const start = i;
        while (i < line.size() && line[i] != ' ')
            ++i;
        if (i > start)
            words.push_back(line.subview({.start = start, .end = i}));
    }
    return words;
}

/// The value of `key=value`, or nothing when the word carries another key.
[[nodiscard]] cc::optional<cc::string_view> value_of(cc::string_view word, cc::string_view key)
{
    if (!word.starts_with(key) || word.size() <= key.size() || word[key.size()] != '=')
        return cc::nullopt;
    return word.subview(key.size() + 1);
}

[[nodiscard]] cc::vector<corpus_case> read_corpus(cc::string_view text)
{
    constexpr cc::string_view k_case = "--- case ";
    constexpr cc::string_view k_hlsl = "--- hlsl";
    constexpr cc::string_view k_groups = "--- groups";
    constexpr cc::string_view k_error = "--- error";

    enum class section
    {
        none,
        hlsl,
        groups,
        error
    };

    cc::vector<corpus_case> cases;
    auto mode = section::none;

    for (auto const& line : lines_of(text))
    {
        if (line.starts_with(k_case))
        {
            cases.push_back({.name = line.subview(k_case.size())});
            mode = section::none;
            continue;
        }
        if (line == k_hlsl)
        {
            mode = section::hlsl;
            continue;
        }
        if (line == k_groups)
        {
            mode = section::groups;
            continue;
        }
        if (line == k_error)
        {
            mode = section::error;
            continue;
        }

        if (cases.empty())
            continue;
        auto& current = cases.back();

        switch (mode)
        {
        case section::none:
            break;

        case section::hlsl:
            current.hlsl += line;
            current.hlsl += '\n';
            break;

        case section::groups:
        {
            auto const words = words_of(line);
            if (words.empty() || line.starts_with('#'))
                break;

            if (!line.starts_with("  "))
            {
                auto const number = value_of(words[1], "group");
                REQUIRE(number.has_value());
                current.groups.push_back({.name = words[0], .group = cc::from_string<u32>(number.value()).value()});
                break;
            }

            REQUIRE(!current.groups.empty());
            if (words[0] == "static")
            {
                cc::string fields;
                for (isize w = 2; w < words.size(); ++w)
                {
                    if (!fields.empty())
                        fields += ' ';
                    fields += words[w];
                }
                current.statics.push_back({.name = words[1], .fields = cc::move(fields)});
                break;
            }
            expected_binding binding;
            binding.name = words[0];
            for (isize w = 1; w < words.size(); ++w)
            {
                auto const& word = words[w];
                if (auto const v = value_of(word, "index"); v.has_value())
                    binding.index = cc::from_string<u32>(v.value()).value();
                else if (auto const c = value_of(word, "count"); c.has_value())
                    binding.count = cc::from_string<u32>(c.value()).value();
                else if (auto const t = value_of(word, "type"); t.has_value())
                {
                    for (auto const& entry : k_binding_types)
                        if (entry.name == t.value())
                            binding.type = entry.value;
                }
                else if (auto const d = value_of(word, "dim"); d.has_value())
                {
                    for (auto const& entry : k_dimensions)
                        if (entry.name == d.value())
                            binding.dimension = entry.value;
                }
            }
            current.groups.back().bindings.push_back(binding);
            break;
        }

        case section::error:
            if (!line.empty() && !line.starts_with('#'))
                current.error = line;
            break;
        }
    }

    return cases;
}

/// The message a case pins, against what the parse reported.
/// `to_string()` prefixes the message and appends the site, so the first line is what the case compares.
[[nodiscard]] cc::string first_line_of(cc::string_view rendered)
{
    auto const end = rendered.find('\n');
    return cc::string(end < 0 ? rendered : rendered.subview({.start = 0, .end = end}));
}
} // namespace

TEST("slib - the binding corpus parses as it says it does")
{
    auto const cases = read_corpus(slib_test::binding_corpus_text());

    // The corpus is baked in at configure time, so an empty one means the bake broke rather than that nothing is pinned.
    REQUIRE(cases.size() > 20);

    for (auto const& c : cases)
    {
        auto const parsed = slib::parse_binding_groups(c.hlsl);

        if (c.error.has_value())
        {
            if (parsed.has_value())
            {
                CC_LOG_ERROR("[corpus] '{}' was accepted, but must be rejected with: {}", c.name, c.error.value());
                CHECK(false);
                continue;
            }

            auto const reported = first_line_of(parsed.error().to_string());
            auto const expected = cc::format("error: {}", c.error.value());
            if (reported != expected)
                CC_LOG_ERROR("[corpus] '{}' reported\n  {}\nbut must report\n  {}", c.name, reported, expected);
            CHECK(reported == expected);
            continue;
        }

        if (parsed.has_error())
        {
            CC_LOG_ERROR("[corpus] '{}' was rejected: {}", c.name, first_line_of(parsed.error().to_string()));
            CHECK(false);
            continue;
        }

        auto const& groups = parsed.value();
        if (groups.size() != c.groups.size())
        {
            CC_LOG_ERROR("[corpus] '{}' found {} group(s), expected {}", c.name, groups.size(), c.groups.size());
            CHECK(false);
            continue;
        }

        for (isize g = 0; g < groups.size(); ++g)
        {
            auto const& group = groups[g];
            auto const& expected = c.groups[g];

            CHECK(group.name == expected.name);
            CHECK(group.group == expected.group);

            if (group.bindings.size() != expected.bindings.size())
            {
                CC_LOG_ERROR("[corpus] '{}' group '{}' has {} binding(s), expected {}", c.name, group.name,
                             group.bindings.size(), expected.bindings.size());
                CHECK(false);
                continue;
            }

            for (isize b = 0; b < group.bindings.size(); ++b)
            {
                auto const& binding = group.bindings[b];
                auto const& want = expected.bindings[b];

                if (binding.name != want.name || binding.index != want.index || binding.count != want.count
                    || binding.type != want.type || binding.texture_dimension != want.dimension)
                    CC_LOG_ERROR("[corpus] '{}' binding {} is '{}' index={} count={} type={} dim={}, expected '{}' "
                                 "index={} count={} type={} dim={}",
                                 c.name, b, binding.name, binding.index, binding.count, name_of(binding.type),
                                 name_of(binding.texture_dimension), want.name, want.index, want.count,
                                 name_of(want.type), name_of(want.dimension));

                CHECK(binding.name == want.name);
                CHECK(binding.index == want.index);
                CHECK(binding.count == want.count);
                CHECK(binding.type == want.type);
                CHECK(binding.texture_dimension == want.dimension);

                // The group number is both the SPIR-V set and the HLSL space, so every binding carries it twice.
                REQUIRE(binding.group_index.has_value());
                REQUIRE(binding.space.has_value());
                CHECK(binding.group_index.value() == expected.group);
                CHECK(binding.space.value() == expected.group);
            }
        }

        // The `static` lines: which samplers the layout bakes in, and the state each declared.
        isize declared_count = 0;
        for (auto const& group : groups)
            declared_count += group.static_samplers.size();

        if (declared_count != c.statics.size())
        {
            CC_LOG_ERROR("[corpus] '{}' declares {} static sampler(s), expected {}", c.name, declared_count,
                         c.statics.size());
            CHECK(false);
            continue;
        }

        for (auto const& want : c.statics)
        {
            slib::declared_sampler const* found = nullptr;
            for (auto const& group : groups)
                for (auto const& declared : group.static_samplers)
                    if (declared.name == want.name)
                        found = &declared;

            if (found == nullptr)
            {
                CC_LOG_ERROR("[corpus] '{}' does not declare '{}' static", c.name, want.name);
                CHECK(false);
                continue;
            }

            auto const rendered = render(found->sampler);
            if (rendered != want.fields)
                CC_LOG_ERROR("[corpus] '{}' static '{}' is [{}], expected [{}]", c.name, want.name, rendered,
                             want.fields);
            CHECK(rendered == want.fields);
        }
    }
}

TEST("slib - an empty source declares no binding group")
{
    auto const groups = slib::parse_binding_groups("");
    REQUIRE(groups.has_value());
    CHECK(groups.value().empty());
}

namespace
{
constexpr char const* k_two_stage_shader = R"(
#pragma sc group 0
namespace frame_bindings
{
    Texture2D<float4> pixel_only;
    Texture2D<float4> both;
    SamplerState samp;
}
)";
} // namespace

TEST("slib - the DXIL arm writes register() after the declared name")
{
    auto const rewritten = slib::rewrite_binding_groups(k_two_stage_shader, sg::shader_format::dxil);
    REQUIRE(rewritten.has_value());

    // One counter across register classes, so the sampler takes s2 rather than s0.
    CHECK(rewritten.value().contains("Texture2D<float4> pixel_only : register(t0, space0);"));
    CHECK(rewritten.value().contains("Texture2D<float4> both : register(t1, space0);"));
    CHECK(rewritten.value().contains("SamplerState samp : register(s2, space0);"));

    // And the directive is gone: DXC ignores a pragma it does not know today, but -Wall would make it an error.
    CHECK(!rewritten.value().contains("#pragma sc"));
}

TEST("slib - the SPIR-V arm writes the attribute before the declaration")
{
    auto const rewritten = slib::rewrite_binding_groups(k_two_stage_shader, sg::shader_format::spirv);
    REQUIRE(rewritten.has_value());

    CHECK(rewritten.value().contains("[[vk::binding(0, 0)]] Texture2D<float4> pixel_only;"));
    CHECK(rewritten.value().contains("[[vk::binding(1, 0)]] Texture2D<float4> both;"));
    CHECK(rewritten.value().contains("[[vk::binding(2, 0)]] SamplerState samp;"));

    // Nothing DXIL-only leaks into this arm, and nothing Vulkan-only into the other.
    CHECK(!rewritten.value().contains("register("));
}

TEST("slib - a source carrying no attribute comes back byte for byte")
{
    constexpr cc::string_view k_hand_written = R"(
Texture2D<float4> albedo : register(t0, space0);
SamplerState samp : register(s0, space0);

// an ordinary comment mentioning group 0, register(t1) and [[vk::binding]]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {}
)";

    for (auto const target : {sg::shader_format::dxil, sg::shader_format::spirv, sg::shader_format::metal_lib})
    {
        auto const rewritten = slib::rewrite_binding_groups(k_hand_written, target);
        REQUIRE(rewritten.has_value());
        CHECK(rewritten.value() == k_hand_written);
    }
}

TEST("slib - a target with no arm is an error only once a group needs one")
{
    auto const rewritten = slib::rewrite_binding_groups(k_two_stage_shader, sg::shader_format::metal_lib);
    CHECK(rewritten.has_error());
}

namespace
{
constexpr char const* k_misspelled_shader = R"(
#pragma sc gruop 0
namespace frame
{
    Texture2D<float4> albedo;
}
)";
} // namespace

TEST("slib - a rejected source fails the rewrite rather than passing through")
{
    auto const rewritten = slib::rewrite_binding_groups(k_misspelled_shader, sg::shader_format::dxil);
    CHECK(rewritten.has_error());
}
