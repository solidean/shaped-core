#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

/// Reading and writing process environment variables.
///
/// Environment variables are how a tool asks a program for behavior it has no API for yet — a run that must not steal focus, a run that should write a preview image.
/// That is an escape hatch, and naming the convention in one place is what lets a real API replace it later without hunting for `getenv` calls.
///
/// Reading is what nearly every caller wants.
/// **Writing is process-global and racy against every other thread**, and it is here for one job: exercising a code path that reads a variable, from a test.
/// A caller that wants a CHILD process to see a variable passes it when spawning that child — never by setting it here first.

namespace cc
{
/// The variable's value, or nothing when it is unset or empty.
///
/// Empty reads as absent on purpose: a shell that exports a variable with no value is saying nothing, and every caller
/// so far would otherwise have to special-case it.
[[nodiscard]] cc::optional<cc::string> environment_variable(cc::string_view name);

/// Whether `name` is set to something that means yes.
///
/// Anything but "0", "false", "no" and "off" counts as yes (compared case-insensitively), so `X=1` and a bare `X=yes`
/// both work and `X=0` is a reliable way to say no.
/// Unset and empty are no.
[[nodiscard]] bool is_environment_flag_set(cc::string_view name);

/// Set `name` for this process, and for every child spawned after the call.
///
/// NOT thread-safe, and not against readers either: the whole environment is one process-global table, so a concurrent
/// `environment_variable` may see either value or, on some platforms, a dangling one.
/// Set what a test needs before it starts threads, or hold whatever exclusion the test suite uses.
///
/// An empty `value` REMOVES the variable, because `environment_variable` already reads empty as absent — keeping the two
/// spellings distinct here would only invent a state nothing can observe.
/// `name` must not be empty and must not contain '='.
void set_environment_variable(cc::string_view name, cc::string_view value);

/// Remove `name`, whether or not it was set.
/// Carries the same threading warning as `set_environment_variable`.
void unset_environment_variable(cc::string_view name);
} // namespace cc

/// One environment variable, set for a scope and put back on the way out — to its previous value, or unset when it had none.
///
/// The scoped form is what a test wants, and leaving it to each caller is how a variable escapes its test: an early return
/// or a failed REQUIRE past a bare `set_environment_variable` leaks it into every test that runs afterwards.
///
/// Carries `set_environment_variable`'s threading warning, and adds nothing to it: overlapping scopes on ONE variable
/// restore in destruction order, so nesting works and interleaving does not.
struct cc::scoped_environment_variable
{
    scoped_environment_variable(cc::string_view name, cc::string_view value)
      : _name(name), _previous(cc::environment_variable(name))
    {
        cc::set_environment_variable(name, value);
    }

    ~scoped_environment_variable()
    {
        if (_previous.has_value())
            cc::set_environment_variable(_name, _previous.value());
        else
            cc::unset_environment_variable(_name);
    }

    scoped_environment_variable(scoped_environment_variable const&) = delete;
    scoped_environment_variable& operator=(scoped_environment_variable const&) = delete;
    scoped_environment_variable(scoped_environment_variable&&) = delete;
    scoped_environment_variable& operator=(scoped_environment_variable&&) = delete;

private:
    cc::string _name;
    cc::optional<cc::string> _previous;
};
