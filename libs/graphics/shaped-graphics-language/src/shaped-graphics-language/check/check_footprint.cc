#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/footprint.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// An entry point's footprint, pinned by `@expect(footprint = "...")` (the spec's checking file, "Footprint pins").

namespace
{
constexpr auto k_pin_usage = cc::string_view("on a function, @expect names its entry point's footprint: "
                                             "footprint = \"binding.member: read, binding: read\"");

/// `text` as its `slot: access` items sorted and spaced one way, so a pin compares whatever order it is written in.
cc::string normalized(cc::string_view text)
{
    auto items = cc::vector<cc::string>();
    auto const trimmed = [](cc::string_view s)
    {
        while (!s.empty() && s[0] == ' ')
            s = s.subview({.offset = 1, .size = s.size() - 1});
        while (!s.empty() && s[s.size() - 1] == ' ')
            s = s.subview({.offset = 0, .size = s.size() - 1});
        return s;
    };
    auto start = isize(0);
    for (auto i = isize(0); i <= text.size(); ++i)
    {
        if (i < text.size() && text[i] != ',')
            continue;
        auto const item = trimmed(text.subview({.offset = start, .size = i - start}));
        start = i + 1;
        if (item.empty())
            continue;
        auto colon = isize(-1);
        for (auto j = isize(0); j < item.size() && colon < 0; ++j)
            if (item[j] == ':')
                colon = j;
        if (colon < 0)
            items.push_back(cc::string(item));
        else
            items.push_back(cc::format("{}: {}", trimmed(item.subview({.offset = 0, .size = colon})),
                                       trimmed(item.subview({.offset = colon + 1, .size = item.size() - colon - 1}))));
    }
    // Insertion sort: a pin names a handful of slots.
    for (auto i = isize(1); i < items.size(); ++i)
        for (auto j = i; j > 0 && items[j] < items[j - 1]; --j)
            cc::swap(items[j], items[j - 1]);
    auto joined = cc::string();
    for (auto const& item : items)
        joined += joined.empty() ? item : cc::format(", {}", item);
    return joined;
}
} // namespace

void checker::read_footprint_pin(symbol_id id, i32 file, ast::range_of<ast::attribute> attributes, bool is_entry_point)
{
    auto const& ast = ast_of(file);
    for (auto const& a : ast.at(attributes))
    {
        if (text_of(file, a.name) != "expect")
            continue;
        if (ast.at(a.arguments).empty())
            report(diagnostic_kind::invalid_attribute_arguments, file, a.name, cc::string(k_pin_usage));
        for (auto const& argument : ast.at(a.arguments))
        {
            auto const where = ast::is_valid(argument.value) ? span_of(file, argument.value) : a.name;
            auto const* const literal
                = ast::is_valid(argument.value) ? ast.at(argument.value).node.try_as<ast::literal>() : nullptr;
            auto const name = argument.name.empty() ? cc::string_view() : text_of(file, argument.name);
            if (name != "footprint" || literal == nullptr || literal->kind != ast::literal_kind::quoted)
            {
                report(diagnostic_kind::invalid_attribute_arguments, file, where, cc::string(k_pin_usage));
                continue;
            }
            if (!is_entry_point)
            {
                report(diagnostic_kind::invalid_attribute_arguments, file, where,
                       "only an entry point has a footprint to pin: a `@vertex`, `@pixel` or `@compute` function");
                continue;
            }
            auto const quoted = text_of(file, where);
            footprint_pins.push_back({.function = id,
                                      .file = file,
                                      .expected = cc::string(quoted.subview({.offset = 1, .size = quoted.size() - 2})),
                                      .where = where});
        }
    }
}

void checker::judge_footprint_pins()
{
    for (auto const& pin : footprint_pins)
        for (auto const& e : out.entry_points)
        {
            if (e.function != pin.function)
                continue;
            // The tree an emitter prints, which is what the footprint is defined over.
            auto const footprint = footprint_of(out, legalize(out, e));
            auto const actual = footprint_text(footprint);
            if (normalized(actual) != normalized(pin.expected))
                report(diagnostic_kind::unmet_expectation, pin.file, pin.where,
                       cc::format("the footprint of '{}' is \"{}\"", e.name, actual));
        }
}
