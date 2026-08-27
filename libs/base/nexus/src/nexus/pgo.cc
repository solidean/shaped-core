#include "pgo.hh"

#include <nexus/tests/execute.hh>

void nx::pgo::report_elements_per_sec(cc::string_view name, double value)
{
    nx::impl::record_metric(name, value, "1/s", true);
}

void nx::pgo::report_time_for(cc::string_view name, double seconds)
{
    nx::impl::record_metric(name, seconds, "s", false);
}

void nx::pgo::report_raw(cc::string_view name, double value, cc::string_view unit, bool higher_is_better)
{
    nx::impl::record_metric(name, value, unit, higher_is_better);
}
