#include "hot_functions.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/platform/symbolize.hh>
#include <clean-core/record/fwd.hh>
#include <clean-core/string/format.hh>

using namespace cc::primitive_defines;

namespace
{
struct accumulator
{
    cc::string file;
    i32 line = 0;
    isize self = 0;
    isize total = 0;
};

/// What a frame is FILED under.
///
/// A resolved name where there is one, and otherwise the module, because a driver DLL that owns a third of the profile
/// is worth saying even though not one of its addresses has a name.
[[nodiscard]] cc::string bucket_of(cc::symbol_info const& info)
{
    if (info.has_function())
        return info.function;
    if (!info.module.empty())
        return info.module;
    return "<unknown>";
}
} // namespace

cc::rec::hot_function const* cc::rec::hot_report::find(cc::string_view function) const
{
    for (auto const& f : functions)
        if (f.function == function)
            return &f;
    return nullptr;
}

double cc::rec::hot_report::self_ratio_of(cc::string_view function) const
{
    auto const* const f = find(function);
    return f != nullptr ? f->self_ratio : 0.0;
}

cc::string cc::rec::hot_report::to_string(isize max_rows) const
{
    if (functions.empty())
        return cc::format("no stacks in this recording ({} sample(s))", sample_count);

    cc::string out;
    out += cc::format("{} sample(s)", sample_count);
    if (unresolved_samples > 0)
        out += cc::format(", {} in no known module", unresolved_samples);
    out += "\n";
    out += "  self%  total%  function\n";

    auto const rows = max_rows > 0 ? cc::min(max_rows, functions.size()) : functions.size();
    for (isize i = 0; i < rows; ++i)
    {
        auto const& f = functions[i];
        out += cc::format("  {:5.1f}   {:5.1f}  {}", f.self_ratio * 100, f.total_ratio * 100, f.function);
        if (f.line > 0)
            out += cc::format("  ({}:{})", f.file, f.line);
        out += "\n";
    }

    if (rows < functions.size())
        out += cc::format("  ... and {} more\n", functions.size() - rows);

    return out;
}

cc::rec::hot_report cc::rec::hot_functions(rec::recording const& r, rec::hot_options const& opts)
{
    rec::hot_report report;

    // Against the recording's own table where it has one, so a recording that travelled reports the names ITS binaries
    // had rather than whatever this process happens to have loaded at those addresses.
    auto symbols = r.modules().empty() ? cc::symbolizer() : cc::symbolizer(r.modules());

    cc::map<cc::string, accumulator> by_function;

    // Reused across stacks so a deep recursion does not reallocate on every sample.
    cc::set<cc::string> seen;

    auto const fold = [&](cc::span<u64 const> frames)
    {
        if (frames.empty())
            return;

        ++report.sample_count;
        seen.clear();

        for (isize i = 0; i < frames.size(); ++i)
        {
            auto const& info = symbols.resolve(reinterpret_cast<void const*>(frames[i]));
            auto const key = bucket_of(info);

            if (i == 0)
            {
                // The innermost frame is the self time, and it is the only place unresolvedness is worth counting:
                // a stack whose bottom is unknown tells you nothing about where the program actually was.
                if (info.module.empty() && !info.has_function())
                    ++report.unresolved_samples;

                auto& acc = by_function[key];
                ++acc.self;
                if (acc.line == 0 && info.has_line())
                {
                    acc.file = info.file;
                    acc.line = info.line;
                }
            }

            // Once per stack however often it recurs, or a recursive descent would report more than 100%.
            if (!seen.contains(key))
            {
                seen.insert(key);
                ++by_function[key].total;
            }
        }
    };

    r.for_each_event(
        [&](rec::chunk_view const&, rec::event_view const& e)
        {
            auto const is_sample = e.desc->kind == rec::event_kind::sample;
            auto const is_logged_stack
                = opts.include_stacktrace_events && cc::string_view(e.desc->name) == "record.stacktrace";

            if (is_sample || is_logged_stack)
                fold(e.field_as_u64_array("frames"));
        });

    if (report.sample_count == 0)
        return report;

    auto const inv = 1.0 / f64(report.sample_count);
    for (auto const& [name, acc] : by_function)
    {
        auto const self_ratio = f64(acc.self) * inv;
        if (self_ratio < opts.min_self_ratio)
            continue;

        report.functions.push_back({
            .function = name,
            .file = acc.file,
            .line = acc.line,
            .self_samples = acc.self,
            .total_samples = acc.total,
            .self_ratio = self_ratio,
            .total_ratio = f64(acc.total) * inv,
        });
    }

    // By name last, so two functions with identical counts do not swap between runs — a report you can assert on has
    // to order the same way twice.
    cc::sort(report.functions,
             [](rec::hot_function const& a, rec::hot_function const& b)
             {
                 if (a.self_samples != b.self_samples)
                     return a.self_samples > b.self_samples;
                 if (a.total_samples != b.total_samples)
                     return a.total_samples > b.total_samples;
                 return a.function < b.function;
             });

    return report;
}
