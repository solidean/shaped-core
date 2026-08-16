#include "parse_report.hh"

using namespace cc::primitive_defines;

isize vdoc::parse_report::count_of(diagnostic_kind kind) const
{
    auto count = isize(0);
    for (auto const& d : diagnostics)
        if (d.kind == kind)
            ++count;

    return count;
}

void vdoc::parse_report::drop_for_entities(cc::span<entity_id const> sorted_entities)
{
    if (sorted_entities.empty())
        return;

    auto const names = [&](entity_id entity)
    {
        auto lo = isize(0);
        auto hi = sorted_entities.size();
        while (lo < hi)
        {
            auto const mid = lo + (hi - lo) / 2;
            auto const order = sorted_entities[mid].compare_bytes(entity);
            if (order == 0)
                return true;

            if (order < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        return false;
    };

    // A stable partition, so surviving entries keep the order a reader already saw them in.
    auto kept = isize(0);
    for (isize i = 0; i < diagnostics.size(); ++i)
        if (!names(diagnostics[i].path.entity))
            diagnostics[kept++] = diagnostics[i];
    while (diagnostics.size() > kept)
        diagnostics.remove_back();

    kept = 0;
    for (isize i = 0; i < agreed_multi_values.size(); ++i)
        if (!names(agreed_multi_values[i].path.entity))
            agreed_multi_values[kept++] = agreed_multi_values[i];
    while (agreed_multi_values.size() > kept)
        agreed_multi_values.remove_back();
}

void vdoc::parse_report::clear()
{
    diagnostics.clear();
    agreed_multi_values.clear();
}
