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

    // CHK-203: each argument is one stage as an enum case; the function is reached only from an entry point of one.
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
cc::string checker::stream_of(i32 file, ast::attribute const* a)
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
        report(diagnostic_kind::invalid_attribute_arguments, file, a->name, "@stream takes one name: `@stream(normals)`");
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

type_id checker::resolve_buffer(i32 file, ast::expr_id expr, ast::index const& node)
{
    auto const where = span_of(file, expr);
    auto const arguments = ast_of(file).at(node.arguments);
    if (arguments.size() != 1 || !arguments[0].name.empty() || arguments[0].is_splat)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, where, "a buffer takes one element type: `buffer[float]`");
        return checked_module::error_type;
    }

    auto const element = resolve_type(file, arguments[0].value);
    if (element == checked_module::error_type)
        return checked_module::error_type;

    // A struct element needs a layout rule the four targets agree on, which the spec's bindings file leaves open.
    auto const& info = out.at(element);
    if (info.kind != type_kind::structure || !sgl::is_valid(out.at(info.symbol).intrinsic_type))
    {
        unsupported(file, span_of(file, arguments[0].value), "a buffer of anything but a scalar or a vector");
        return checked_module::error_type;
    }
    return buffer_type(element, false);
}

type_id checker::resolve_type(i32 file, ast::expr_id expr)
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
    if (resource != type_id::none)
        result = resource;
    else if (n != nullptr)
    {
        auto const text = text_of(file, n->where);
        auto const* const found = names.get_ptr(text);
        if (found == nullptr || found->empty())
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
                                  kind == symbol_kind::function ? "function" : "binding"));
        }
    }
    else if (auto const* const applied = e.node.try_as<ast::index>())
    {
        if (is_named(file, applied->object, "buffer"))
            result = resolve_buffer(file, expr, *applied);
        else if (auto const applied_resource = resolve_resource_applied(file, expr, *applied);
                 applied_resource != type_id::none)
            result = applied_resource;
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
        auto const inner = resolve_type(file, q->type);
        if (inner != checked_module::error_type)
            result = qualify_resource(file, expr, inner, q->access);
    }
    else if (!e.node.is<ast::invalid_expr>())
        unsupported(file, where, "this expression as a type");

    set_type(file, expr, result);
    return result;
}

type_id checker::resolve_value_type(i32 file, ast::expr_id expr)
{
    auto const type = resolve_type(file, expr);
    if (type == checked_module::error_type || !is_resource(out.at(type).kind))
        return type;
    unsupported(file, span_of(file, expr),
                out.at(type).kind == type_kind::buffer
                    ? "a buffer as a value; a buffer is a binding member, read as `values[i]`"
                    : "a texture, an image or a sampler as a value; each is a binding member, handed to a builtin");
    return checked_module::error_type;
}

type_id checker::type_of_builtin(cc::string_view name, i32 file, source_span where)
{
    auto const* const found = names.get_ptr(name);
    if (found != nullptr && !found->empty())
    {
        auto const id = found->front();
        if (out.at(id).kind == symbol_kind::structure && demand(id, file, where) == symbol_state::checked
            && is_valid(out.at(id).intrinsic_type))
            return out.at(id).type;
    }
    report(diagnostic_kind::unknown_name, file, where, cc::format("{}, which the prelude must declare @builtin", name));
    return checked_module::error_type;
}

// ---- structs and bindings -------------------------------------------------------------------------------------------

ast::range_of<member_info> checker::compile_members(i32 file,
                                                    ast::range_of<ast::decl_id> members,
                                                    bool is_struct,
                                                    bool is_target_struct)
{
    auto const& ast = ast_of(file);
    auto const owner = is_struct ? cc::string_view("a struct field") : cc::string_view("a binding member");
    auto collected = cc::vector<member_info>();

    for (auto const member : ast.at(members))
    {
        auto const& d = ast.at(member);
        auto const where = span_of(file, member);

        if (auto const* const smp = d.node.try_as<ast::sampler_decl>(); smp != nullptr && !is_struct)
        {
            // CHK-199: a static sampler of the group, a member whose type is the sampler its settings make.
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
        if (d.node.is<ast::property_decl>())
            unsupported(file, where, "a property");
        else if (d.node.is<ast::fun_decl>())
            unsupported(file, where, "a method");
        else if (!d.node.is<ast::field_decl>() && !d.node.is<ast::invalid_decl>())
            unsupported(file, where, "this member");

        auto const* const line = d.node.try_as<ast::field_decl>();
        if (line == nullptr || !ast::is_valid(line->field))
            continue;

        auto const& f = ast.at(line->field);
        if (f.name.empty())
            continue;
        auto const name = text_of(file, f.name);

        cc::string_view const known_on_field[] = {"position", "thread_id", "per_instance", "stream"};
        cc::string_view const known_on_member[] = {"unfilterable", "non_filtering"};
        judge_attributes(file, f.attributes,
                         is_struct ? cc::span<cc::string_view const>(known_on_field)
                                   : cc::span<cc::string_view const>(known_on_member),
                         owner, is_target_struct ? setting_scope::target : setting_scope::none);
        judge_attributes(file, d.attributes, {}, owner);
        if (f.is_mut)
            unsupported(file, f.name, "a mut member");
        if (ast::is_valid(f.default_value))
            unsupported(file, span_of(file, f.default_value), "a default value");

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

        // CHK-197 and CHK-198: each attribute names what only one kind of member can be.
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
            .is_thread_id = find_attribute(file, f.attributes, "thread_id") != nullptr,
            .is_per_instance = find_attribute(file, f.attributes, "per_instance") != nullptr,
            .stream = stream_of(file, find_attribute(file, f.attributes, "stream")),
            .is_unfilterable = unfilterable != nullptr,
            .is_non_filtering = non_filtering != nullptr,
        });
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
    cc::string_view const known[] = {"builtin", "vertex", "pixel"};
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

    auto const members = compile_members(file, s.members, true, is_pixel);

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

    judge_attributes(file, ast.at(decl).attributes, {}, "an enum");

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
            if (d.node.is<ast::property_decl>())
                unsupported(file, where, "a property");
            else if (d.node.is<ast::fun_decl>())
                unsupported(file, where, "a method");
            else if (!d.node.is<ast::field_decl>() && !d.node.is<ast::invalid_decl>())
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

void checker::compile_binding(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const decl = out.at(id).declaration;
    auto const& d = ast_of(file).at(decl);
    auto const& b = d.node.as<ast::binding_decl>();

    cc::string_view const known[] = {"inline"};
    judge_attributes(file, d.attributes, known, "a binding");

    if (ast::is_valid(b.composition))
    {
        unsupported(file, span_of(file, b.composition), "a binding composition");
        out.symbols[index_of(id)].state = symbol_state::failed;
        return;
    }

    auto const members = compile_members(file, b.members, false);
    auto const is_inline = find_attribute(file, d.attributes, "inline") != nullptr;
    // CHK-200: an `@inline` binding holds constants only, so a static sampler in one has nowhere to go.
    if (is_inline)
        for (auto const member : ast_of(file).at(b.members))
            if (auto const* const smp = ast_of(file).at(member).node.try_as<ast::sampler_decl>())
                report(diagnostic_kind::wrong_kind_of_name, file, smp->name,
                       "an @inline binding holds constants only, and a sampler is none");
    out.symbols[index_of(id)].info = i32(out.bindings.size());
    out.bindings.push_back({
        .symbol = id,
        .is_inline = is_inline,
        .members = members,
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
    cc::string_view const known[] = {"builtin", "pure", "operator", "vertex", "pixel", "compute", "stages"};
    judge_attributes(file, d.attributes, known, "a function",
                     is_raster_entry ? setting_scope::description : setting_scope::none);

    if (!f.type_parameters.empty())
    {
        unsupported(file, f.name, "a generic function");
        is_failed = true;
    }
    if (f.receiver != ast::receiver_kind::none)
    {
        unsupported(file, f.name, "a function that takes self");
        is_failed = true;
    }

    auto const is_builtin = find_attribute(file, d.attributes, "builtin") != nullptr;
    auto parameters = cc::vector<parameter>();
    for (auto const& p : ast.at(f.parameters))
    {
        auto const name = text_of(file, p.name);
        cc::string_view const known_on_parameter[] = {"thread_id"};
        judge_attributes(file, p.attributes, known_on_parameter, "a parameter");
        if (p.is_mut)
            unsupported(file, p.name, "a mut parameter");
        if (ast::is_valid(p.default_value))
        {
            unsupported(file, span_of(file, p.default_value), "a default argument");
            is_failed = true;
        }

        for (auto const& other : parameters)
            if (other.name == name && name != "_")
            {
                report(diagnostic_kind::duplicate_declaration, file, p.name, name);
                is_failed = true;
            }

        // CHK-201: a builtin alone may take a resource, and its parameter is then a pattern of one (CHK-202).
        auto type = checked_module::error_type;
        if (ast::is_valid(p.type))
            type = is_builtin ? resolve_pattern_type(file, p.type) : resolve_value_type(file, p.type);
        else if (f.receiver == ast::receiver_kind::none || &p != &ast.at(f.parameters).front())
            report(diagnostic_kind::missing_type, file, span_of(file, p.form), name);
        is_failed = is_failed || type == checked_module::error_type;

        auto const index = isize(&p - ast.fields.data());
        parameters.push_back({.name = name,
                              .type = type,
                              .field = ast::field_id(index),
                              .is_thread_id = find_attribute(file, p.attributes, "thread_id") != nullptr});
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
        auto const* const found = names.get_ptr(text);
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

    // Without `-> T` a block body returns nothing, and an arrow body returns what its expression is.
    auto result = checked_module::nothing_type;
    auto const infers_result = !ast::is_valid(f.return_type) && f.body.kind == ast::body_kind::arrow;
    if (ast::is_valid(f.return_type))
        result = resolve_value_type(file, f.return_type);
    else if (infers_result)
        result = checked_module::error_type;
    is_failed = is_failed || (result == checked_module::error_type && !infers_result);

    auto const has_body = f.body.kind != ast::body_kind::none;
    if (find_attribute(file, d.attributes, "builtin") != nullptr)
    {
        // The record is the overload: the name and the parameter types together, as the registry read them from its own text.
        auto types = cc::vector<cc::string_view>();
        auto is_silent = false;
        for (auto const& p : parameters)
        {
            is_silent = is_silent || p.type == checked_module::error_type;
            types.push_back(out.name_of(p.type));
        }
        auto const intrinsic = builtins.find_function(out.at(id).name, types);
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

    if (info.entry_stage == stage::compute)
    {
        // A compute entry point is dispatched over a grid and hands nothing back.
        if (info.result != checked_module::nothing_type)
            invalid("a @compute fun returns nothing");

        // Its one parameter is the thread id itself, or a struct whose fields are system values.
        if (parameters.size() != 1)
            invalid("a @compute fun takes one parameter: the thread id, or a struct of system values");
        else if (parameters[0].is_thread_id)
        {
            if (!is_int3(parameters[0].type))
                invalid("a @thread_id parameter is an int3");
        }
        else
        {
            auto ids = 0;
            for (auto const& m : out.at(out.at(parameters[0].type).members))
                if (m.is_thread_id)
                {
                    ++ids;
                    if (!is_int3(m.type))
                        invalid("a @thread_id field is an int3");
                }
            if (out.at(parameters[0].type).kind != type_kind::structure || out.at(parameters[0].type).is_opaque)
                invalid("the parameter of a @compute fun is a struct with fields, or carries @thread_id itself");
            else if (ids != 1)
                invalid("the struct of a @compute fun has exactly one @thread_id field");
        }

        notes[s.info].is_valid_entry = is_valid;
        return;
    }

    if (parameters.size() != 1)
        invalid("an entry point takes one struct parameter");
    else if (info.entry_stage == stage::vertex && out.at(parameters[0].type).edge != stage::vertex)
        invalid("the parameter of a @vertex fun is a @vertex struct");
    else if (out.at(parameters[0].type).is_opaque)
        invalid("the parameter of an entry point is a struct with fields");

    if (info.entry_stage == stage::pixel)
    {
        if (result.edge != stage::pixel)
            invalid("a @pixel fun returns a @pixel struct");
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
    }

    notes[s.info].is_valid_entry = is_valid;
}
