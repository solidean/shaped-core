#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

/// Reading process environment variables.
///
/// Deliberately read-only: setting one is process-global and racy against every other thread, so a caller that wants
/// a child process to see a variable passes it when spawning that child instead.
///
/// Environment variables are how a tool asks a program for behavior it has no API for yet — a run that must not steal focus, a run that should write a preview image.
/// That is an escape hatch, and naming the convention in one place is what lets a real API replace it later without hunting for `getenv` calls.

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
} // namespace cc
