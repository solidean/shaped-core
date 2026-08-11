#pragma once

#include <clean-core/string/string.hh>
#include <nexus/fuzz/fwd.hh>

namespace nx::fuzz
{
struct regression_dialect;
} // namespace nx::fuzz

/// Controls how a minimized failing run is rendered as copy-pasteable regression code.
/// The reproducer is wrapped in a SECTION so it drops straight into the TEST that already holds the
/// fuzz setup; the body is emitted as plain `test->eval_op(...)` calls plus the failing assert.
struct nx::fuzz::regression_dialect
{
    cc::string assert_macro = "CHECK";
    cc::string section_open = "SECTION(\"regression\")\n{";
    cc::string section_close = "}";
};

namespace nx::fuzz
{

/// The nexus default: a SECTION holding the replay, a sibling of the SECTION that runs the fuzzer.
[[nodiscard]] inline regression_dialect nexus_section_dialect()
{
    return regression_dialect{};
}
} // namespace nx::fuzz
