#pragma once

#include <clean-core/common/macros.hh>
#include <clean-core/container/span.hh>
#include <clean-core/platform/source_location.hh>
#include <clean-core/record/fwd.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/fwd.hh>

// The log rule: a passing test logs no warning and no error it did not declare.
//
// Every section pass runs under an owner id (cc::rec::owner_scope).
// A record carries it to the test from any thread the test's work reaches, including inside a library's own CC_RECORD_ASYNC_SCOPE.
// The run keeps only warning-and-above records and judges them once, when execute_tests ends, after a single flush.
// So a declaration may follow the line that provokes it, and a verdict is final only at the end of the run.
//
// From narrowest to broadest:
//
//   nx::expect_warning("did not fit the upload ring");             // must appear at least once in this pass
//   nx::expect_error("refusing connection", nx::exactly(1));        // must appear exactly once
//   nx::allow_warnings("waiting on an in-flight stream");          // may appear, any number of times
//   TEST("...", nx::config::allow_logs(cc::rec::level::warning))  // the whole test waives warnings
//   NX_ALLOW_LOGS(cc::rec::level::warning, "sg.dx12", "clear value") // every test in the binary
//
// A pattern is a glob over the formatted message (`*` and `?`, with `/` ordinary), matched anywhere in it.
// So a line pasted out of the console matches itself; the domain is optional and matched exactly.
//
// **A declaration covers the pass it is made in.**
// One made inside a SECTION does not cover a sibling section; one made above the sections covers every pass.
// Records from the `nexus` domain never count, since a failed check already fails.
// A test marked `owns_recorder` is outside the rule: the run's recorder is not up while it runs.

/// How often an expected record must appear; the default is at least once.
struct nx::log_expectation
{
    /// The domain the record must come from, or empty for any.
    cc::string_view domain = {};
    int at_least = 1;
    /// Negative for no upper bound.
    int at_most = -1;
};

namespace nx
{
/// Exactly `n` matching records.
[[nodiscard]] constexpr log_expectation exactly(int n, cc::string_view domain = {})
{
    return {.domain = domain, .at_least = n, .at_most = n};
}

/// A matching warning must be logged in this pass; absent (or outside the count) fails the test.
void expect_warning(cc::string_view pattern,
                    log_expectation const& expectation = {},
                    cc::source_location location = cc::source_location::current());

/// A matching error must be logged in this pass; absent (or outside the count) fails the test.
void expect_error(cc::string_view pattern,
                  log_expectation const& expectation = {},
                  cc::source_location location = cc::source_location::current());

/// Matching warnings may be logged in this pass, any number of times including none.
void allow_warnings(cc::string_view pattern,
                    cc::string_view domain = {},
                    cc::source_location location = cc::source_location::current());

/// Matching errors may be logged in this pass, any number of times including none.
void allow_errors(cc::string_view pattern,
                  cc::string_view domain = {},
                  cc::source_location location = cc::source_location::current());
} // namespace nx

namespace nx::impl
{
/// Registers a binary-wide allowance; NX_ALLOW_LOGS is the spelling.
/// Allows records at or below `level` whose domain matches (empty for any) and whose text matches `pattern`.
/// Must be called before the run starts, which static initialization guarantees.
void register_log_allowance(cc::rec::level level,
                            cc::string_view domain,
                            cc::string_view pattern,
                            cc::source_location location);

/// One allowance per pattern, for a list several binaries share.
void register_log_allowance(cc::rec::level level,
                            cc::string_view domain,
                            cc::span<cc::string_view const> patterns,
                            cc::source_location location);
} // namespace nx::impl

/// Allows matching records in every test of this binary: at or below `level`, from `domain` (empty for any), matching the glob `pattern` anywhere in the message.
/// `pattern_` may also be a list of patterns, which allows each of them.
/// The broadest waiver there is, so it belongs beside the driver of the tests that need it, with the reason above it.
/// Expands to a `static`, so it belongs in a .cc: in a header it registers once per including translation unit.
#define NX_ALLOW_LOGS(level_, domain_, pattern_)                  \
    static bool const CC_MACRO_JOIN(_nx_allow_logs_, __COUNTER__) \
        = (::nx::impl::register_log_allowance((level_), (domain_), (pattern_), ::cc::source_location::current()), true)
