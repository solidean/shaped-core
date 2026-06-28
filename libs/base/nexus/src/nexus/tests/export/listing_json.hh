#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

namespace nx
{
/// Returns a JSON listing of every registered test for binary pre-selection (see `dev.py test`). Each test
/// reports its name, source location, bucket, enabled/seed config, whether its name passes the filters
/// (`name_matches`), and whether it would actually run under `config` (`eligible`). The top level echoes the
/// active filters/bucket and an `eligible_count`. Unlike the schedule, this lists all tests — including ones
/// excluded by bucket or disabled status — so callers can explain why a name matched but was not run.
cc::string write_test_listing_json(cc::string_view suite_name,
                                   test_schedule_config const& config,
                                   test_registry const& registry);
} // namespace nx
