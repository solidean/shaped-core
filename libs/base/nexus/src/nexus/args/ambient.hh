#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/args/fwd.hh>
#include <nexus/args/value.hh>

// =========================================================================================================
// What this process was invoked with, answerable from anywhere.
//
// Deliberately light: span, string_view and the value traits, and nothing from the builder.
// This is the header a debug site three libraries down includes, and it must not drag a parser in with it.
//
// `nx::current_args()` is one function with two answers, which is the point — a helper written for a tool
// keeps working when it is called from inside a test.
//
// The captured copy is exact and free.
// The OS fallback exists for a binary that never went through nx::run, and every platform that cannot
// answer yields an empty list rather than an assertion.
// =========================================================================================================

namespace nx
{
/// The arguments in effect here: the running test's when called inside one, otherwise the process's.
/// argv[0] is NOT included — `program_path()` is where that lives.
[[nodiscard]] cc::span<cc::string_view const> current_args();

/// The process's own arguments, ignoring any test scope.
[[nodiscard]] cc::span<cc::string_view const> process_args();

/// argv[0], or empty when the platform cannot say.
[[nodiscard]] cc::string_view program_path();

/// The basename of `program_path()`, without a directory or a trailing `.exe`.
[[nodiscard]] cc::string_view program_name();

// =========================================================================================================
// The undeclared accessors.
//
// A debug convenience, and no more than that: these read a command line NOBODY declared, so they cannot
// know whether `--foo bar` is a flag with a value or a flag followed by a positional.
// They guess, they never fail, and they are the wrong tool for anything a user is expected to rely on.
//
// The grammar they accept:
//   --name=value / -n=value   the value, unambiguously
//   --name value / -n value   the value, when the next token does not itself start with '-'
//   --name / -n               present, with no value
//
// Everything after a bare `--` is ignored, so an argument being passed through to another program cannot
// switch on a debug path in this one.
// =========================================================================================================

/// Whether `name` appears at all, written with or without dashes.
[[nodiscard]] bool has_arg(cc::string_view name);

/// The raw text `name` was given, or nothing.
[[nodiscard]] cc::optional<cc::string_view> get_arg(cc::string_view name);

/// The value of `name`, parsed as `T`, or nothing when it is absent or does not parse.
/// The two are not distinguished on purpose: a debug helper that reports why it failed invites being
/// depended on, and this one should not be.
template <arg_value T>
[[nodiscard]] cc::optional<T> get_arg(cc::string_view name)
{
    auto const text = get_arg(name);
    if (!text.has_value())
        return cc::nullopt;

    auto value = T();
    auto error = cc::string();
    if (!nx::custom::arg_value_trait<T>::parse(text.value(), value, error))
        return cc::nullopt;

    return value;
}

} // namespace nx

namespace nx::impl
{
/// Record this process's argv, once.
/// Called by nx::run.
/// A second call is a no-op rather than an assertion, because a nexus meta-test nests a whole run inside a
/// running one.
void set_process_args(int argc, char const* const* argv);

/// The arguments the running test was given, or empty outside a test.
/// Installed by the scheduler; declared here so ambient.cc can ask without depending on the test machinery.
[[nodiscard]] cc::span<cc::string_view const> current_test_args();

} // namespace nx::impl
