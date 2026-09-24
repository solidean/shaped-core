#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/check/resources.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// The texture, image and sampler types of a binding (the spec's bindings file).

namespace
{
/// The element types a texture may sample to: a float, an int or a uint, one to four wide.
bool is_sample_element(cc::string_view name)
{
    cc::string_view const stems[] = {"float", "int", "uint"};
    for (auto const stem : stems)
    {
        if (!name.starts_with(stem))
            continue;
        auto const rest = name.subview({.start = stem.size(), .end = name.size()});
        if (rest.empty() || rest == "2" || rest == "3" || rest == "4")
            return true;
    }
    return false;
}

cc::string_view access_prefix(image_access access)
{
    switch (access)
    {
    case image_access::read:
        return "";
    case image_access::read_write:
        return "mut ";
    case image_access::write:
        return "out ";
    }
    return "";
}

cc::string spelling_of(type_info const& t, checked_module const& m)
{
    auto const& shape = info_of(t.shape);
    switch (t.kind)
    {
    case type_kind::texture:
        if (t.is_depth)
            return cc::string(shape.depth);
        // A builtin's bare pattern takes every texture of the shape (CHK-189).
        return t.element == type_id::none ? cc::string(shape.texture)
                                          : cc::format("{}[{}]", shape.texture, m.name_of(t.element));
    case type_kind::image:
        // A builtin's pattern names the texel it reads or writes where an image names its format (CHK-186),
        // and a bare one takes every image of the shape (CHK-189).
        if (t.format < 0 && t.element == type_id::none)
            return cc::string(shape.image);
        return t.format < 0
                 ? cc::format("{}{}[{}]", access_prefix(t.access), shape.image, m.name_of(t.element))
                 : cc::format("{}{}[.{}]", access_prefix(t.access), shape.image, k_storage_formats[t.format].name);
    case type_kind::sampler:
        return t.is_comparison ? cc::string("comparison_sampler") : cc::string("sampler");
    default:
        return {};
    }
}
} // namespace

type_id checker::resource_type(type_info info)
{
    info.spelled = spelling_of(info, out);
    // Interned, as a buffer is: two mentions of `texture2d[float4]` are one type.
    for (auto i = isize(0); i < out.types.size(); ++i)
        if (out.types[i] == info)
            return type_id(i);
    auto const id = type_id(out.types.size());
    out.types.push_back(cc::move(info));
    return id;
}

void checker::judge_feature(i32 file, source_span where, cc::string_view form, cc::string_view feature)
{
    report(diagnostic_kind::needs_feature, file, where,
           cc::format("{} needs {}, which a function cannot opt into yet", form, feature));
}

type_id checker::resolve_resource_name(i32 file, ast::expr_id expr, cc::string_view text)
{
    if (text == "sampler" || text == "comparison_sampler")
        return resource_type({.kind = type_kind::sampler, .is_comparison = text == "comparison_sampler"});

    for (auto const& shape : k_shapes)
    {
        if (shape.depth.empty() || shape.depth != text)
            continue;
        return resource_type({.kind = type_kind::texture, .shape = shape.shape, .is_depth = true});
    }

    // A texture or an image named without its argument is still one, and says what it is missing.
    for (auto const& shape : k_shapes)
        if (shape.texture == text || (!shape.image.empty() && shape.image == text))
        {
            report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, expr),
                   shape.texture == text ? cc::format("a texture takes what it samples to: `{}[float4]`", text)
                                         : cc::format("an image takes its format: `{}[.rgba8_unorm]`", text));
            return checked_module::error_type;
        }
    return type_id::none;
}

type_id checker::resolve_resource_applied(i32 file, ast::expr_id expr, ast::index const& node)
{
    if (!ast::is_valid(node.object))
        return type_id::none;
    auto const* const n = ast_of(file).at(node.object).node.try_as<ast::name>();
    if (n == nullptr)
        return type_id::none;
    auto const text = text_of(file, n->where);

    shape_info const* texture = nullptr;
    shape_info const* image = nullptr;
    for (auto const& shape : k_shapes)
    {
        if (shape.texture == text)
            texture = &shape;
        if (!shape.image.empty() && shape.image == text)
            image = &shape;
    }
    if (texture == nullptr && image == nullptr)
        return type_id::none;

    auto const where = span_of(file, expr);
    auto const arguments = ast_of(file).at(node.arguments);
    if (arguments.size() != 1 || !arguments[0].name.empty() || arguments[0].is_splat)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, where,
               texture != nullptr ? cc::format("a texture takes one element type: `{}[float4]`", text)
                                  : cc::format("an image takes one format: `{}[.rgba8_unorm]`", text));
        return checked_module::error_type;
    }

    if (texture != nullptr)
    {
        auto const element = resolve_type(file, arguments[0].value);
        if (element == checked_module::error_type)
            return checked_module::error_type;
        if (!is_sample_element(out.name_of(element)))
        {
            report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, arguments[0].value),
                   cc::format("a texture samples to a float, an int or a uint, one to four wide, and not to {}",
                              out.name_of(element)));
            return checked_module::error_type;
        }
        if (!texture->feature.empty())
            judge_feature(file, where, text, texture->feature);
        return resource_type({.kind = type_kind::texture, .element = element, .shape = texture->shape});
    }

    // CHK-179 (temporary): the argument is read as exactly an enum case of sg's formats.
    // Values as type arguments in general are in libs/graphics/shaped-graphics-language/docs/TODO.md.
    auto const* const dot = ast_of(file).at(arguments[0].value).node.try_as<ast::leading_dot>();
    auto const format = dot != nullptr ? find_storage_format(text_of(file, dot->name)) : -1;
    if (format < 0)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, arguments[0].value),
               "an image takes one of sg's storage formats as an enum case: `.rgba8_unorm`");
        return checked_module::error_type;
    }
    if (!k_storage_formats[format].is_portable)
        judge_feature(file, where, cc::format("an image of .{}", k_storage_formats[format].name),
                      "sg::feature::extended_storage_formats");
    return resource_type({.kind = type_kind::image, .shape = image->shape, .format = format});
}

type_id checker::qualify_resource(i32 file, ast::expr_id expr, type_id inner, ast::type_access access)
{
    auto const where = span_of(file, expr);
    auto const& t = out.at(inner);
    auto const is_write_only = access == ast::type_access::write_only;

    if (t.kind == type_kind::buffer)
    {
        if (is_write_only)
        {
            report(diagnostic_kind::wrong_kind_of_name, file, where,
                   "a buffer is never `out`: no target has a buffer the shader only writes");
            return checked_module::error_type;
        }
        return buffer_type(t.element, true);
    }
    if (t.kind == type_kind::image)
    {
        auto qualified = t;
        qualified.access = is_write_only ? image_access::write : image_access::read_write;
        if (!is_write_only && !k_storage_formats[t.format].is_readwrite_portable)
            judge_feature(file, where, cc::format("a `mut` image of .{}", k_storage_formats[t.format].name),
                          "sg::feature::readwrite_storage_formats");
        return resource_type(cc::move(qualified));
    }
    if (t.kind == type_kind::texture)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, where,
               "a texture is only ever read; a storage texture the shader writes is an image, such as `image2d`");
        return checked_module::error_type;
    }
    report(diagnostic_kind::wrong_kind_of_name, file, where,
           is_write_only ? "only an image may be `out`, and this is no image"
                         : "only a resource may be `mut`, and this is a value");
    return checked_module::error_type;
}

namespace
{
/// The position of `value` in `names`, or -1.
i32 position_in(cc::span<cc::string_view const> names, cc::string_view value)
{
    for (auto i = isize(0); i < names.size(); ++i)
        if (names[i] == value)
            return i32(i);
    return -1;
}
} // namespace

sampler_state checker::compile_sampler(i32 file, ast::sampler_decl const& s)
{
    auto state = sampler_state{};
    auto const& ast = ast_of(file);
    for (auto const& setting : ast.at(s.settings))
    {
        if (setting.name.empty() || !ast::is_valid(setting.value))
            continue;
        auto const key = text_of(file, setting.name);
        auto const where = span_of(file, setting.value);
        auto const& value = ast.at(setting.value);

        auto const* const dot = value.node.try_as<ast::leading_dot>();
        auto const case_name = dot != nullptr ? text_of(file, dot->name) : cc::string_view();
        auto const enum_setting = [&](cc::span<cc::string_view const> names) -> i32
        {
            auto const found = position_in(names, case_name);
            if (found < 0)
            {
                auto expected = cc::string();
                for (auto const n : names)
                    expected.appendf("{}.{}", expected.empty() ? "" : ", ", n);
                report(diagnostic_kind::invalid_attribute_arguments, file, where,
                       cc::format("{} takes one of {}", key, expected));
            }
            return found;
        };
        auto const number_setting = [&]() -> cc::optional<f64>
        {
            auto const* const literal = value.node.try_as<ast::literal>();
            auto const parsed = literal != nullptr ? parse_plain_float(text_of(file, where)) : cc::optional<f64>();
            if (!parsed.has_value())
                report(diagnostic_kind::invalid_attribute_arguments, file, where, cc::format("{} takes a number", key));
            return parsed;
        };

        if (key == "filter" || key == "min_filter" || key == "mag_filter" || key == "mip_filter")
        {
            auto const found = enum_setting(k_sampler_filters);
            if (found < 0)
                continue;
            if (key == "filter" || key == "min_filter")
                state.min_filter = u8(found);
            if (key == "filter" || key == "mag_filter")
                state.mag_filter = u8(found);
            if (key == "filter" || key == "mip_filter")
                state.mip_filter = u8(found);
        }
        else if (key == "address" || key == "address_u" || key == "address_v" || key == "address_w")
        {
            auto const found = enum_setting(k_sampler_addresses);
            if (found < 0)
                continue;
            if (key == "address" || key == "address_u")
                state.address_u = u8(found);
            if (key == "address" || key == "address_v")
                state.address_v = u8(found);
            if (key == "address" || key == "address_w")
                state.address_w = u8(found);
        }
        else if (key == "compare")
            state.compare = enum_setting(k_compare_ops);
        else if (key == "max_anisotropy")
        {
            if (auto const n = number_setting(); n.has_value())
                state.max_anisotropy = i32(n.value());
        }
        else if (key == "min_lod" || key == "max_lod" || key == "mip_lod_bias")
        {
            auto const n = number_setting();
            if (!n.has_value())
                continue;
            auto& field = key == "min_lod" ? state.min_lod : key == "max_lod" ? state.max_lod : state.mip_lod_bias;
            field = f32(n.value());
        }
        else
            report(diagnostic_kind::invalid_attribute_arguments, file, setting.name,
                   cc::format("{} is no sampler setting; the settings are sg::sampler's fields", key));
    }
    return state;
}

cc::string sgl::check::texel_name_of(i32 format)
{
    auto const& f = k_storage_formats[format];
    auto const stem = f.component == value_kind::scalar_float ? "float"
                    : f.component == value_kind::scalar_int   ? "int"
                                                              : "uint";
    return f.channels == 1 ? cc::string(stem) : cc::format("{}{}", stem, f.channels);
}

type_id checker::resolve_pattern_type(i32 file, ast::expr_id expr)
{
    if (!ast::is_valid(expr))
        return checked_module::error_type;
    auto const& e = ast_of(file).at(expr);

    if (auto const* const q = e.node.try_as<ast::qualified_type>())
    {
        auto const inner = resolve_pattern_type(file, q->type);
        if (inner == checked_module::error_type || out.at(inner).kind != type_kind::image || out.at(inner).format >= 0)
            return inner == checked_module::error_type ? inner : qualify_resource(file, expr, inner, q->access);
        auto qualified = out.at(inner);
        qualified.access = q->access == ast::type_access::write_only ? image_access::write : image_access::read_write;
        auto const result = resource_type(cc::move(qualified));
        set_type(file, expr, result);
        return result;
    }

    // CHK-189: a bare shape name takes every texture or image of that shape, whatever it holds and however it is read.
    if (auto const* const bare = e.node.try_as<ast::name>())
        for (auto const& shape : k_shapes)
        {
            auto const text = text_of(file, bare->where);
            auto const is_texture = shape.texture == text;
            if (!is_texture && (shape.image.empty() || shape.image != text))
                continue;
            auto const result
                = resource_type({.kind = is_texture ? type_kind::texture : type_kind::image, .shape = shape.shape});
            set_type(file, expr, result);
            return result;
        }

    auto const* const applied = e.node.try_as<ast::index>();
    auto const* const head = applied != nullptr && ast::is_valid(applied->object)
                               ? ast_of(file).at(applied->object).node.try_as<ast::name>()
                               : nullptr;
    auto const arguments = applied != nullptr ? ast_of(file).at(applied->arguments) : cc::span<ast::argument const>();
    if (head != nullptr && arguments.size() == 1 && !ast_of(file).at(arguments[0].value).node.is<ast::leading_dot>())
        for (auto const& shape : k_shapes)
            if (!shape.image.empty() && shape.image == text_of(file, head->where))
            {
                auto const texel = resolve_type(file, arguments[0].value);
                if (texel == checked_module::error_type)
                    return texel;
                auto const result = resource_type({.kind = type_kind::image, .element = texel, .shape = shape.shape});
                set_type(file, expr, result);
                return result;
            }
    return resolve_type(file, expr);
}

bool checker::takes(type_id parameter, type_id argument) const
{
    if (parameter == argument)
        return true;
    auto const& p = out.at(parameter);
    auto const& a = out.at(argument);
    auto const is_bare = p.element == type_id::none && p.format < 0 && !p.is_depth;
    if (is_bare && p.kind == type_kind::texture)
        return a.kind == type_kind::texture && !a.is_depth && a.shape == p.shape;
    if (is_bare && p.kind == type_kind::image)
        return a.kind == type_kind::image && a.format >= 0 && a.shape == p.shape;
    if (p.kind != type_kind::image || p.format >= 0 || a.kind != type_kind::image || a.format < 0 || p.shape != a.shape)
        return false;
    if (out.name_of(p.element) != texel_name_of(a.format))
        return false;
    // A pattern that reads takes an image the shader may read, one that writes an image it may write.
    switch (p.access)
    {
    case image_access::read:
        return a.access != image_access::write;
    case image_access::write:
        return a.access != image_access::read;
    case image_access::read_write:
        return a.access == image_access::read_write;
    }
    return false;
}
