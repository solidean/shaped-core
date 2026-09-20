#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
stage stage_of(bool is_vertex, bool is_pixel)
{
    if (is_vertex)
        return stage::vertex;
    return is_pixel ? stage::pixel : stage::none;
}
} // namespace

// ---- types ----------------------------------------------------------------------------------------------------------

type_id checker::resolve_type(i32 file, ast::expr_id expr)
{
    if (!ast::is_valid(expr))
        return checked_module::error_type;

    auto const& e = ast_of(file).at(expr);
    auto const where = span_of(file, expr);
    auto result = checked_module::error_type;

    if (!e.attributes.empty())
        unsupported(file, where, "an attribute on a type");

    if (auto const* const n = e.node.try_as<ast::name>())
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
            if (kind == symbol_kind::structure)
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
    else if (e.node.is<ast::index>())
        unsupported(file, where, "type arguments");
    else if (e.node.is<ast::struct_type>())
        unsupported(file, where, "an anonymous struct type");
    else if (e.node.is<ast::function_type>())
        unsupported(file, where, "a function type");
    else if (e.node.is<ast::tuple>())
        unsupported(file, where, "a tuple type");
    else if (e.node.is<ast::member>())
        unsupported(file, where, "a qualified type name");
    else if (!e.node.is<ast::invalid_expr>())
        unsupported(file, where, "this expression as a type");

    set_type(file, expr, result);
    return result;
}

type_id checker::type_of_builtin(builtin b, i32 file, source_span where)
{
    auto const name = to_string(b);
    auto const* const found = names.get_ptr(name);
    if (found != nullptr && !found->empty())
    {
        auto const id = found->front();
        if (out.at(id).kind == symbol_kind::structure && demand(id, file, where) == symbol_state::checked
            && out.at(id).intrinsic == b)
            return out.at(id).type;
    }
    report(diagnostic_kind::unknown_name, file, where, cc::format("{}, which the prelude must declare @builtin", name));
    return checked_module::error_type;
}

// ---- structs and bindings -------------------------------------------------------------------------------------------

ast::range_of<member_info> checker::compile_members(i32 file, ast::range_of<ast::decl_id> members, bool is_struct)
{
    auto const& ast = ast_of(file);
    auto const owner = is_struct ? cc::string_view("a struct field") : cc::string_view("a binding member");
    auto collected = cc::vector<member_info>();

    for (auto const member : ast.at(members))
    {
        auto const& d = ast.at(member);
        auto const where = span_of(file, member);

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

        cc::string_view const known_on_field[] = {"position"};
        judge_attributes(file, f.attributes,
                         is_struct ? cc::span<cc::string_view const>(known_on_field) : cc::span<cc::string_view const>(),
                         owner);
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
            type = resolve_type(file, f.type);
        else
            report(diagnostic_kind::missing_type, file, f.name, name);

        collected.push_back({
            .name = name,
            .type = type,
            .field = line->field,
            .is_position = find_attribute(file, f.attributes, "position") != nullptr,
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

    cc::string_view const known[] = {"builtin", "vertex", "pixel"};
    judge_attributes(file, d.attributes, known, "a struct");

    auto const is_builtin = find_attribute(file, d.attributes, "builtin") != nullptr;
    if (is_builtin)
    {
        auto const intrinsic = builtin_of(out.at(id).name);
        if (intrinsic != builtin::none && is_type(intrinsic))
            out.symbols[index_of(id)].intrinsic = intrinsic;
        else
            report(diagnostic_kind::unknown_builtin, file, s.name, out.at(id).name);
    }
    else if (s.is_opaque)
        report(diagnostic_kind::opaque_struct_needs_builtin, file, s.name, out.at(id).name);

    auto const is_vertex = find_attribute(file, d.attributes, "vertex") != nullptr;
    auto const is_pixel = find_attribute(file, d.attributes, "pixel") != nullptr;
    if (is_vertex && is_pixel)
        unsupported(file, s.name, "a struct of two stages");

    auto const members = compile_members(file, s.members, true);

    // The type exists only now, so a field that needs its own struct found a cycle and not a type.
    auto const type = type_id(out.types.size());
    out.types.push_back({
        .kind = type_kind::structure,
        .symbol = id,
        .members = members,
        .is_opaque = s.is_opaque,
        .edge = stage_of(is_vertex, is_pixel),
    });
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
    out.symbols[index_of(id)].info = i32(out.bindings.size());
    out.bindings.push_back({
        .symbol = id,
        .is_inline = find_attribute(file, d.attributes, "inline") != nullptr,
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

    cc::string_view const known[] = {"builtin", "pure", "operator", "vertex", "pixel"};
    judge_attributes(file, d.attributes, known, "a function");

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

    auto parameters = cc::vector<parameter>();
    for (auto const& p : ast.at(f.parameters))
    {
        auto const name = text_of(file, p.name);
        judge_attributes(file, p.attributes, {}, "a parameter");
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

        auto type = checked_module::error_type;
        if (ast::is_valid(p.type))
            type = resolve_type(file, p.type);
        else if (f.receiver == ast::receiver_kind::none || &p != &ast.at(f.parameters).front())
            report(diagnostic_kind::missing_type, file, span_of(file, p.form), name);
        is_failed = is_failed || type == checked_module::error_type;

        auto const index = isize(&p - ast.fields.data());
        parameters.push_back({.name = name, .type = type, .field = ast::field_id(index)});
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

    auto result = checked_module::error_type;
    if (ast::is_valid(f.return_type))
        result = resolve_type(file, f.return_type);
    else
        unsupported(file, f.name, "a function without a written return type");
    is_failed = is_failed || result == checked_module::error_type;

    auto const has_body = f.body.kind != ast::body_kind::none;
    if (find_attribute(file, d.attributes, "builtin") != nullptr)
    {
        auto const intrinsic = builtin_of(out.at(id).name);
        if (intrinsic != builtin::none && !is_type(intrinsic))
            out.symbols[index_of(id)].intrinsic = intrinsic;
        else
        {
            report(diagnostic_kind::unknown_builtin, file, f.name, out.at(id).name);
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

    out.symbols[index_of(id)].info = i32(out.functions.size());
    out.functions.push_back({
        .symbol = id,
        .parameters = {.first = u32(out.parameters.size()), .count = u32(parameters.size())},
        .result = result,
        .bindings = {.first = u32(out.binding_lists.size()), .count = u32(bindings.size())},
        .entry_stage = stage_of(is_vertex, is_pixel),
        .is_pure = find_attribute(file, d.attributes, "pure") != nullptr,
    });
    out.parameters.push_back_range(parameters);
    out.binding_lists.push_back_range(bindings);
    notes.push_back({});

    if (is_vertex && is_pixel)
        report(diagnostic_kind::invalid_entry_point, file, f.name, "an entry point has one stage");
    else if (!is_failed && (is_vertex || is_pixel))
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

    if (s.intrinsic != builtin::none)
        invalid("an entry point is no @builtin");
    if (!s.operator_spelling.empty())
        invalid("an entry point is no @operator");

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
                is_hpos4
                    = is_hpos4 && type.kind == type_kind::structure && out.at(type.symbol).intrinsic == builtin::hpos4;
            }
        if (positions != 1)
            invalid("a @vertex fun returns a struct with exactly one @position field");
        else if (!is_hpos4)
            invalid("the @position field of a @vertex fun is an hpos4");
    }

    notes[s.info].is_valid_entry = is_valid;
}
