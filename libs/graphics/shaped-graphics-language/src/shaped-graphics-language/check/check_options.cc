#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// Options (the spec's checking file, CHK-353 to CHK-355).
//
// An option is a const whose value the compile may set, so the value is substituted where the const is compiled and
// every reader of a const reads the compile's value without knowing it is one.

namespace
{
/// The struct declaration `type` holds its values in, through arrays, buffers, atomics and streams; `none` for any other.
symbol_id struct_of(checked_module const& m, type_id type)
{
    while (is_valid(type))
    {
        auto const& t = m.at(type);
        if (t.kind == type_kind::structure)
            return t.symbol;
        if (t.kind != type_kind::array && t.kind != type_kind::buffer && t.kind != type_kind::atomic
            && t.kind != type_kind::stream)
            return symbol_id::none;
        type = t.element;
    }
    return symbol_id::none;
}
} // namespace

void checker::apply_option_value(symbol_id id, constant_info& info)
{
    auto const& s = out.at(id);
    auto const name = out.qualified_name_of(id);
    // a name given twice is judge_option_values' to report, and the first value stands meanwhile
    for (auto const& given : options)
    {
        if (given.name != name)
            continue;
        auto const where = ast_of(s.file).at(s.declaration).node.as<ast::const_decl>().name;
        auto const refuse = [&]
        {
            report(diagnostic_kind::invalid_option, s.file, where,
                   cc::format("the value '{}' given for the option {} is no {}", given.value, name,
                              out.name_of(info.type)));
        };
        auto text = cc::string_view(given.value);
        if (info.kind == constant_kind::integer)
        {
            // the sign belongs to the value, so the most negative int is one too
            auto const digits = text.starts_with('-') ? text.subview(1) : text;
            auto const value = !digits.starts_with('-') && !digits.starts_with('+')
                                    && classify_number(digits) == number_class::plain_integer
                                 ? parse_plain_integer(text)
                                 : cc::optional<i32>();
            if (!value.has_value())
                return refuse();
            info.integer = value.value();
            return;
        }
        // an enum case, `bool`'s two values among them, written with or without its leading dot
        if (text.starts_with('.'))
            text = text.subview(1);
        auto const cases = out.at(out.at(info.type).cases);
        for (auto i = isize(0); i < cases.size(); ++i)
            if (cases[i].name == text)
            {
                info.case_index = i32(i);
                return;
            }
        return refuse();
    }
}

void checker::judge_option_values()
{
    for (auto i = isize(0); i < options.size(); ++i)
    {
        auto const& given = options[i];
        auto earlier = 0;
        for (auto j = isize(0); j < i; ++j)
            earlier += options[j].name == given.name ? 1 : 0;
        // a name given twice is reported once, at its second value
        if (earlier == 1)
            report(diagnostic_kind::invalid_option, program_file(), source_span{},
                   cc::format("a value is given for {} more than once", given.name));
        if (earlier > 0)
            continue;

        // An option of the program's module is set by its own name, and one of a module it uses by `module.name`.
        auto is_option = false;
        auto qualified = cc::string();
        for (auto s = isize(0); s < out.symbols.size(); ++s)
        {
            auto const& symbol = out.symbols[s];
            if (is_prelude_file(symbol.file) || symbol.kind != symbol_kind::constant || !ast::is_valid(symbol.declaration)
                || find_attribute(symbol.file, ast_of(symbol.file).at(symbol.declaration).attributes, "option") == nullptr)
                continue;
            auto const name = out.qualified_name_of(symbol_id(s));
            if (name == given.name)
                is_option = true;
            else if (symbol.name == given.name)
                qualified = name;
        }
        if (is_option)
            continue;
        if (qualified.empty())
            report(diagnostic_kind::invalid_option, program_file(), source_span{},
                   cc::format("a value is given for {}, and no option is set by that name", given.name));
        else
            report(diagnostic_kind::invalid_option, program_file(), source_span{},
                   cc::format("a value is given for {}, and the option of that name is a module's, set as {}",
                              given.name, qualified));
    }
}

void checker::note_option(i32 file, source_span where, constant_info const& c)
{
    if (is_valid(c.option))
        out.option_uses.push_back({.option = c.option, .file = file, .where = where});
}

cc::vector<symbol_id> checker::options_reached(symbol_id function, symbol_id also)
{
    // Every function the tree may inline, whatever a branch on a constant removes, so the set is one for every value.
    auto reached = cc::vector<symbol_id>();
    reached.push_back(function);
    if (is_valid(also))
        reached.push_back(also);
    for (auto i = isize(0); i < reached.size(); ++i)
        for (auto const& call : calls)
        {
            if (call.caller != reached[i])
                continue;
            auto is_known = false;
            for (auto const r : reached)
                is_known = is_known || r == call.callee;
            if (!is_known)
                reached.push_back(call.callee);
        }

    struct extent
    {
        i32 file = 0;
        source_span span;
    };
    auto extents = cc::vector<extent>();
    auto declarations = cc::vector<symbol_id>();
    auto const add = [&](symbol_id id)
    {
        auto const& s = out.at(id);
        if (!ast::is_valid(s.declaration))
            return;
        for (auto const known : declarations)
            if (known == id)
                return;
        declarations.push_back(id);
        // a declaration's attributes stand in front of its form, and a workgroup size among them may name an option
        auto span = span_of(s.file, s.declaration);
        auto first = span.offset;
        for (auto const& a : ast_of(s.file).at(ast_of(s.file).at(s.declaration).attributes))
            first = cc::min(first, a.name.offset);
        extents.push_back({.file = s.file, .span = {.offset = first, .length = span.end() - first}});
    };
    for (auto const id : reached)
        add(id);
    for (auto const binding : out.at(out.functions[out.at(function).info].bindings))
        add(binding);

    // A struct a signature, a local or a member names shapes the text where its own fields name an option, such as an
    // array's length, so every struct an extent names is reached too, and every struct its fields name in turn.
    for (auto e = isize(0); e < extents.size(); ++e)
    {
        auto const file = extents[e].file;
        auto const span = extents[e].span;
        auto const& tables = out.files[file];
        for (auto x = isize(0); x < tables.type_of.size(); ++x)
        {
            auto const where = span_of(file, ast::expr_id(x));
            if (where.offset < span.offset || where.end() > span.end())
                continue;
            if (auto const s = struct_of(out, tables.type_of[x]); is_valid(s))
                add(s);
        }
    }

    auto result = cc::vector<symbol_id>();
    for (auto const& use : out.option_uses)
    {
        auto is_inside = false;
        for (auto const& e : extents)
            is_inside = is_inside
                     || (e.file == use.file && use.where.offset >= e.span.offset && use.where.end() <= e.span.end());
        auto is_known = false;
        for (auto const r : result)
            is_known = is_known || r == use.option;
        if (is_inside && !is_known)
            result.push_back(use.option);
    }
    cc::sort(result);
    return result;
}
