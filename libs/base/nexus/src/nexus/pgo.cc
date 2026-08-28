#include "pgo.hh"

#include <clean-core/record/stat.hh>
#include <nexus/bench/units.hh>
#include <nexus/tests/execute.hh>

void nx::pgo::report(cc::string_view name, double value, cc::rec::unit const& unit)
{
    nx::impl::record_metric(name, value, unit);
}

void nx::pgo::report_elements_per_sec(cc::string_view name, double value)
{
    nx::impl::record_metric(name, value, nx::bench::unit_items_per_second);
}

void nx::pgo::report_time_for(cc::string_view name, double seconds)
{
    nx::impl::record_metric(name, seconds, cc::rec::unit_seconds);
}
