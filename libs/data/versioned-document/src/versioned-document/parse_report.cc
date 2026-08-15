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

void vdoc::parse_report::clear()
{
    diagnostics.clear();
    agreed_multi_values.clear();
}
