#pragma once

#include <clean-core/platform/source_location.hh>
#include <clean-core/string/string_view.hh>

namespace nx
{
/// Whether the run tries the tests a known external bug would skip: `--known-issues` on the binary,
/// `uv run dev.py test --known-issues` from the driver.
/// False outside a running test.
[[nodiscard]] bool runs_known_issues();

/// Skips the running test for the external bug `id`, whose reproduction is `docs/bugs-external/<id>`.
///
/// Under runs_known_issues() it logs that it does not and returns, so the test runs on and a pass says the bug may be fixed.
/// The caller decides whether the bug applies here; sg::known_issue_affecting answers that for a graphics context.
/// The skip reason starts `known issue <id>:`, which is what groups such skips in a run's summary.
void skip_known_issue(cc::string_view id,
                      cc::string_view symptom,
                      cc::source_location loc = cc::source_location::current());
} // namespace nx
