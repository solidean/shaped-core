#include "check.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

checked_module sgl::check::check(cc::span<module_file const> prelude, module_file user, builtins::registry const& builtins)
{
    auto files = cc::vector<module_file>();
    files.push_back_range(prelude);
    files.push_back(user);
    auto c = checker{.files = files, .builtins = builtins};
    c.out.builtins = &builtins;
    c.run();
    return cc::move(c.out);
}

checked_module sgl::check::check(cc::span<module_file const> prelude, module_file user)
{
    return check(prelude, user, builtins::default_registry());
}

// ---- number literals ------------------------------------------------------------------------------------------------

number_class impl::classify_number(cc::string_view text)
{
    auto const is_digit = [](char c) { return c >= '0' && c <= '9'; };
    auto at = isize(0);
    auto const size = text.size();
    auto const digits = [&]
    {
        auto const start = at;
        while (at < size && (is_digit(text[at]) || text[at] == '\''))
            ++at;
        return at > start;
    };

    if (at < size && (text[at] == '-' || text[at] == '+'))
        ++at;
    if (!digits())
        return number_class::other;
    if (at == size)
        return number_class::plain_integer;

    auto is_float = false;
    if (text[at] == '.')
    {
        is_float = true;
        ++at;
        digits();
    }
    if (at < size && text[at] == 'e')
    {
        is_float = true;
        ++at;
        if (at < size && (text[at] == '-' || text[at] == '+'))
            ++at;
        if (!digits())
            return number_class::other;
    }
    return is_float && at == size ? number_class::plain_float : number_class::other;
}

cc::optional<f64> impl::parse_plain_float(cc::string_view text)
{
    auto plain = cc::string();
    for (auto const c : text)
        if (c != '\'' && c != '+')
            plain += c;
    // an exponent's `+` went with the rest, and `1e+6` is `1e6`
    return cc::from_string<f64>(plain);
}

cc::optional<i64> impl::parse_literal_integer(cc::string_view text)
{
    auto plain = cc::string();
    for (auto const c : text)
        if (c != '\'' && c != '+')
            plain += c;
    return cc::from_string<i64>(plain);
}

cc::optional<i32> impl::parse_plain_integer(cc::string_view text)
{
    auto const value = parse_literal_integer(text);
    if (!value.has_value() || value.value() < -2147483647 - 1 || value.value() > 2147483647)
        return cc::nullopt;
    return i32(value.value());
}

// ---- shared helpers -------------------------------------------------------------------------------------------------

source_span checker::span_of(i32 file, form_id form) const
{
    return sgl::is_valid(form) ? file_of(file).at(form).where : source_span{};
}

source_span checker::span_of(i32 file, ast::expr_id expr) const
{
    return ast::is_valid(expr) ? span_of(file, ast_of(file).at(expr).form) : source_span{};
}

source_span checker::span_of(i32 file, ast::decl_id decl) const
{
    return ast::is_valid(decl) ? span_of(file, ast_of(file).at(decl).form) : source_span{};
}

source_span checker::span_of(i32 file, ast::stmt_id stmt) const
{
    return ast::is_valid(stmt) ? span_of(file, ast_of(file).at(stmt).form) : source_span{};
}

located_diagnostic& checker::report(diagnostic_kind kind, i32 file, source_span where, cc::string detail)
{
    out.diagnostics.push_back({
        .what = {.kind = kind, .level = default_severity_of(kind), .where = where},
        .file = file,
        .detail = cc::move(detail),
    });
    return out.diagnostics.back();
}

void checker::unsupported(i32 file, source_span where, cc::string_view construct)
{
    report(diagnostic_kind::unsupported_yet, file, where, construct);
}

void checker::report_once(diagnostic_kind kind, i32 file, source_span where, cc::string_view detail)
{
    for (auto const& d : out.diagnostics)
        if (d.what.kind == kind && d.file == file && d.what.where == where)
            return;
    report(kind, file, where, cc::string(detail));
}

isize checker::error_count() const
{
    auto count = isize(0);
    for (auto const& d : out.diagnostics)
        if (d.what.level != severity::warning)
            ++count;
    return count;
}

ast::attribute const* checker::find_attribute(i32 file, ast::range_of<ast::attribute> range, cc::string_view name) const
{
    for (auto const& a : ast_of(file).at(range))
        if (text_of(file, a.name) == name)
            return &a;
    return nullptr;
}

void checker::judge_attributes(i32 file,
                               ast::range_of<ast::attribute> range,
                               cc::span<cc::string_view const> known,
                               cc::string_view owner,
                               setting_scope scope)
{
    for (auto const& a : ast_of(file).at(range))
    {
        auto const name = text_of(file, a.name);
        auto is_known = false;
        for (auto const k : known)
            is_known = is_known || k == name;

        // A setting's value is judged by each pipeline that reads it.
        if (!is_known && scope != setting_scope::none && is_setting_attribute(name, scope == setting_scope::target))
            continue;
        if (!is_known && scope != setting_scope::none)
        {
            // An attribute has no path to disambiguate with, so the pipeline has to set the field itself.
            auto const paths = setting_attribute_paths(name, scope == setting_scope::target);
            if (paths.size() > 1)
            {
                report(diagnostic_kind::invalid_pipeline, file, a.name,
                       cc::format("@{} names both {} and {}: set it in the pipeline by its whole path", name, paths[0],
                                  paths[1]));
                continue;
            }
        }
        if (!is_known)
            unsupported(file, a.name, cc::format("the attribute @{} on {}", name, owner));
        // CHK-220: its one argument is `false` or `true`, whatever it stands on
        else if (name == "shadowable")
        {
            auto const arguments = ast_of(file).at(a.arguments);
            auto const text = arguments.size() == 1 && ast::is_valid(arguments[0].value) && arguments[0].name.empty()
                                ? text_of(file, span_of(file, arguments[0].value))
                                : cc::string_view();
            if (text != "false" && text != "true")
                report(diagnostic_kind::invalid_attribute_arguments, file, a.name,
                       "@shadowable takes `false` or `true`, as in @shadowable(false)");
        }
        else if (sgl::is_valid(a.list) && name != "operator" && name != "compute" && name != "stream"
                 && name != "stages" && name != "shadowable" && name != "expect")
            report(diagnostic_kind::invalid_attribute_arguments, file, span_of(file, a.list),
                   cc::format("@{} takes no arguments", name));
    }
}

void checker::set_type(i32 file, ast::expr_id expr, type_id type)
{
    if (ast::is_valid(expr))
        out.files[file].type_of[ast::index_of(expr)] = type;
}

void checker::set_target(i32 file, ast::expr_id expr, target where)
{
    if (ast::is_valid(expr))
        out.files[file].target_of[ast::index_of(expr)] = where;
}

// ---- the driver -----------------------------------------------------------------------------------------------------

void checker::run()
{
    out.types.push_back({.kind = type_kind::error});
    out.types.push_back({.kind = type_kind::void_});
    for (auto file = i32(0); file < i32(files.size()); ++file)
    {
        auto const count = ast_of(file).exprs.size();
        out.files.push_back({
            .type_of = cc::vector<type_id>::create_filled(count, type_id::none),
            .target_of = cc::vector<target>::create_filled(count, target{}),
            .call_of = cc::vector<i32>::create_filled(count, -1),
        });
        file_features.push_back({});
    }

    for (auto file = i32(0); file < i32(files.size()); ++file)
        declare_file(file);
    declare_constructors();
    merge_scopes();
    attach_extensions();

    // Source order is only the order of the first demand: whatever a symbol needs is compiled from inside it.
    for (auto i = isize(0); i < out.symbols.size(); ++i)
        if (out.symbols[i].state == symbol_state::untouched)
            compile(symbol_id(i));

    judge_redeclarations();

    // A default is checked where it is declared, once, and a call binds against the signature alone (CHK-243).
    for (auto i = isize(0); i < out.symbols.size(); ++i)
        if (out.symbols[i].kind == symbol_kind::function && out.symbols[i].state == symbol_state::checked)
            check_defaults(symbol_id(i));

    // A call needs a signature only, so each body is checked once, after every signature is known.
    // That is what lets a function call one declared below it.
    // The exception checked its body already: an arrow body without `-> T`, whose signature is not known before.
    for (auto i = isize(0); i < out.symbols.size(); ++i)
        if (out.symbols[i].kind == symbol_kind::function && out.symbols[i].state == symbol_state::checked)
            check_body(symbol_id(i));

    // A test in a body that was never checked, a function whose signature failed or a test inside a test, is found
    // nowhere else, and a test is run or fails: it is never left out (CHK-224).
    add_unregistered_tests();

    // Last, since a test in a function body is found while that body is checked (CHK-224).
    for (auto i = isize(0); i < out.tests.size(); ++i)
        check_test(i32(i));

    judge_wide_literals();
    find_recursion();

    // Every binding and body is checked by now, so what each entry point needs and declares is known.
    for (auto i = isize(0); i < out.symbols.size(); ++i)
        if (out.symbols[i].kind == symbol_kind::function && out.symbols[i].state == symbol_state::checked)
            judge_entry_features(symbol_id(i));
    report_unused_requires();

    for (auto i = isize(0); i < out.symbols.size(); ++i)
        if (out.symbols[i].kind == symbol_kind::function && out.symbols[i].state == symbol_state::checked)
            flatten_entry_point(symbol_id(i));
    for (auto i = isize(0); i < out.tests.size(); ++i)
        flatten_test(i32(i));
}

void checker::declare_file(i32 file)
{
    auto const& ast = ast_of(file);
    for (auto const decl : ast.at(ast.declarations))
        declare(file, decl);
}

void checker::add_symbol(symbol s, source_span name_where)
{
    auto const id = symbol_id(out.symbols.size());
    auto const file = s.file;
    auto const is_function = s.kind == symbol_kind::function;
    s.is_shadowable = !ast::is_valid(s.declaration) || is_shadowable_by(file, ast_of(file).at(s.declaration).attributes);
    auto const name = s.name;
    auto const spelling = s.operator_spelling;
    out.symbols.push_back(cc::move(s));

    if (!spelling.empty())
    {
        operators[spelling].push_back(id);
        return;
    }

    // CHK-12 holds within one scope; the user file's may shadow the prelude's.
    // A struct shares its name with functions, its constructors among them, and stands in front of them (CHK-240).
    auto& declared = is_prelude_file(file) ? prelude_names[name] : file_names[name];
    auto const is_struct = out.at(id).kind == symbol_kind::structure;
    auto const is_allowed
        = declared.empty() || (is_function && is_overload_set(declared)) || (is_struct && is_all_functions(declared));
    if (!is_allowed)
    {
        // The later declaration is compiled like any other and no lookup finds it.
        report(diagnostic_kind::duplicate_declaration, file, name_where, name);
        return;
    }
    if (is_struct)
    {
        declared.insert_at(0, id);
        return;
    }
    declared.push_back(id);
}

void checker::declare_members(symbol_id owner, i32 file, ast::range_of<ast::decl_id> members)
{
    for (auto const member : ast_of(file).at(members))
    {
        auto const& d = ast_of(file).at(member);
        // An extension inside a type's block is meant to extend the type it names, seen only inside that block.
        auto const* const ef = d.node.try_as<ast::fun_decl>();
        auto const* const ep = d.node.try_as<ast::property_decl>();
        if ((ef != nullptr && !ef->extended_type.empty()) || (ep != nullptr && !ep->extended_type.empty()))
        {
            unsupported(file, ef != nullptr ? ef->extended_type : ep->extended_type,
                        "an extension inside a type's block");
            continue;
        }
        if (auto const* const f = d.node.try_as<ast::fun_decl>(); f != nullptr && !f->name.empty())
            add_member({.file = file,
                        .declaration = member,
                        .kind = symbol_kind::function,
                        .name = cc::string(text_of(file, f->name)),
                        .role = f->receiver == ast::receiver_kind::none ? function_role::static_ : function_role::method,
                        .owner = owner},
                       f->name);
        else if (auto const* const p = d.node.try_as<ast::property_decl>(); p != nullptr && !p->name.empty())
            add_member({.file = file,
                        .declaration = member,
                        .kind = symbol_kind::function,
                        .name = cc::string(text_of(file, p->name)),
                        .role = function_role::property,
                        .owner = owner},
                       p->name);
    }
}

cc::string_view checker::member_kind_of(symbol_id owner, cc::string_view name) const
{
    auto const& o = out.at(owner);
    auto const& ast = ast_of(o.file);
    auto const& node = ast.at(o.declaration).node;
    auto const* const s = node.try_as<ast::struct_decl>();
    auto const* const e = node.try_as<ast::enum_decl>();
    auto const members = s != nullptr ? s->members : e != nullptr ? e->members : ast::range_of<ast::decl_id>();
    for (auto const member : ast.at(members))
    {
        auto const& d = ast.at(member).node;
        if (auto const* const f = d.try_as<ast::field_decl>();
            f != nullptr && ast::is_valid(f->field) && text_of(o.file, ast.at(f->field).name) == name)
            return "field";
        if (auto const* const c = d.try_as<ast::enum_case_decl>(); c != nullptr && text_of(o.file, c->name) == name)
            return "case";
    }
    auto const* const scope = type_scopes.get_ptr(i32(index_of(owner)));
    auto const* const found = scope != nullptr ? scope->get_ptr(name) : nullptr;
    if (found == nullptr || found->empty())
        return {};
    return out.at(found->front()).role == function_role::property ? "property" : "function";
}

void checker::add_member(symbol s, source_span name_where)
{
    auto const id = symbol_id(out.symbols.size());
    auto const owner = s.owner;
    auto const file = s.file;
    auto const name = s.name;
    auto const kind = s.role == function_role::property ? cc::string_view("property") : cc::string_view("function");
    // CHK-238: a type scope holds one kind of thing per name, and functions of one name are an overload set
    auto const existing = member_kind_of(owner, name);
    out.symbols.push_back(cc::move(s));
    if (!existing.empty() && (existing != "function" || kind != "function"))
    {
        if (existing == kind)
            report(diagnostic_kind::duplicate_declaration, file, name_where, name);
        else
            report(diagnostic_kind::member_name_clash, file, name_where,
                   cc::format("{} is a {} of {} already", name, existing, out.at(owner).name));
        out.symbols[index_of(id)].state = symbol_state::failed;
        return;
    }
    type_scopes[i32(index_of(owner))][name].push_back(id);
}

void checker::attach_extensions()
{
    for (auto const& pending : pending_extensions)
    {
        auto const file = pending.file;
        auto const& d = ast_of(file).at(pending.declaration).node;
        auto const* const f = d.try_as<ast::fun_decl>();
        auto const* const p = d.try_as<ast::property_decl>();
        auto const extended = f != nullptr ? f->extended_type : p->extended_type;
        auto const name_where = f != nullptr ? f->name : p->name;
        auto const type_name = text_of(file, extended);

        // CHK-237: `T` names a struct or an enum
        auto const* const found = names_seen_from(file).get_ptr(type_name);
        if (found == nullptr || found->empty())
        {
            report(diagnostic_kind::unknown_name, file, extended, type_name);
            continue;
        }
        auto const owner = found->front();
        auto const owner_kind = out.at(owner).kind;
        if (owner_kind != symbol_kind::structure && owner_kind != symbol_kind::enumeration)
        {
            report(diagnostic_kind::wrong_kind_of_name, file, extended,
                   cc::format("{} is no struct and no enum, and only a type has functions of its own", type_name));
            continue;
        }
        auto const role = p != nullptr                            ? function_role::property
                        : f->receiver == ast::receiver_kind::none ? function_role::static_
                                                                  : function_role::method;
        add_member({.file = file,
                    .declaration = pending.declaration,
                    .kind = symbol_kind::function,
                    .name = cc::string(text_of(file, name_where)),
                    .role = role,
                    .owner = owner},
                   name_where);
    }
}

cc::vector<symbol_id> checker::candidates_of(i32 file, cc::string_view name, type_id first) const
{
    auto result = cc::vector<symbol_id>();
    if (auto const* const found = names_seen_from(file).get_ptr(name))
        for (auto const id : *found)
            if (out.at(id).kind == symbol_kind::function)
                result.push_back(id);

    // CHK-247: the type scope of the first argument's type, extensions visible from `file` among it, and the functions
    // of the name visible where that type is declared.
    if (!is_valid(first) || index_of(first) >= out.types.size())
        return result;
    auto const& type = out.at(first);
    if ((type.kind != type_kind::structure && type.kind != type_kind::enumeration) || !is_valid(type.symbol))
        return result;
    auto const add = [&](symbol_id id)
    {
        auto is_known = false;
        for (auto const r : result)
            is_known = is_known || r == id;
        if (!is_known)
            result.push_back(id);
    };
    auto const* const scope = type_scopes.get_ptr(i32(index_of(type.symbol)));
    auto const* const members = scope != nullptr ? scope->get_ptr(name) : nullptr;
    if (members != nullptr)
        for (auto const id : *members)
            if (is_visible_from(file, id))
                add(id);
    // The program's file sees its own declarations already, so only a prelude type has a declaring scope to add, and
    // it matters where the program shadows the name with something that is no function (CHK-188).
    if (is_prelude_file(out.at(type.symbol).file))
        if (auto const* const declared = prelude_names.get_ptr(name))
            for (auto const id : *declared)
                if (out.at(id).kind == symbol_kind::function)
                    add(id);
    return result;
}

void checker::judge_redeclarations()
{
    auto const judge = [&](cc::span<symbol_id const> set)
    {
        for (auto i = isize(0); i < set.size(); ++i)
            for (auto j = isize(0); j < i; ++j)
            {
                auto const& a = out.at(set[j]);
                auto const& b = out.at(set[i]);
                if (a.kind != symbol_kind::function || b.kind != symbol_kind::function || a.info < 0 || b.info < 0
                    || a.state != symbol_state::checked || b.state != symbol_state::checked)
                    continue;
                auto const pa = out.at(out.functions[a.info].parameters);
                auto const pb = out.at(out.functions[b.info].parameters);
                auto is_same = pa.size() == pb.size();
                for (auto k = isize(0); is_same && k < pa.size(); ++k)
                    is_same = pa[k].type == pb[k].type && pa[k].name == pb[k].name
                           && pa[k].is_named_only == pb[k].is_named_only;
                if (!is_same)
                    continue;
                // the later one: a synthesized constructor is the struct's, which stands before any function of it
                auto const later = b.role == function_role::constructor ? set[j] : set[i];
                auto const& l = out.at(later);
                auto const& decl = ast_of(l.file).at(l.declaration).node;
                auto where = span_of(l.file, l.declaration);
                if (auto const* const f = decl.try_as<ast::fun_decl>())
                    where = f->name;
                report(diagnostic_kind::duplicate_declaration, l.file, where,
                       cc::format("{} has these parameters already", l.name));
                // No call could choose between the two, so the later one is out of every lookup.
                out.symbols[index_of(later)].state = symbol_state::failed;
            }
    };
    for (auto const& [name, ids] : prelude_names)
        judge(ids);
    for (auto const& [name, ids] : file_names)
        judge(ids);
    for (auto const& [owner, scope] : type_scopes)
        for (auto const& [name, ids] : scope)
            judge(ids);
}

bool checker::is_overload_set(cc::span<symbol_id const> ids) const
{
    for (auto i = isize(0); i < ids.size(); ++i)
    {
        auto const kind = out.at(ids[i]).kind;
        if (kind != symbol_kind::function && !(i == 0 && kind == symbol_kind::structure))
            return false;
    }
    return true;
}

void checker::declare_constructors()
{
    auto const count = out.symbols.size();
    for (auto i = isize(0); i < count; ++i)
    {
        auto const s = out.symbols[i];
        if (s.kind != symbol_kind::structure)
            continue;
        auto const& decl = ast_of(s.file).at(s.declaration).node.as<ast::struct_decl>();
        // A struct nobody can name has no constructor anybody could call, and an opaque one has none (CHK-33).
        if (decl.is_opaque || decl.name.empty())
            continue;
        // Only a struct a lookup finds has a constructor there: a duplicate is found by nothing.
        auto const& scope = is_prelude_file(s.file) ? prelude_names : file_names;
        auto const* const found = scope.get_ptr(s.name);
        if (found == nullptr || found->empty() || found->front() != symbol_id(i))
            continue;
        add_symbol({.file = s.file,
                    .declaration = s.declaration,
                    .kind = symbol_kind::function,
                    .name = s.name,
                    .role = function_role::constructor,
                    .owner = symbol_id(i)},
                   decl.name);
    }
}

bool checker::is_all_functions(cc::span<symbol_id const> ids) const
{
    auto result = true;
    for (auto const id : ids)
        result = result && out.at(id).kind == symbol_kind::function;
    return result;
}

void checker::merge_scopes()
{
    names = prelude_names;
    for (auto const& [name, ids] : file_names)
    {
        auto& seen = names[name];
        // Two overload sets are one, a struct's constructors among them (CHK-189).
        // Anything else of the user file hides what the prelude has of that name.
        if (!is_overload_set(seen) || !is_all_functions(ids))
        {
            // CHK-220: unless the prelude's may not be hidden, which keeps the prelude's and reports the user's.
            auto is_sealed = false;
            for (auto const s : seen)
                is_sealed = is_sealed || !out.at(s).is_shadowable;
            if (is_sealed)
            {
                for (auto const s : ids)
                    report(diagnostic_kind::shadows_unshadowable, out.at(s).file,
                           span_of(out.at(s).file, out.at(s).declaration),
                           cc::format("{} is @shadowable(false) in the prelude", name));
                continue;
            }
            seen.clear();
        }
        seen.push_back_range(ids);
    }
}

void checker::declare(i32 file, ast::decl_id decl)
{
    auto const& d = ast_of(file).at(decl);
    auto const named = [&](symbol_kind kind, source_span name)
    { return symbol{.file = file, .declaration = decl, .kind = kind, .name = text_of(file, name)}; };
    auto const unsupported_symbol = [&](source_span name, cc::string_view construct)
    {
        unsupported(file, span_of(file, decl), construct);
        if (name.empty())
            return;
        auto s = named(symbol_kind::unsupported, name);
        s.state = symbol_state::failed;
        add_symbol(cc::move(s), name);
    };

    d.node.visit(
        [&](ast::fun_decl const& f)
        {
            // a function that lost its name was reported by the AST pass, and nothing can call it
            if (f.name.empty())
                return;
            if (!f.extended_type.empty())
            {
                pending_extensions.push_back({.file = file, .declaration = decl});
                return;
            }
            auto s = named(symbol_kind::function, f.name);
            if (auto const* const a = find_attribute(file, d.attributes, "operator"))
            {
                auto const arguments = ast_of(file).at(a->arguments);
                auto spelling = cc::string_view();
                if (arguments.size() == 1 && arguments[0].name.empty() && !arguments[0].is_splat
                    && ast::is_valid(arguments[0].value))
                {
                    auto const& value = ast_of(file).at(arguments[0].value);
                    auto const* const literal = value.node.try_as<ast::literal>();
                    if (literal != nullptr && literal->kind == ast::literal_kind::quoted)
                        spelling = text_of(file, span_of(file, value.form));
                }
                if (spelling.size() >= 2 && spelling.starts_with('"') && spelling.ends_with('"'))
                    spelling = spelling.subview({.offset = 1, .size = spelling.size() - 2});
                else
                    spelling = {};

                if (spelling.empty())
                {
                    report(diagnostic_kind::invalid_attribute_arguments, file, a->name,
                           "@operator takes one quoted operator, as in @operator(\"*\")");
                    // Neither its name nor an operator finds it: the attribute says its name is hidden.
                    s.state = symbol_state::failed;
                    out.symbols.push_back(cc::move(s));
                    return;
                }
                s.operator_spelling = spelling;
            }
            add_symbol(cc::move(s), f.name);
        },
        [&](ast::struct_decl const& s)
        {
            auto const owner = symbol_id(out.symbols.size());
            if (!s.name.empty())
            {
                add_symbol(named(symbol_kind::structure, s.name), s.name);
                declare_members(owner, file, s.members);
            }
            add_member_tests(file, s.members, cc::format("struct {}", text_of(file, s.name)));
        },
        [&](ast::binding_decl const& b)
        {
            if (!b.name.empty())
                add_symbol(named(symbol_kind::binding, b.name), b.name);
        },
        // one unnamed module: the line is accepted and names nothing
        [&](ast::module_decl const&) {}, //
        [&](ast::use_decl const&) { unsupported(file, span_of(file, decl), "use"); },
        [&](ast::require_decl const& r)
        {
            judge_attributes(file, d.attributes, {}, "a require");
            file_features[file] |= read_require(file, r, require_scope::file, symbol_id::none);
        },
        [&](ast::enum_decl const& e)
        {
            auto const owner = symbol_id(out.symbols.size());
            if (!e.name.empty())
            {
                add_symbol(named(symbol_kind::enumeration, e.name), e.name);
                declare_members(owner, file, e.members);
            }
            add_member_tests(file, e.members, cc::format("enum {}", text_of(file, e.name)));
        },
        [&](ast::type_decl const& t) { unsupported_symbol(t.name, "type alias"); },
        [&](ast::const_decl const& c)
        {
            if (!c.name.empty())
                add_symbol(named(symbol_kind::constant, c.name), c.name);
        },
        [&](ast::sampler_decl const& s) { unsupported_symbol(s.name, "sampler"); },
        [&](ast::pipeline_decl const& p)
        {
            // Without a name it is the file's pipeline, named `pipeline`; a second one is a duplicate like any other.
            auto s = symbol{.file = file, .declaration = decl, .kind = symbol_kind::pipeline, .name = "pipeline"};
            if (!p.name.empty())
                s.name = text_of(file, p.name);
            add_symbol(cc::move(s), p.name.empty() ? span_of(file, decl) : p.name);
        },
        [&](ast::notation_decl const&) { unsupported(file, span_of(file, decl), "notation"); },
        [&](ast::test_decl const&) { add_test(file, decl, ""); },
        // A member line at module level and an `invalid` declaration were reported by the AST pass.
        [&](ast::field_decl const&) {}, //
        [&](ast::property_decl const& p)
        {
            if (!p.extended_type.empty())
                pending_extensions.push_back({.file = file, .declaration = decl});
        },
        [&](ast::enum_case_decl const&) {}, [&](ast::invalid_decl const&) {});
}

symbol_state checker::demand(symbol_id id, i32 file, source_span where)
{
    auto const state = out.at(id).state;
    if (state == symbol_state::untouched)
    {
        compile(id);
        return out.at(id).state;
    }
    if (state != symbol_state::in_compilation)
        return state;

    // The loop starts where the demanded symbol entered compilation and closes on it again.
    auto loop = cc::string();
    auto is_inside = false;
    for (auto const s : compiling)
    {
        is_inside = is_inside || s == id;
        if (is_inside)
            loop.appendf("{} -> ", out.at(s).name);
    }
    loop += out.at(id).name;
    report(diagnostic_kind::dependency_cycle, file, where, cc::move(loop));
    return symbol_state::in_compilation;
}

void checker::compile(symbol_id id)
{
    out.symbols[index_of(id)].state = symbol_state::in_compilation;
    compiling.push_back(id);

    // A symbol demanded from inside a binding's members is no member of it: it sees its own file's features alone.
    auto const outer_granted = granted;
    auto* const outer_used = used_features;
    granted = {};
    used_features = nullptr;

    switch (out.at(id).kind)
    {
    case symbol_kind::structure:
        compile_struct(id);
        break;
    case symbol_kind::enumeration:
        compile_enum(id);
        break;
    case symbol_kind::binding:
        compile_binding(id);
        break;
    case symbol_kind::function:
        if (out.at(id).role == function_role::constructor)
            compile_constructor(id);
        else if (out.at(id).role == function_role::property)
            compile_property(id);
        else
            compile_function(id);
        break;
    case symbol_kind::pipeline:
        compile_pipeline(id);
        break;
    case symbol_kind::constant:
        compile_const(id);
        break;
    case symbol_kind::test:
        // A test's signature is made where it is found, and its body is checked with the others.
        break;
    case symbol_kind::unsupported:
        out.symbols[index_of(id)].state = symbol_state::failed;
        break;
    }

    granted = outer_granted;
    used_features = outer_used;
    compiling.remove_back();
    if (out.at(id).state == symbol_state::in_compilation)
        out.symbols[index_of(id)].state = symbol_state::checked;
}
