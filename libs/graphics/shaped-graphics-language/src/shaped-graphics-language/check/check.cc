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

cc::optional<i32> impl::parse_plain_integer(cc::string_view text)
{
    auto plain = cc::string();
    for (auto const c : text)
        if (c != '\'' && c != '+')
            plain += c;
    auto const value = cc::from_string<i64>(plain);
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

void checker::report(diagnostic_kind kind, i32 file, source_span where, cc::string detail)
{
    out.diagnostics.push_back({
        .what = {.kind = kind, .level = default_severity_of(kind), .where = where},
        .file = file,
        .detail = cc::move(detail),
    });
}

void checker::unsupported(i32 file, source_span where, cc::string_view construct)
{
    report(diagnostic_kind::unsupported_yet, file, where, construct);
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
        else if (sgl::is_valid(a.list) && name != "operator" && name != "compute" && name != "stream" && name != "stages")
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
        });
    }

    for (auto file = i32(0); file < i32(files.size()); ++file)
        declare_file(file);
    merge_scopes();

    // Source order is only the order of the first demand: whatever a symbol needs is compiled from inside it.
    for (auto i = isize(0); i < out.symbols.size(); ++i)
        if (out.symbols[i].state == symbol_state::untouched)
            compile(symbol_id(i));

    // A call needs a signature only, so each body is checked once, after every signature is known.
    // That is what lets a function call one declared below it.
    // The exception checked its body already: an arrow body without `-> T`, whose signature is not known before.
    for (auto i = isize(0); i < out.symbols.size(); ++i)
        if (out.symbols[i].kind == symbol_kind::function && out.symbols[i].state == symbol_state::checked)
            check_body(symbol_id(i));

    find_recursion();

    for (auto i = isize(0); i < out.symbols.size(); ++i)
        if (out.symbols[i].kind == symbol_kind::function && out.symbols[i].state == symbol_state::checked)
            flatten_entry_point(symbol_id(i));
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
    auto const name = s.name;
    auto const spelling = s.operator_spelling;
    out.symbols.push_back(cc::move(s));

    if (!spelling.empty())
    {
        operators[spelling].push_back(id);
        return;
    }

    // CHK-12 holds within one scope; the user file's may shadow the prelude's.
    auto& declared = is_prelude_file(file) ? prelude_names[name] : file_names[name];
    if (!declared.empty() && !(is_function && is_all_functions(declared)))
    {
        // The later declaration is compiled like any other and no lookup finds it.
        report(diagnostic_kind::duplicate_declaration, file, name_where, name);
        return;
    }
    declared.push_back(id);
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
        // Two overload sets are one; anything else of the user file hides what the prelude has of that name.
        if (!is_all_functions(seen) || !is_all_functions(ids))
            seen.clear();
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
            if (!s.name.empty())
                add_symbol(named(symbol_kind::structure, s.name), s.name);
        },
        [&](ast::binding_decl const& b)
        {
            if (!b.name.empty())
                add_symbol(named(symbol_kind::binding, b.name), b.name);
        },
        // one unnamed module: the line is accepted and names nothing
        [&](ast::module_decl const&) {}, //
        [&](ast::use_decl const&) { unsupported(file, span_of(file, decl), "use"); },
        [&](ast::enum_decl const& e)
        {
            if (!e.name.empty())
                add_symbol(named(symbol_kind::enumeration, e.name), e.name);
        },
        [&](ast::type_decl const& t) { unsupported_symbol(t.name, "type alias"); },
        [&](ast::const_decl const& c) { unsupported_symbol(c.name, "const"); },
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
        // A member line at module level and an `invalid` declaration were reported by the AST pass.
        [&](ast::field_decl const&) {}, //
        [&](ast::property_decl const&) {}, [&](ast::enum_case_decl const&) {}, [&](ast::invalid_decl const&) {});
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
        compile_function(id);
        break;
    case symbol_kind::pipeline:
        compile_pipeline(id);
        break;
    case symbol_kind::unsupported:
        out.symbols[index_of(id)].state = symbol_state::failed;
        break;
    }

    compiling.remove_back();
    if (out.at(id).state == symbol_state::in_compilation)
        out.symbols[index_of(id)].state = symbol_state::checked;
}
