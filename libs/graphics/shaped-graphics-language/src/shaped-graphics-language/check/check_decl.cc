#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
stage stage_of(bool is_vertex, bool is_pixel, bool is_compute)
{
    if (is_vertex)
        return stage::vertex;
    if (is_pixel)
        return stage::pixel;
    return is_compute ? stage::compute : stage::none;
}
} // namespace

/// True for the prelude's `int3`, which is what a dispatch reports a thread's id as.
bool checker::is_int3(type_id type) const
{
    auto const* const record = out.builtin_type_of(type);
    return record != nullptr && record->name == "int3";
}

/// `@stages(.pixel)` or `@stages(.vertex, .pixel)`: a bad argument reports and leaves every stage.
sgl::u8 checker::stages_of(i32 file, ast::attribute const* a)
{
    if (a == nullptr)
        return k_every_stage;

    // CHK-208: each argument is one stage as an enum case; the function is reached only from an entry point of one.
    auto result = u8(0);
    auto const arguments = ast_of(file).at(a->arguments);
    for (auto const& argument : arguments)
    {
        auto const* const dot
            = ast::is_valid(argument.value) ? ast_of(file).at(argument.value).node.try_as<ast::leading_dot>() : nullptr;
        auto const name = dot != nullptr ? text_of(file, dot->name) : cc::string_view();
        auto const s = name == "vertex"  ? stage::vertex
                     : name == "pixel"   ? stage::pixel
                     : name == "compute" ? stage::compute
                                         : stage::none;
        if (!argument.name.empty() || argument.is_splat || s == stage::none)
        {
            report(diagnostic_kind::invalid_attribute_arguments, file, a->name,
                   "@stages takes the stages a function may be reached from: `@stages(.pixel)`, `@stages(.vertex, "
                   ".pixel)`");
            return k_every_stage;
        }
        result = u8(result | stage_bit(s));
    }
    if (arguments.empty())
    {
        report(diagnostic_kind::invalid_attribute_arguments, file, a->name, "@stages names at least one stage");
        return k_every_stage;
    }
    return result;
}

pixel_output checker::pixel_output_of(i32 file,
                                      ast::range_of<ast::attribute> attributes,
                                      type_id member_type,
                                      cc::string_view name)
{
    auto const* const depth = find_attribute(file, attributes, "depth");
    auto const* const mask = find_attribute(file, attributes, "sample_mask");
    if (depth == nullptr && mask == nullptr)
        return pixel_output::color;
    // CHK-276: the pixel's depth is a float and its sample mask a uint, and a member is one of them at most
    if (depth != nullptr && mask != nullptr)
    {
        report(diagnostic_kind::invalid_attribute_arguments, file, depth->name,
               cc::format("{} is the depth or the sample mask, not both", name));
        return pixel_output::depth;
    }
    if (mask != nullptr)
    {
        if (out.name_of(member_type) != "uint")
            report(diagnostic_kind::type_mismatch, file, mask->name,
                   cc::format("a @sample_mask member is a uint, and {} is a {}", name, out.name_of(member_type)));
        return pixel_output::sample_mask;
    }
    if (out.name_of(member_type) != "float")
        report(diagnostic_kind::type_mismatch, file, depth->name,
               cc::format("a @depth member is a float, and {} is a {}", name, out.name_of(member_type)));
    auto const arguments = ast_of(file).at(depth->arguments);
    if (arguments.empty())
        return pixel_output::depth;
    auto const* const dot = arguments.size() == 1 && ast::is_valid(arguments[0].value) && arguments[0].name.empty()
                              ? ast_of(file).at(arguments[0].value).node.try_as<ast::leading_dot>()
                              : nullptr;
    auto const promise = dot != nullptr ? text_of(file, dot->name) : cc::string_view();
    if (promise == "greater_equal")
        return pixel_output::depth_greater_equal;
    if (promise == "less_equal")
        return pixel_output::depth_less_equal;
    report(diagnostic_kind::invalid_attribute_arguments, file, depth->name,
           "@depth takes nothing, or the direction it only moves in: `@depth(.greater_equal)`, `@depth(.less_equal)`");
    return pixel_output::depth;
}

cc::string checker::vertex_format_of(i32 file, ast::attribute const* a, type_id member_type)
{
    if (a == nullptr)
        return {};

    // What each `sg::vertex_attribute_format` decodes into, so a format and the member reading it agree.
    struct decoded
    {
        cc::string_view format;
        cc::string_view type;
    };
    static constexpr decoded k_formats[] = {
        {"f32", "float"},   {"vec2f", "float2"}, {"vec3f", "float3"},       {"vec4f", "float4"},     {"i32", "int"},
        {"vec2i", "int2"},  {"vec3i", "int3"},   {"vec4i", "int4"},         {"u32", "uint"},         {"vec2u", "uint2"},
        {"vec3u", "uint3"}, {"vec4u", "uint4"},  {"rgba8_unorm", "float4"}, {"rgba8_uint", "uint4"},
    };

    auto const arguments = ast_of(file).at(a->arguments);
    auto const* const dot = arguments.size() == 1 && ast::is_valid(arguments[0].value) && arguments[0].name.empty()
                              ? ast_of(file).at(arguments[0].value).node.try_as<ast::leading_dot>()
                              : nullptr;
    auto const name = dot != nullptr ? text_of(file, dot->name) : cc::string_view();
    for (auto const& f : k_formats)
    {
        if (f.format != name)
            continue;
        if (out.name_of(member_type) != f.type)
            report(diagnostic_kind::type_mismatch, file, a->name,
                   cc::format("@format(.{}) decodes into a {}, and the member is a {}", name, f.type,
                              out.name_of(member_type)));
        return cc::string(name);
    }
    report(diagnostic_kind::invalid_attribute_arguments, file, a->name,
           "@format on a vertex member takes one case of sg::vertex_attribute_format: `@format(.rgba8_unorm)`");
    return {};
}

/// `@interpolate(.flat)` or `@interpolate(.linear, .centroid)`; a bad argument reports and leaves the default.
interpolation checker::interpolation_of(i32 file, ast::attribute const* a)
{
    auto result = interpolation();
    if (a == nullptr)
        return result;

    // CHK-273: a kind, then a sampling that only a kind other than flat takes
    auto const arguments = ast_of(file).at(a->arguments);
    auto const case_at = [&](isize i)
    {
        auto const& argument = arguments[i];
        auto const* const dot
            = ast::is_valid(argument.value) ? ast_of(file).at(argument.value).node.try_as<ast::leading_dot>() : nullptr;
        return dot != nullptr && argument.name.empty() && !argument.is_splat ? text_of(file, dot->name)
                                                                             : cc::string_view();
    };
    auto const kind = arguments.size() >= 1 ? case_at(0) : cc::string_view();
    auto const sampling = arguments.size() >= 2 ? case_at(1) : cc::string_view("center");
    auto const is_kind = kind == "perspective" || kind == "linear" || kind == "flat";
    auto const is_sampling = sampling == "center" || sampling == "centroid" || sampling == "sample";
    if (arguments.empty() || arguments.size() > 2 || !is_kind || !is_sampling
        || (kind == "flat" && arguments.size() == 2))
    {
        report(diagnostic_kind::invalid_attribute_arguments, file, a->name,
               "@interpolate takes a kind - `.perspective`, `.linear` or `.flat` - and, for the first two, a sampling: "
               "`.center`, `.centroid` or `.sample`");
        return result;
    }
    result.kind = kind == "linear" ? interpolation::kind_t::linear
                : kind == "flat"   ? interpolation::kind_t::flat
                                   : interpolation::kind_t::perspective;
    result.sampling = sampling == "centroid" ? interpolation::sampling_t::centroid
                    : sampling == "sample"   ? interpolation::sampling_t::sample
                                             : interpolation::sampling_t::center;
    return result;
}

/// `@compute(64)` or `@compute(8, 8, 1)`: the axes nobody wrote are 1, and a bad argument reports and stays 1.
cc::fixed_array<sgl::i32, 3> checker::workgroup_of(i32 file, ast::attribute const* a)
{
    auto result = cc::fixed_array<i32, 3>{1, 1, 1};
    if (a == nullptr)
        return result;

    auto const arguments = ast_of(file).at(a->arguments);
    if (arguments.empty() || arguments.size() > 3)
    {
        report(diagnostic_kind::invalid_attribute_arguments, file, a->name,
               "@compute takes one to three workgroup sizes: `@compute(64)`, `@compute(8, 8)`");
        return result;
    }
    for (auto i = isize(0); i < arguments.size(); ++i)
    {
        auto const& argument = arguments[i];
        auto const text
            = ast::is_valid(argument.value) ? text_of(file, span_of(file, argument.value)) : cc::string_view();
        auto const value = ast::is_valid(argument.value) && ast_of(file).at(argument.value).node.is<ast::literal>()
                                && classify_number(text) == number_class::plain_integer
                             ? parse_plain_integer(text)
                             : cc::optional<i32>();
        if (!argument.name.empty() || argument.is_splat || !value.has_value() || value.value() < 1)
        {
            report(diagnostic_kind::invalid_attribute_arguments, file, a->name,
                   "a workgroup size is a positive int literal");
            return {1, 1, 1};
        }
        result[i] = value.value();
    }
    return result;
}

/// `@stream(normals)`: one bare name, which is the buffer a vertex input member is read from.
cc::string checker::name_argument_of(i32 file, ast::attribute const* a)
{
    if (a == nullptr)
        return {};
    auto const arguments = ast_of(file).at(a->arguments);
    auto const* const name = arguments.size() == 1 && arguments[0].name.empty() && !arguments[0].is_splat
                                  && ast::is_valid(arguments[0].value)
                               ? ast_of(file).at(arguments[0].value).node.try_as<ast::name>()
                               : nullptr;
    if (name == nullptr)
    {
        auto const attribute = text_of(file, a->name);
        report(diagnostic_kind::invalid_attribute_arguments, file, a->name,
               cc::format("@{} takes one name: `@{}({})`", attribute, attribute,
                          attribute == "sampler" ? "linear" : "normals"));
        return {};
    }
    return cc::string(text_of(file, name->where));
}

// ---- types ----------------------------------------------------------------------------------------------------------

bool checker::is_named(i32 file, ast::expr_id expr, cc::string_view name) const
{
    if (!ast::is_valid(expr))
        return false;
    auto const* const n = ast_of(file).at(expr).node.try_as<ast::name>();
    return n != nullptr && text_of(file, n->where) == name;
}

type_id checker::buffer_type(type_id element, bool is_mut)
{
    // Interned, because type equality is id equality: two mentions of `buffer[float]` are one type.
    for (auto i = isize(0); i < out.types.size(); ++i)
    {
        auto const& t = out.types[i];
        if (t.kind == type_kind::buffer && t.element == element && t.is_mut == is_mut)
            return type_id(i);
    }
    auto const id = type_id(out.types.size());
    out.types.push_back({.kind = type_kind::buffer, .element = element, .is_mut = is_mut});
    return id;
}

type_id checker::resolve_buffer(i32 file, ast::expr_id expr, ast::index const& node, function_scope const* scope)
{
    auto const where = span_of(file, expr);
    auto const arguments = ast_of(file).at(node.arguments);
    if (arguments.size() != 1 || !arguments[0].name.empty() || arguments[0].is_splat)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, where, "a buffer takes one element type: `buffer[float]`");
        return checked_module::error_type;
    }

    auto const element = resolve_type(file, arguments[0].value, scope);
    if (element == checked_module::error_type)
        return checked_module::error_type;

    // A struct element needs a layout rule the four targets agree on, which the spec's bindings file leaves open.
    if (out.builtin_type_of(element) == nullptr)
    {
        unsupported(file, span_of(file, arguments[0].value), "a buffer of anything but a scalar or a vector");
        return checked_module::error_type;
    }
    return buffer_type(element, false);
}

type_id checker::resolve_type(i32 file, ast::expr_id expr, function_scope const* scope)
{
    if (!ast::is_valid(expr))
        return checked_module::error_type;

    auto const& e = ast_of(file).at(expr);
    auto const where = span_of(file, expr);
    auto result = checked_module::error_type;

    if (!e.attributes.empty())
        unsupported(file, where, "an attribute on a type");

    auto const* const n = e.node.try_as<ast::name>();
    auto const resource = n != nullptr ? resolve_resource_name(file, expr, text_of(file, n->where)) : type_id::none;
    if (e.node.is<ast::void_ref>())
        result = checked_module::void_type;
    else if (resource != type_id::none)
        result = resource;
    else if (n != nullptr)
    {
        auto const text = text_of(file, n->where);
        auto const* const found = names_seen_from(file).get_ptr(text);
        // CHK-54: types and values share one namespace, so a local hides a type of its name
        if (auto const* const local = scope != nullptr ? scope->find_local(text) : nullptr)
        {
            set_target(file, expr, local->where);
            report(diagnostic_kind::wrong_kind_of_name, file, where,
                   cc::format("{} is a local, and a type stands here", text));
        }
        else if (found == nullptr || found->empty())
            report(diagnostic_kind::unknown_name, file, where, text);
        else
        {
            auto const id = found->front();
            auto const kind = out.at(id).kind;
            set_target(file, expr, {.kind = target_kind::symbol, .symbol = id});
            if (kind == symbol_kind::structure || kind == symbol_kind::enumeration)
            {
                if (demand(id, file, where) == symbol_state::checked)
                    result = out.at(id).type;
            }
            else if (kind != symbol_kind::unsupported)
                report(diagnostic_kind::wrong_kind_of_name, file, where,
                       cc::format("{} is a {}, and a type stands here", text,
                                  kind == symbol_kind::function   ? "function"
                                  : kind == symbol_kind::constant ? "const"
                                                                  : "binding"));
        }
    }
    else if (auto const* const applied = e.node.try_as<ast::index>())
    {
        if (is_named(file, applied->object, "buffer"))
            result = resolve_buffer(file, expr, *applied, scope);
        else if (auto const applied_resource = resolve_resource_applied(file, expr, *applied);
                 applied_resource != type_id::none)
            result = applied_resource;
        else if (ast::is_valid(applied->object) && is_type_name(file, applied->object))
            result = resolve_array(file, expr, *applied, scope);
        else
            unsupported(file, where, "type arguments");
    }
    else if (e.node.is<ast::struct_type>())
        unsupported(file, where, "an anonymous struct type");
    else if (e.node.is<ast::function_type>())
        unsupported(file, where, "a function type");
    else if (e.node.is<ast::tuple>())
        unsupported(file, where, "a tuple type");
    else if (e.node.is<ast::member>())
        unsupported(file, where, "a qualified type name");
    else if (auto const* const q = e.node.try_as<ast::qualified_type>())
    {
        auto const inner = resolve_type(file, q->type, scope);
        if (inner != checked_module::error_type)
            result = qualify_resource(file, expr, inner, q->access);
    }
    else if (!e.node.is<ast::invalid_expr>())
        unsupported(file, where, "this expression as a type");

    set_type(file, expr, result);
    return result;
}

type_id checker::resolve_value_type(i32 file, ast::expr_id expr, function_scope const* scope)
{
    auto const type = resolve_type(file, expr, scope);
    if (type == checked_module::error_type)
        return type;
    // CHK-286: a binding array is a binding member, whose length only the host knows
    auto innermost = type;
    while (out.at(innermost).kind == type_kind::array)
    {
        if (out.at(innermost).count == 0)
        {
            report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, expr),
                   "an array without a length is a binding array, which only a binding member may be");
            return checked_module::error_type;
        }
        innermost = out.at(innermost).element;
    }
    if (!is_resource(out.at(innermost).kind))
        return type;
    if (innermost != type)
    {
        unsupported(file, span_of(file, expr), "an array of resources as a value; a binding array is a binding member");
        return checked_module::error_type;
    }
    unsupported(file, span_of(file, expr),
                out.at(type).kind == type_kind::buffer
                    ? "a buffer as a value; a buffer is a binding member, read as `values[i]`"
                    : "a texture, an image or a sampler as a value; each is a binding member, handed to a builtin");
    return checked_module::error_type;
}

type_id checker::type_of_builtin(cc::string_view name, i32 file, source_span where)
{
    auto const* const found = prelude_names.get_ptr(name);
    if (found != nullptr && !found->empty())
    {
        auto const id = found->front();
        auto const kind = out.at(id).kind;
        if ((kind == symbol_kind::structure || kind == symbol_kind::enumeration)
            && demand(id, file, where) == symbol_state::checked && is_valid(out.at(id).intrinsic_type))
            return out.at(id).type;
    }
    report(diagnostic_kind::unknown_name, file, where, cc::format("{}, which the prelude must declare @builtin", name));
    return checked_module::error_type;
}

// ---- structs and bindings -------------------------------------------------------------------------------------------

ast::range_of<member_info> checker::compile_members(i32 file,
                                                    ast::range_of<ast::decl_id> members,
                                                    bool is_struct,
                                                    bool is_target_struct,
                                                    bool is_vertex_struct,
                                                    bool is_workgroup)
{
    auto const& ast = ast_of(file);
    auto const owner = is_struct ? cc::string_view("a struct field") : cc::string_view("a binding member");
    auto collected = cc::vector<member_info>();
    // `@sampler(name)` of a member, resolved once every member is known, since the sampler may stand below it
    struct named_sampler
    {
        isize member;
        ast::attribute const* attribute;
        cc::string name;
    };
    auto named_samplers = cc::vector<named_sampler>();

    for (auto const member : ast.at(members))
    {
        auto const& d = ast.at(member);
        auto const where = span_of(file, member);

        // CHK-292: workgroup memory holds values, which a sampler is none of
        if (auto const* const smp = d.node.try_as<ast::sampler_decl>(); smp != nullptr && is_workgroup)
        {
            report(diagnostic_kind::wrong_kind_of_name, file, smp->name,
                   "a @workgroup binding holds values the workgroup shares, and a sampler is none");
            continue;
        }
        if (auto const* const smp = d.node.try_as<ast::sampler_decl>(); smp != nullptr && !is_struct)
        {
            // CHK-204: a static sampler of the group, a member whose type is the sampler its settings make.
            auto const name = text_of(file, smp->name);
            auto is_duplicate = false;
            for (auto const& other : collected)
                is_duplicate = is_duplicate || other.name == name;
            if (is_duplicate)
            {
                report(diagnostic_kind::duplicate_declaration, file, smp->name, name);
                continue;
            }
            judge_attributes(file, d.attributes, {}, owner);
            auto const state = compile_sampler(file, *smp);
            collected.push_back({
                .name = name,
                .type = resource_type({.kind = type_kind::sampler, .is_comparison = state.compare >= 0}),
                .static_sampler = i32(out.samplers.size()),
            });
            out.samplers.push_back(state);
            continue;
        }
        // A struct's properties and methods are functions of its type scope, compiled as symbols of their own.
        auto const is_function = d.node.is<ast::property_decl>() || d.node.is<ast::fun_decl>();
        if (is_function && !is_struct)
            unsupported(file, where, d.node.is<ast::property_decl>() ? "a property of a binding" : "a method");
        // a `require` of a binding is read by compile_binding, and one in a struct was reported by the AST pass
        else if (!is_function && !d.node.is<ast::field_decl>() && !d.node.is<ast::invalid_decl>()
                 && !d.node.is<ast::test_decl>() && !d.node.is<ast::require_decl>())
            unsupported(file, where, "this member");

        auto const* const line = d.node.try_as<ast::field_decl>();
        if (line == nullptr || !ast::is_valid(line->field))
            continue;

        auto const& f = ast.at(line->field);
        if (f.name.empty())
            continue;
        auto const name = text_of(file, f.name);

        cc::string_view const known_on_field[] = {"position", "per_instance", "stream", "interpolate"};
        // CHK-276: a `@pixel struct` member may be the depth or the sample mask rather than a color target
        cc::string_view const known_on_pixel_field[]
            = {"position", "per_instance", "stream", "interpolate", "depth", "sample_mask"};
        // CHK-275: on a `@vertex struct` member `@format` is the member's own bytes, never a pipeline setting
        cc::string_view const known_on_vertex_field[] = {"position", "per_instance", "stream", "interpolate", "format"};
        cc::string_view const known_on_member[] = {"unfilterable", "non_filtering", "sampler"};
        judge_attributes(file, f.attributes,
                         !is_struct         ? cc::span<cc::string_view const>(known_on_member)
                         : is_vertex_struct ? cc::span<cc::string_view const>(known_on_vertex_field)
                         : is_target_struct ? cc::span<cc::string_view const>(known_on_pixel_field)
                                            : cc::span<cc::string_view const>(known_on_field),
                         owner, is_target_struct ? setting_scope::target : setting_scope::none);
        judge_attributes(file, d.attributes, {}, owner);
        if (f.is_mut)
            unsupported(file, f.name, "a mut member");
        if (!is_struct && f.is_named_only)
            unsupported(file, f.name, "a named-only binding member");

        auto is_duplicate = false;
        for (auto const& other : collected)
            is_duplicate = is_duplicate || other.name == name;
        if (is_duplicate)
        {
            report(diagnostic_kind::duplicate_declaration, file, f.name, name);
            continue;
        }

        auto type = checked_module::error_type;
        if (ast::is_valid(f.type))
            type = is_struct ? resolve_value_type(file, f.type) : resolve_type(file, f.type);
        else
            report(diagnostic_kind::missing_type, file, f.name, name);

        if (is_workgroup && type != checked_module::error_type && holds_resource(type))
        {
            report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, f.type),
                   cc::format("a @workgroup binding holds values the workgroup shares, and {} is a resource",
                              out.name_of(type)));
            type = checked_module::error_type;
        }
        // CHK-291: an array's layout in a block is the struct-buffer work's to settle
        if (!is_struct && !is_workgroup && type != checked_module::error_type && out.at(type).kind == type_kind::array)
        {
            auto innermost = type;
            while (out.at(innermost).kind == type_kind::array)
                innermost = out.at(innermost).element;
            unsupported(file, span_of(file, f.type),
                        is_resource(out.at(innermost).kind)
                            ? "a binding array"
                            : "an array in a constant block, whose layout no rule settles yet");
            type = checked_module::error_type;
        }

        // CHK-214: a binding member is a slot of the group's layout, and a void one fills none.
        if (!is_struct && type == checked_module::void_type)
        {
            report(diagnostic_kind::type_mismatch, file, span_of(file, f.type),
                   cc::format("{} is void, and a binding member has to hold something", name));
            type = checked_module::error_type;
        }

        // CHK-202 and CHK-203: each attribute names what only one kind of member can be.
        auto const* const unfilterable = find_attribute(file, f.attributes, "unfilterable");
        auto const* const non_filtering = find_attribute(file, f.attributes, "non_filtering");
        auto const& t = out.at(type);
        if (unfilterable != nullptr && type != checked_module::error_type
            && (t.kind != type_kind::texture || t.is_depth || !out.name_of(t.element).starts_with("float")))
            report(diagnostic_kind::wrong_kind_of_name, file, unfilterable->name,
                   "only a texture of floats is filtered, so only one can be @unfilterable");
        if (non_filtering != nullptr && type != checked_module::error_type
            && (t.kind != type_kind::sampler || t.is_comparison))
            report(diagnostic_kind::wrong_kind_of_name, file, non_filtering->name,
                   "only a `sampler` member can be @non_filtering");

        collected.push_back({
            .name = name,
            .type = type,
            .field = line->field,
            .is_position = find_attribute(file, f.attributes, "position") != nullptr,
            .interpolate = interpolation_of(file, find_attribute(file, f.attributes, "interpolate")),
            .has_interpolate = find_attribute(file, f.attributes, "interpolate") != nullptr,
            .output = is_target_struct ? pixel_output_of(file, f.attributes, type, name) : pixel_output::color,
            .vertex_format = is_vertex_struct ? vertex_format_of(file, find_attribute(file, f.attributes, "format"), type)
                                              : cc::string(),
            .is_per_instance = find_attribute(file, f.attributes, "per_instance") != nullptr,
            .stream = name_argument_of(file, find_attribute(file, f.attributes, "stream")),
            .is_unfilterable = unfilterable != nullptr,
            .is_non_filtering = non_filtering != nullptr,
        });
        if (auto const* const named = is_struct ? nullptr : find_attribute(file, f.attributes, "sampler"))
            named_samplers.push_back(
                {.member = collected.size() - 1, .attribute = named, .name = name_argument_of(file, named)});
    }

    // CHK-279: a texture names a sampler of its own binding, which its sampling calls take when they name none
    for (auto const& n : named_samplers)
    {
        auto& m = collected[n.member];
        if (n.name.empty() || m.type == checked_module::error_type)
            continue;
        if (auto const& t = out.at(m.type); t.kind != type_kind::texture)
        {
            report(diagnostic_kind::wrong_kind_of_name, file, n.attribute->name,
                   "only a texture is sampled, so only one takes a @sampler");
            continue;
        }
        auto found = isize(-1);
        for (auto i = isize(0); i < collected.size(); ++i)
            if (collected[i].name == n.name)
                found = i;
        auto const where = span_of(file, ast_of(file).at(n.attribute->arguments)[0].value);
        if (found < 0)
        {
            report(diagnostic_kind::unknown_member, file, where,
                   cc::format("the binding has no member {}, and @sampler names one of its own", n.name));
            continue;
        }
        if (collected[found].type != checked_module::error_type
            && out.at(collected[found].type).kind != type_kind::sampler)
        {
            report(diagnostic_kind::wrong_kind_of_name, file, where, cc::format("{} is no sampler", n.name));
            continue;
        }
        m.default_sampler = i32(found);
    }

    auto const range = ast::range_of<member_info>{.first = u32(out.members.size()), .count = u32(collected.size())};
    out.members.push_back_range(collected);
    return range;
}

void checker::compile_struct(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const decl = out.at(id).declaration;
    auto const& d = ast_of(file).at(decl);
    auto const& s = d.node.as<ast::struct_decl>();

    auto const is_vertex = find_attribute(file, d.attributes, "vertex") != nullptr;
    auto const is_pixel = find_attribute(file, d.attributes, "pixel") != nullptr;

    // An edge struct's attributes may be pipeline settings, which every pipeline it is an edge of starts from.
    cc::string_view const known[] = {"builtin", "vertex", "pixel", "shadowable"};
    judge_attributes(file, d.attributes, known, "a struct",
                     is_vertex || is_pixel ? setting_scope::description : setting_scope::none);

    auto const is_builtin = find_attribute(file, d.attributes, "builtin") != nullptr;
    if (is_builtin)
    {
        auto const intrinsic = builtins.find_type(out.at(id).name);
        if (is_valid(intrinsic))
            out.symbols[index_of(id)].intrinsic_type = intrinsic;
        else
            report(diagnostic_kind::unknown_builtin, file, s.name, out.at(id).name);
    }
    else if (s.is_opaque)
        report(diagnostic_kind::opaque_struct_needs_builtin, file, s.name, out.at(id).name);

    if (is_vertex && is_pixel)
        unsupported(file, s.name, "a struct of two stages");

    auto const members = compile_members(file, s.members, true, is_pixel, is_vertex);

    // The type exists only now, so a field that needs its own struct found a cycle and not a type.
    auto const type = type_id(out.types.size());
    out.types.push_back({
        .kind = type_kind::structure,
        .symbol = id,
        .members = members,
        .is_opaque = s.is_opaque,
        // A struct has no compute edge: a compute entry point has no stage struct at all.
        .edge = stage_of(is_vertex, is_pixel, false),
    });
    out.symbols[index_of(id)].type = type;
}

void checker::compile_enum(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const decl = out.at(id).declaration;
    auto const& ast = ast_of(file);
    auto const& e = ast.at(decl).node.as<ast::enum_decl>();

    cc::string_view const known[] = {"builtin", "shadowable"};
    judge_attributes(file, ast.at(decl).attributes, known, "an enum");
    // A builtin enum is written as its record says, `bool` as the target's bool, and not as the `int` of its cases.
    if (find_attribute(file, ast.at(decl).attributes, "builtin") != nullptr)
    {
        auto const intrinsic = builtins.find_type(out.at(id).name);
        if (is_valid(intrinsic))
            out.symbols[index_of(id)].intrinsic_type = intrinsic;
        else
            report(diagnostic_kind::unknown_builtin, file, e.name, out.at(id).name);
    }

    auto collected = cc::vector<enum_case_info>();
    auto next_value = cc::optional<i32>(0);

    for (auto const member : ast.at(e.members))
    {
        auto const& d = ast.at(member);
        auto const where = span_of(file, member);

        auto const* const c = d.node.try_as<ast::enum_case_decl>();
        if (c == nullptr)
        {
            // A field in an `enum` is a normal error the AST pass already reported (AST-86).
            // Its properties and methods are functions of its type scope, compiled as symbols of their own.
            if (!d.node.is<ast::property_decl>() && !d.node.is<ast::fun_decl>() && !d.node.is<ast::field_decl>()
                && !d.node.is<ast::invalid_decl>() && !d.node.is<ast::test_decl>())
                unsupported(file, where, "a declaration in an enum");
            continue;
        }

        judge_attributes(file, d.attributes, {}, "an enum case");
        if (c->name.empty())
            continue;
        auto const name = text_of(file, c->name);

        auto is_duplicate = false;
        for (auto const& other : collected)
            is_duplicate = is_duplicate || other.name == name;
        if (is_duplicate)
        {
            report(diagnostic_kind::duplicate_declaration, file, c->name, name);
            continue;
        }

        if (!next_value.has_value() && !ast::is_valid(c->value))
        {
            unsupported(file, c->name, "an implicit case value past the last int");
            continue;
        }
        auto value = next_value.value_or(0);
        if (ast::is_valid(c->value))
        {
            auto const where_value = span_of(file, c->value);
            auto const text = text_of(file, where_value);
            auto const written
                = ast.at(c->value).node.is<ast::literal>() && classify_number(text) == number_class::plain_integer
                    ? parse_plain_integer(text)
                    : cc::optional<i32>();
            if (written.has_value())
                value = written.value();
            else
                unsupported(file, where_value, "a case value that is no int literal");
        }
        // An implicit value follows the one before it, and `int` has no value after its last.
        if (value == 2147483647)
            next_value = cc::nullopt;
        else
            next_value = value + 1;

        collected.push_back({.name = cc::string(name), .value = value, .declaration = member});
    }

    auto const cases = ast::range_of<enum_case_info>{
        .first = u32(out.enum_cases.size()),
        .count = u32(collected.size()),
    };
    for (auto& c : collected)
        out.enum_cases.push_back(cc::move(c));

    auto const type = type_id(out.types.size());
    out.types.push_back({.kind = type_kind::enumeration, .symbol = id, .cases = cases});
    out.symbols[index_of(id)].type = type;
}

bool checker::is_shadowable_by(i32 file, ast::range_of<ast::attribute> attributes) const
{
    auto const* const a = find_attribute(file, attributes, "shadowable");
    if (a == nullptr)
        return true;
    auto const arguments = ast_of(file).at(a->arguments);
    return !(arguments.size() == 1 && ast::is_valid(arguments[0].value)
             && text_of(file, span_of(file, arguments[0].value)) == "false");
}

void checker::judge_shadowing(i32 file, cc::string_view name, source_span where)
{
    // CHK-220: a local or a parameter hides a module-level symbol of its name (CHK-54), unless that one says it may not.
    auto const* const found = names_seen_from(file).get_ptr(name);
    if (found == nullptr)
        return;
    for (auto const s : *found)
        if (!out.at(s).is_shadowable)
        {
            report(diagnostic_kind::shadows_unshadowable, file, where, cc::format("{} is @shadowable(false)", name));
            return;
        }
}

void checker::compile_const(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const decl = out.at(id).declaration;
    auto const& ast = ast_of(file);
    auto const& d = ast.at(decl);
    auto const& c = d.node.as<ast::const_decl>();
    constexpr auto error_type = checked_module::error_type;

    cc::string_view const known[] = {"shadowable"};
    judge_attributes(file, d.attributes, known, "a const");

    auto const fail = [&] { out.symbols[index_of(id)].state = symbol_state::failed; };
    if (!ast::is_valid(c.value))
    {
        unsupported(file, c.name, "a const without a value");
        return fail();
    }

    // CHK-219: a literal, an enum case or another const, which is all a value known before the program runs is yet.
    auto const& value = ast.at(c.value);
    auto const where = span_of(file, c.value);
    auto info = constant_info{.symbol = id};
    auto is_negated = false;
    auto literal = c.value;
    if (auto const* const call = value.node.try_as<ast::call>();
        call != nullptr && call->spelling == ast::call_spelling::prefix && sgl::is_valid(call->op)
        && text_of(file, file_of(file).at(call->op).where) == "-" && ast.at(call->arguments).size() == 1)
    {
        is_negated = true;
        literal = ast.at(call->arguments)[0].value;
    }

    if (ast::is_valid(literal) && ast.at(literal).node.is<ast::literal>())
    {
        auto const text = text_of(file, span_of(file, literal));
        auto const number = classify_number(text);
        if (number == number_class::plain_integer && parse_plain_integer(text).has_value())
        {
            info.kind = constant_kind::integer;
            info.integer = is_negated ? -parse_plain_integer(text).value() : parse_plain_integer(text).value();
            info.type = type_of_builtin(builtins::k_int, file, where);
        }
        else if (number == number_class::plain_float && parse_plain_float(text).has_value())
        {
            info.kind = constant_kind::real;
            info.real = is_negated ? -parse_plain_float(text).value() : parse_plain_float(text).value();
            info.type = type_of_builtin(builtins::k_float, file, where);
        }
        else
        {
            unsupported(file, where, "a const whose literal is no plain int or float");
            return fail();
        }
    }
    else if (value.node.is<ast::member>() || value.node.is<ast::name>())
    {
        auto scope = function_scope{.file = file};
        auto const type = check_expr(scope, c.value);
        if (type == error_type)
            return fail();
        auto const& target = out.files[file].target_at(c.value);
        if (target.kind == target_kind::enum_case)
        {
            info.kind = constant_kind::enum_case;
            info.case_index = target.index;
            info.type = type;
        }
        else if (target.kind == target_kind::symbol && out.at(target.symbol).kind == symbol_kind::constant)
        {
            info = out.constants[out.at(target.symbol).info];
            info.symbol = id;
        }
        else
        {
            unsupported(file, where, "a const whose value is no literal, no enum case and no const");
            return fail();
        }
    }
    else
    {
        unsupported(file, where, "a const whose value is no literal, no enum case and no const");
        return fail();
    }
    if (info.type == error_type)
        return fail();

    if (ast::is_valid(c.type))
    {
        auto const declared = resolve_value_type(file, c.type);
        if (declared != error_type && declared != info.type)
        {
            tell_apart(report(diagnostic_kind::type_mismatch, file, where,
                              cc::format("expected {}, got {}", out.name_of(declared), out.name_of(info.type))),
                       declared, info.type);
            return fail();
        }
    }

    set_type(file, c.value, info.type);
    out.symbols[index_of(id)].type = info.type;
    out.symbols[index_of(id)].info = i32(out.constants.size());
    out.constants.push_back(info);
}

void checker::compile_binding(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const decl = out.at(id).declaration;
    auto const& d = ast_of(file).at(decl);
    auto const& b = d.node.as<ast::binding_decl>();

    cc::string_view const known[] = {"inline", "workgroup", "shadowable"};
    judge_attributes(file, d.attributes, known, "a binding");
    auto const is_workgroup = find_attribute(file, d.attributes, "workgroup") != nullptr;

    if (ast::is_valid(b.composition))
    {
        unsupported(file, span_of(file, b.composition), "a binding composition");
        out.symbols[index_of(id)].state = symbol_state::failed;
        return;
    }

    // CHK-260: its own `require` lines grant its members what their file does not.
    auto declared = feature_set();
    for (auto const member : ast_of(file).at(b.members))
        if (auto const* const r = ast_of(file).at(member).node.try_as<ast::require_decl>())
        {
            judge_attributes(file, ast_of(file).at(member).attributes, {}, "a require");
            declared |= read_require(file, *r, require_scope::binding, id);
        }

    // `compile` restores whatever grant was in effect around this binding.
    auto used = feature_set();
    granted = declared;
    used_features = &used;
    auto const members = compile_members(file, b.members, false, false, false, is_workgroup);
    granted = {};
    used_features = nullptr;

    auto const is_inline = find_attribute(file, d.attributes, "inline") != nullptr;
    if (is_inline && is_workgroup)
        report(diagnostic_kind::invalid_attribute_arguments, file, find_attribute(file, d.attributes, "workgroup")->name,
               "a binding is @inline constants or @workgroup memory, never both");
    // CHK-293: WebGPU's default limit, and vulkan's required minimum, is what every target has
    if (is_workgroup)
    {
        auto total = 0;
        for (auto const& m : out.at(members))
            if (m.type != checked_module::error_type)
                total += workgroup_size_of(m.type);
        if (total > k_portable_workgroup_bytes)
            report(diagnostic_kind::invalid_attribute_arguments, file,
                   find_attribute(file, d.attributes, "workgroup")->name,
                   cc::format("{} holds {} bytes, and a workgroup has {} on every target", out.at(id).name, total,
                              k_portable_workgroup_bytes));
    }
    // CHK-205: an `@inline` binding holds constants only, so a static sampler in one has nowhere to go.
    if (is_inline)
        for (auto const member : ast_of(file).at(b.members))
            if (auto const* const smp = ast_of(file).at(member).node.try_as<ast::sampler_decl>())
                report(diagnostic_kind::wrong_kind_of_name, file, smp->name,
                       "an @inline binding holds constants only, and a sampler is none");
    out.symbols[index_of(id)].info = i32(out.bindings.size());
    out.bindings.push_back({
        .symbol = id,
        .is_inline = is_inline,
        .is_workgroup = is_workgroup,
        .members = members,
        .declared = declared,
        .required = declared | used,
    });
}

// ---- functions ------------------------------------------------------------------------------------------------------

void checker::compile_function(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const decl = out.at(id).declaration;
    auto const& ast = ast_of(file);
    auto const& d = ast.at(decl);
    auto const& f = d.node.as<ast::fun_decl>();
    auto is_failed = false;

    // An entry point's attributes may be pipeline settings, which every pipeline it is a stage of starts from.
    auto const is_raster_entry = find_attribute(file, d.attributes, "vertex") != nullptr
                              || find_attribute(file, d.attributes, "pixel") != nullptr;
    cc::string_view const known[]
        = {"builtin", "pure", "operator", "vertex", "pixel", "compute", "stages", "shadowable", "expect"};
    judge_attributes(file, d.attributes, known, "a function",
                     is_raster_entry ? setting_scope::description : setting_scope::none);
    read_footprint_pin(id, file, d.attributes,
                       is_raster_entry || find_attribute(file, d.attributes, "compute") != nullptr);

    if (!f.type_parameters.empty())
    {
        unsupported(file, f.name, "a generic function");
        is_failed = true;
    }
    // CHK-234: `self` is the receiver of a method, a parameter of its type; `mut self` waits for places (CHK-134)
    auto receiver = checked_module::error_type;
    if (f.receiver == ast::receiver_kind::mut_self)
    {
        unsupported(file, f.name, "mut self");
        is_failed = true;
    }
    else if (f.receiver == ast::receiver_kind::self && out.at(id).role != function_role::method)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, f.name,
               "self is the receiver of a method, and this function belongs to no type");
        is_failed = true;
    }
    else if (f.receiver == ast::receiver_kind::self)
        receiver = receiver_of(id);

    auto const is_builtin = find_attribute(file, d.attributes, "builtin") != nullptr;
    auto parameters = cc::vector<parameter>();
    for (auto const& p : ast.at(f.parameters))
    {
        auto const name = text_of(file, p.name);
        // CHK-271: a stage input is a parameter its attribute marks, one attribute per input
        cc::string_view known_on_parameter[16] = {};
        auto known_count = isize(0);
        for (auto const& input : stage_inputs())
            known_on_parameter[known_count++] = input.name;
        judge_attributes(file, p.attributes, cc::span<cc::string_view const>(known_on_parameter, known_count),
                         "a parameter");
        auto input = stage_input::none;
        for (auto const& candidate : stage_inputs())
            if (find_attribute(file, p.attributes, candidate.name) != nullptr)
            {
                if (input != stage_input::none)
                {
                    report(diagnostic_kind::invalid_attribute_arguments, file, p.name,
                           cc::format("{} is marked as two stage inputs; a parameter is one", name));
                    is_failed = true;
                }
                input = candidate.input;
            }
        // `mut self` was reported as itself
        if (p.is_mut && f.receiver != ast::receiver_kind::mut_self)
            unsupported(file, p.name, "a mut parameter");

        for (auto const& other : parameters)
            if (other.name == name && name != "_")
            {
                report(diagnostic_kind::duplicate_declaration, file, p.name, name);
                is_failed = true;
            }

        // CHK-206: a builtin alone may take a resource, and its parameter is then a pattern of one (CHK-207).
        auto type = checked_module::error_type;
        auto const is_receiver = f.receiver != ast::receiver_kind::none && &p == &ast.at(f.parameters).front();
        if (ast::is_valid(p.type))
            type = is_builtin ? resolve_pattern_type(file, p.type) : resolve_value_type(file, p.type);
        else if (is_receiver)
            type = receiver;
        else
            report(diagnostic_kind::missing_type, file, span_of(file, p.form), name);
        is_failed = is_failed || type == checked_module::error_type;

        auto const index = isize(&p - ast.fields.data());
        parameters.push_back({.name = name,
                              .type = type,
                              .field = ast::field_id(index),
                              .has_default = ast::is_valid(p.default_value),
                              .is_named_only = p.is_named_only,
                              .input = input});
    }

    auto bindings = cc::vector<symbol_id>();
    for (auto const& entry : ast.at(f.bindings))
    {
        auto const where = span_of(file, entry.form);
        auto const* const n = ast::is_valid(entry.value) ? ast.at(entry.value).node.try_as<ast::name>() : nullptr;
        if (n == nullptr || !entry.name.empty() || entry.is_splat || !entry.attributes.empty())
        {
            // an `invalid` entry was reported by the AST pass
            if (!ast::is_valid(entry.value) || !ast.at(entry.value).node.is<ast::invalid_expr>())
                unsupported(file, where, "a binding entry that is not a bare name");
            is_failed = true;
            continue;
        }

        auto const text = text_of(file, n->where);
        auto const* const found = names_seen_from(file).get_ptr(text);
        if (found == nullptr || found->empty())
        {
            report(diagnostic_kind::unknown_name, file, where, text);
            is_failed = true;
            continue;
        }
        auto const binding = found->front();
        set_target(file, entry.value, {.kind = target_kind::symbol, .symbol = binding});
        if (out.at(binding).kind == symbol_kind::binding)
        {
            if (demand(binding, file, where) == symbol_state::checked)
                bindings.push_back(binding);
            else
                is_failed = true;
        }
        else
        {
            if (out.at(binding).kind != symbol_kind::unsupported)
                report(diagnostic_kind::wrong_kind_of_name, file, where, cc::format("{} is no binding", text));
            is_failed = true;
        }
    }

    // Without `-> T` a block body returns `void`, and an arrow body returns what its expression is.
    auto result = checked_module::void_type;
    auto const infers_result = !ast::is_valid(f.return_type) && f.body.kind == ast::body_kind::arrow;
    if (ast::is_valid(f.return_type))
        result = resolve_value_type(file, f.return_type);
    else if (infers_result)
        result = checked_module::error_type;
    is_failed = is_failed || (result == checked_module::error_type && !infers_result);

    auto const has_body = f.body.kind != ast::body_kind::none;
    if (find_attribute(file, d.attributes, "builtin") != nullptr)
    {
        // The record is the overload: the name, the parameter types and the named-only names together, as the registry read
        // them from its own text.
        auto types = cc::vector<cc::string_view>();
        auto named_only = cc::vector<cc::string_view>();
        auto is_silent = false;
        for (auto const& p : parameters)
        {
            is_silent = is_silent || p.type == checked_module::error_type;
            types.push_back(out.name_of(p.type));
            named_only.push_back(p.is_named_only ? cc::string_view(p.name) : cc::string_view());
        }
        auto const intrinsic = builtins.find_function(out.at(id).name, types, named_only);
        if (is_valid(intrinsic))
            out.symbols[index_of(id)].intrinsic = intrinsic;
        else
        {
            // a parameter type that did not resolve was reported, and it is why no record fits
            if (!is_silent)
            {
                auto text = cc::vector<type_id>();
                for (auto const& p : parameters)
                    text.push_back(p.type);
                report(diagnostic_kind::unknown_builtin, file, f.name,
                       builtins.has_function_named(out.at(id).name) ? signature_text(out.at(id).name, text)
                                                                    : cc::string(out.at(id).name));
            }
            is_failed = true;
        }
        if (has_body)
            unsupported(file, span_of(file, f.body.form), "a @builtin function with a body");
    }
    else if (!has_body)
    {
        report(diagnostic_kind::expected_body, file, f.name, out.at(id).name);
        is_failed = true;
    }

    auto const is_vertex = find_attribute(file, d.attributes, "vertex") != nullptr;
    auto const is_pixel = find_attribute(file, d.attributes, "pixel") != nullptr;
    auto const* const compute = find_attribute(file, d.attributes, "compute");
    auto const workgroup = workgroup_of(file, compute);

    out.symbols[index_of(id)].info = i32(out.functions.size());
    out.functions.push_back({
        .symbol = id,
        .parameters = {.first = u32(out.parameters.size()), .count = u32(parameters.size())},
        .result = result,
        .bindings = {.first = u32(out.binding_lists.size()), .count = u32(bindings.size())},
        .entry_stage = stage_of(is_vertex, is_pixel, compute != nullptr),
        .workgroup = {workgroup[0], workgroup[1], workgroup[2]},
        .is_pure = find_attribute(file, d.attributes, "pure") != nullptr,
        .stages = stages_of(file, find_attribute(file, d.attributes, "stages")),
    });
    out.parameters.push_back_range(parameters);
    out.binding_lists.push_back_range(bindings);
    notes.push_back({.infers_result = infers_result});

    // The body is part of what a caller needs here, so it is checked now, while the symbol is still in compilation.
    // Whoever demands this function from inside that body meets `in_compilation`, which is the dependency cycle.
    // A signature that failed has no body check at all, like every other failed function.
    if (infers_result)
    {
        if (!is_failed)
            check_body(id);
        is_failed = is_failed || out.functions[out.at(id).info].result == checked_module::error_type;
    }

    auto const stages = i32(is_vertex) + i32(is_pixel) + i32(compute != nullptr);
    if (stages > 1)
        report(diagnostic_kind::invalid_entry_point, file, f.name, "an entry point has one stage");
    else if (!is_failed && stages == 1)
        judge_entry_point(id);

    if (is_failed)
        out.symbols[index_of(id)].state = symbol_state::failed;
}

void checker::compile_constructor(symbol_id id)
{
    auto const structure = out.at(id).owner;
    auto const file = out.at(id).file;
    auto const where = ast_of(file).at(out.at(structure).declaration).node.as<ast::struct_decl>().name;
    auto const is_checked = demand(structure, file, where) == symbol_state::checked;

    auto parameters = cc::vector<parameter>();
    auto result = checked_module::error_type;
    if (is_checked)
    {
        result = out.at(structure).type;
        for (auto const& m : out.at(out.at(result).members))
        {
            auto const& f = ast_of(file).at(m.field);
            parameters.push_back({.name = m.name,
                                  .type = m.type,
                                  .field = m.field,
                                  .has_default = ast::is_valid(f.default_value),
                                  .is_named_only = f.is_named_only});
        }
    }

    out.symbols[index_of(id)].info = i32(out.functions.size());
    out.functions.push_back({
        .symbol = id,
        .parameters = {.first = u32(out.parameters.size()), .count = u32(parameters.size())},
        .result = result,
        .is_pure = true,
    });
    out.parameters.push_back_range(parameters);
    // A construction has no body of its own, so there is nothing to check and nothing to inline.
    notes.push_back({.is_body_checked = true, .is_body_sound = true});
    if (!is_checked)
        out.symbols[index_of(id)].state = symbol_state::failed;
}

type_id checker::receiver_of(symbol_id id)
{
    auto const owner = out.at(id).owner;
    if (!is_valid(owner))
        return checked_module::error_type;
    auto const& o = out.at(owner);
    auto const& decl = ast_of(o.file).at(o.declaration).node;
    auto const* const s = decl.try_as<ast::struct_decl>();
    auto const where = s != nullptr ? s->name : decl.as<ast::enum_decl>().name;
    if (demand(owner, o.file, where) != symbol_state::checked)
        return checked_module::error_type;
    return out.at(owner).type;
}

void checker::compile_property(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const& d = ast_of(file).at(out.at(id).declaration);
    auto const& p = d.node.as<ast::property_decl>();
    judge_attributes(file, d.attributes, {}, "a property");

    // CHK-236: one parameter, `self`, which nobody writes
    auto const self = receiver_of(id);
    auto is_failed = self == checked_module::error_type;
    auto const infers_result = !ast::is_valid(p.return_type);
    auto result = infers_result ? checked_module::error_type : resolve_value_type(file, p.return_type);
    is_failed = is_failed || (!infers_result && result == checked_module::error_type);
    // a member line without a body does not parse as a property, so this is an extension's
    if (p.body.kind == ast::body_kind::none)
    {
        report(diagnostic_kind::expected_body, file, p.name, out.at(id).name);
        is_failed = true;
    }

    auto const parameters = cc::vector<parameter>{{.name = "self", .type = self}};
    out.symbols[index_of(id)].info = i32(out.functions.size());
    out.functions.push_back({
        .symbol = id,
        .parameters = {.first = u32(out.parameters.size()), .count = u32(parameters.size())},
        .result = result,
    });
    out.parameters.push_back_range(parameters);
    notes.push_back({.infers_result = infers_result});

    // Its value is its result, so a property without `-> T` is checked now, as an arrow body without one is.
    if (infers_result)
    {
        if (!is_failed)
            check_body(id);
        is_failed = is_failed || out.functions[out.at(id).info].result == checked_module::error_type;
    }
    if (is_failed)
        out.symbols[index_of(id)].state = symbol_state::failed;
}

void checker::judge_entry_point(symbol_id id)
{
    auto const& s = out.at(id);
    auto const file = s.file;
    auto const& info = out.functions[s.info];
    auto const where = ast_of(file).at(s.declaration).node.as<ast::fun_decl>().name;
    auto const parameters = out.at(info.parameters);
    auto const& result = out.at(info.result);
    auto is_valid = true;
    auto const invalid = [&](cc::string_view detail)
    {
        report(diagnostic_kind::invalid_entry_point, file, where, detail);
        is_valid = false;
    };

    if (sgl::is_valid(s.intrinsic))
        invalid("an entry point is no @builtin");
    if (!s.operator_spelling.empty())
        invalid("an entry point is no @operator");

    // CHK-294: a workgroup is a compute stage's, and all its memory together fits the portable budget
    auto workgroup_bytes = 0;
    for (auto const binding : out.at(info.bindings))
    {
        auto const& b = out.bindings[out.at(binding).info];
        if (!b.is_workgroup)
            continue;
        if (info.entry_stage != stage::compute)
            invalid(cc::format("{} is @workgroup memory, which only a compute stage has", out.at(binding).name));
        for (auto const& m : out.at(b.members))
            if (m.type != checked_module::error_type)
                workgroup_bytes += workgroup_size_of(m.type);
    }
    if (workgroup_bytes > k_portable_workgroup_bytes)
        invalid(cc::format("its @workgroup bindings hold {} bytes, and a workgroup has {} on every target",
                           workgroup_bytes, k_portable_workgroup_bytes));

    // CHK-271: at most one stage struct, first, and then the stage inputs, each of this stage, each once, of its type
    auto structs = 0;
    auto seen = cc::vector<stage_input>();
    for (auto const& parameter : parameters)
    {
        if (parameter.input == stage_input::none)
        {
            if (!seen.empty() || structs > 0)
                invalid("an entry point takes its stage struct first, and stage inputs after it");
            ++structs;
            continue;
        }
        auto const& input = info_of(parameter.input);
        if (input.in_stage != info.entry_stage)
            invalid(cc::format("@{} is an input of the {} stage", input.name,
                               input.in_stage == stage::vertex  ? "vertex"
                               : input.in_stage == stage::pixel ? "pixel"
                                                                : "compute"));
        else if (out.name_of(parameter.type) != input.type)
            invalid(cc::format("a @{} parameter is an {}", input.name, input.type));
        for (auto const other : seen)
            if (other == parameter.input)
                invalid(cc::format("@{} is taken twice", input.name));
        seen.push_back(parameter.input);
    }
    auto const* const stage_struct = structs == 1 && parameters[0].input == stage_input::none ? &parameters[0] : nullptr;

    if (info.entry_stage == stage::compute)
    {
        // A compute entry point is dispatched over a grid and hands nothing back, and is given nothing but its inputs.
        if (info.result != checked_module::void_type)
            invalid("a @compute fun returns nothing");
        if (stage_struct != nullptr)
            invalid("a @compute fun takes stage inputs alone, such as `@thread_id id: int3`");
        notes[s.info].is_valid_entry = is_valid;
        return;
    }

    // A vertex stage may draw from no vertex buffer at all; a pixel stage always takes what the vertex stage hands on.
    if (info.entry_stage == stage::pixel && stage_struct == nullptr)
        invalid("a @pixel fun takes the struct its vertex stage returns");
    else if (stage_struct != nullptr && info.entry_stage == stage::vertex
             && out.at(stage_struct->type).edge != stage::vertex)
        invalid("the struct parameter of a @vertex fun is a @vertex struct");
    else if (stage_struct != nullptr && out.at(stage_struct->type).is_opaque)
        invalid("the struct parameter of an entry point is a struct with fields");

    // CHK-273: what crosses from the vertex to the pixel stage says how; an integer can only cross flat, and nothing
    // that does not cross is interpolated at all
    auto const judge_link = [&](type_id link)
    {
        for (auto const& m : out.at(out.at(link).members))
        {
            auto const* const record = out.builtin_type_of(m.type);
            auto const is_integer
                = record != nullptr
               && (record->leaf_kind == value_kind::scalar_int || record->leaf_kind == value_kind::scalar_uint);
            if (is_integer && m.interpolate.kind != interpolation::kind_t::flat)
                invalid(cc::format("the {} member '{}' crosses stages only flat: write `@interpolate(.flat)`",
                                   out.name_of(m.type), m.name));
            if (m.has_interpolate && m.is_position)
                invalid(cc::format("the @position member '{}' is the rasterizer's, and is no interpolated value", m.name));
        }
    };
    auto const judge_uninterpolated = [&](type_id edge)
    {
        for (auto const& m : out.at(out.at(edge).members))
            if (m.has_interpolate)
                invalid(cc::format("'{}' crosses no stage edge, so @interpolate means nothing on it", m.name));
    };
    if (info.entry_stage == stage::vertex && stage_struct != nullptr)
        judge_uninterpolated(stage_struct->type);

    if (info.entry_stage == stage::pixel)
    {
        if (result.edge != stage::pixel)
            invalid("a @pixel fun returns a @pixel struct");
        else
        {
            judge_uninterpolated(info.result);
            // CHK-276: one depth and one sample mask at most, since each is the pixel's one value
            auto depths = 0;
            auto masks = 0;
            for (auto const& m : out.at(result.members))
            {
                depths += m.output != pixel_output::color && m.output != pixel_output::sample_mask ? 1 : 0;
                masks += m.output == pixel_output::sample_mask ? 1 : 0;
            }
            if (depths > 1 || masks > 1)
                invalid("a @pixel struct has at most one @depth member and one @sample_mask member");
        }
        if (stage_struct != nullptr)
            judge_link(stage_struct->type);
    }
    else
    {
        auto positions = 0;
        auto is_hpos4 = true;
        for (auto const& m : out.at(result.members))
            if (m.is_position)
            {
                ++positions;
                auto const& type = out.at(m.type);
                auto const* const record = out.builtin_type_of(m.type);
                is_hpos4 = is_hpos4 && type.kind == type_kind::structure && record != nullptr
                        && record->name == builtins::k_hpos4;
            }
        if (positions != 1)
            invalid("a @vertex fun returns a struct with exactly one @position field");
        else if (!is_hpos4)
            invalid("the @position field of a @vertex fun is an hpos4");
        judge_link(info.result);
    }

    notes[s.info].is_valid_entry = is_valid;
}

cc::span<stage_input_info const> sgl::check::stage_inputs()
{
    static constexpr stage_input_info k_inputs[] = {
        {.input = stage_input::vertex_index, .name = "vertex_index", .in_stage = stage::vertex, .type = "int"},
        {.input = stage_input::instance_index, .name = "instance_index", .in_stage = stage::vertex, .type = "int"},
        {.input = stage_input::is_front_facing, .name = "is_front_facing", .in_stage = stage::pixel, .type = "bool"},
        {.input = stage_input::sample_index,
         .name = "sample_index",
         .in_stage = stage::pixel,
         .type = "int",
         .feature = i32(feature::sample_rate_shading)},
        {.input = stage_input::sample_mask, .name = "sample_mask", .in_stage = stage::pixel, .type = "uint"},
        {.input = stage_input::primitive_id,
         .name = "primitive_id",
         .in_stage = stage::pixel,
         .type = "int",
         .feature = i32(feature::primitive_index)},
        {.input = stage_input::thread_id, .name = "thread_id", .in_stage = stage::compute, .type = "int3"},
        {.input = stage_input::local_thread_id, .name = "local_thread_id", .in_stage = stage::compute, .type = "int3"},
        {.input = stage_input::local_thread_index, .name = "local_thread_index", .in_stage = stage::compute, .type = "int"},
        {.input = stage_input::workgroup_id, .name = "workgroup_id", .in_stage = stage::compute, .type = "int3"},
    };
    return k_inputs;
}

stage_input_info const& sgl::check::info_of(stage_input input)
{
    for (auto const& i : stage_inputs())
        if (i.input == input)
            return i;
    CC_UNREACHABLE("a stage input without an entry in stage_inputs()");
}
