#include "known_issues.hh"

#include <clean-core/common/log.hh>
#include <clean-core/string/format.hh>
#include <nexus/tests/check.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/schedule.hh>

// Inside nx so the line a run that tries a known issue logs carries the nexus domain.
namespace nx
{
bool runs_known_issues()
{
    auto const* const config = impl::current_config();
    return config != nullptr && config->run_known_issues;
}

void skip_known_issue(cc::string_view id, cc::string_view symptom, cc::source_location loc)
{
    if (runs_known_issues())
    {
        CC_LOG_INFO("known issue {} is tried rather than skipped (--known-issues): {}", id, symptom);
        return;
    }
    impl::check_handle::make(impl::check_kind::check, impl::cmp_op::skip, "SKIP", true, loc)
        .succeed_note(cc::format("known issue {}: {}; see docs/bugs-external/{}", id, symptom, id));
}
} // namespace nx
