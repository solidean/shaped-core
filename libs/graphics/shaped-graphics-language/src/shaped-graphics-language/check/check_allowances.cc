#include <clean-core/error/optional.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// An entry point's `@allow` judged across the compiles the file can have (the spec's checking file, CHK-576, CHK-577).
//
// A warning stands in the inlined body of one compile, which a branch on the target or an option may leave out.
// So an allowance this compile does not meet is met where another compile raises its warning: the file checked again
// for each target and each value of the options its entry point reaches, until one does or none is left.

namespace
{
/// Whether `m` judged a warning of `x`'s kind of `x`'s entry point, as `test::contain_expected` takes it.
bool raises(checked_module const& m, entry_allowance const& x)
{
    for (auto const& d : m.diagnostics)
        if (d.what.level == severity::warning && to_string(d.what.kind) == x.kind
            && d.entry_points.values().contains(x.function))
            return true;
    return false;
}

bool is_built(checked_module const& m, symbol_id function)
{
    return m.entry_points.values().any([&](flat_entry_point const& e) { return e.function == function; });
}

/// One option an allowing entry point reaches, and the values it is checked at, the compile's own first: as the
/// compile gave it, or unset where it gave none, so a layer that serves the compile serves that value too.
struct option_axis
{
    cc::string name;
    cc::vector<cc::optional<cc::string>> values;
};

/// The values `id` is tried at: every value of a `bool` or of an enum the program declares, and otherwise the
/// compile's value and the default, since a number or an image format has more values than a check could try.
option_axis axis_of(checked_module const& m, symbol_id id, cc::span<option_value const> options)
{
    auto axis = option_axis{.name = m.qualified_name_of(id)};
    auto given = cc::optional<cc::string>();
    for (auto const& o : options)
        if (o.name == axis.name)
        {
            given = o.value;
            break;
        }
    axis.values.push_back(given);
    auto const& c = m.constants[m.at(id).info];
    if (c.kind == constant_kind::enum_case)
    {
        auto const& type = m.at(c.type);
        auto const is_bool = m.name_of(c.type) == "bool";
        auto const is_own_enum = is_valid(type.symbol) && m.at(type.symbol).file >= m.prelude_file_count();
        if (is_bool || is_own_enum)
        {
            // every case but the one this compile holds, which the first value stands for
            auto const cases = m.at(type.cases);
            for (auto i = isize(0); i < cases.size(); ++i)
                if (i != c.case_index)
                    axis.values.push_back(cc::string(cases[i].name));
            return axis;
        }
    }
    if (given.has_value())
        axis.values.push_back(cc::optional<cc::string>());
    return axis;
}

/// The allowance of `other` that is `x` of `m`: of the same entry point, by the name a compile names it by, written at
/// the same place, of the same kind; positions of files differ between a check from a layer and one from the files.
entry_allowance const* same_allowance(checked_module const& m, entry_allowance const& x, checked_module const& other)
{
    for (auto const& y : other.entry_allowances)
        if (y.where == x.where && y.kind == x.kind
            && other.qualified_name_of(y.function) == m.qualified_name_of(x.function))
            return &y;
    return nullptr;
}
} // namespace

cc::vector<compile_target> sgl::check::every_compile_target(feature_set device_features)
{
    // no target, as `describe` checks, and the targets `emit::all_targets` writes for
    return {
        {.device_features = device_features},
        {.api = graphics_api::dx12, .language = shading_language::hlsl, .device_features = device_features},
        {.api = graphics_api::vulkan, .language = shading_language::hlsl, .device_features = device_features},
        {.api = graphics_api::webgpu, .language = shading_language::wgsl, .device_features = device_features},
        {.api = graphics_api::metal, .language = shading_language::msl, .device_features = device_features},
    };
}

void sgl::check::impl::judge_allowances_elsewhere(
    checked_module& m,
    cc::span<option_value const> options,
    compile_target const& target,
    cc::function_ref<checked_module(cc::span<option_value const>, compile_target const&)> recheck)
{
    // CHK-577: only an allowance of a built entry point is ever unmet, and only one this compile does not meet is judged
    auto unmet = cc::vector<isize>();
    for (auto i = isize(0); i < m.entry_allowances.size(); ++i)
        if (auto const& x = m.entry_allowances[i]; is_built(m, x.function) && !raises(m, x))
            unmet.push_back(i);
    if (unmet.empty())
        return;

    // each allowing entry point on its own, since what it reaches is all that changes its body (CHK-355)
    auto const targets = every_compile_target(target.device_features);
    while (!unmet.empty())
    {
        auto const function = m.entry_allowances[unmet[0]].function;
        auto axes = cc::vector<option_axis>();
        for (auto const& e : m.entry_points)
            if (e.function == function)
                for (auto const id : e.options)
                    if (!axes.values().any([&](option_axis const& a) { return a.name == m.qualified_name_of(id); }))
                        axes.push_back(axis_of(m, id, options));
        auto const is_open
            = [&] { return unmet.values().any([&](isize i) { return m.entry_allowances[i].function == function; }); };

        // every combination of the axes' values, the first axis turning fastest; each with every target
        auto at = cc::vector<isize>();
        at.resize_to_filled(axes.size(), 0);
        auto is_done = false;
        while (!is_done && is_open())
        {
            auto given = cc::vector<option_value>();
            for (auto const& o : options)
                if (!axes.values().any([&](option_axis const& a) { return a.name == o.name; }))
                    given.push_back(o);
            auto is_this_compile = true;
            for (auto a = isize(0); a < axes.size(); ++a)
            {
                if (auto const& value = axes[a].values[at[a]]; value.has_value())
                    given.push_back({.name = axes[a].name, .value = value.value()});
                is_this_compile = is_this_compile && at[a] == 0;
            }
            for (auto const& t : targets)
            {
                if (is_this_compile && t == target)
                    continue;
                auto const other = recheck(given, t);
                // a recheck for one entry point's options may meet another's allowances too
                unmet.remove_all_where(
                    [&](isize i)
                    {
                        auto& x = m.entry_allowances[i];
                        auto const* const y = same_allowance(m, x, other);
                        if (y == nullptr || !raises(other, *y))
                            return false;
                        x.is_met_elsewhere = true;
                        return true;
                    });
                if (!is_open())
                    break;
            }
            auto a = isize(0);
            while (a < axes.size() && ++at[a] == axes[a].values.size())
                at[a++] = 0;
            is_done = a == axes.size();
        }
        // none met what is left of this entry point's, which stays unmet
        unmet.remove_all_where([&](isize i) { return m.entry_allowances[i].function == function; });
    }
}
