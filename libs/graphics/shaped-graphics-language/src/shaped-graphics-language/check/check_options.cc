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

void checker::apply_option_value(symbol_id id, constant_info& info)
{
    auto const& s = out.at(id);
    // only the program's options are the compile's to set
    if (s.file != i32(files.size()) - 1)
        return;
    for (auto const& given : options)
    {
        if (given.name != s.name)
            continue;
        auto const where = ast_of(s.file).at(s.declaration).node.as<ast::const_decl>().name;
        auto const refuse = [&]
        {
            report(diagnostic_kind::invalid_option, s.file, where,
                   cc::format("the value '{}' given for the option {} is no {}", given.value, s.name,
                              out.name_of(info.type)));
        };
        auto text = cc::string_view(given.value);
        if (info.kind == constant_kind::integer)
        {
            auto const is_negative = text.starts_with('-');
            auto const digits = is_negative ? text.subview(1) : text;
            auto const value = !digits.starts_with('-') && !digits.starts_with('+')
                                    && classify_number(digits) == number_class::plain_integer
                                 ? parse_plain_integer(digits)
                                 : cc::optional<i32>();
            if (!value.has_value())
                return refuse();
            info.integer = is_negative ? -value.value() : value.value();
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
    auto const program = i32(files.size()) - 1;
    for (auto const& given : options)
    {
        auto is_option = false;
        for (auto const& s : out.symbols)
            if (s.file == program && s.kind == symbol_kind::constant && s.name == given.name
                && ast::is_valid(s.declaration)
                && find_attribute(s.file, ast_of(s.file).at(s.declaration).attributes, "option") != nullptr)
                is_option = true;
        if (!is_option)
            report(diagnostic_kind::invalid_option, program, source_span{},
                   cc::format("a value is given for {}, and the source has no option of that name", given.name));
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
    auto const add = [&](symbol_id id)
    {
        auto const& s = out.at(id);
        if (!ast::is_valid(s.declaration))
            return;
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
