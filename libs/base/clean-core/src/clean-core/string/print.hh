#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

#include <type_traits>

// =========================================================================================================
// cc::print / println (stdout) and cc::eprint / eprintln (stderr).
//
// Each comes in two forms:
//   - a raw string_view, written verbatim (braces are NOT interpreted):
//       cc::print(msg);                 cc::println("done");
//   - a compile-time-checked cc::format string plus arguments:
//       cc::print("{} + {} = {}", 1, 2, 3);   cc::println("{:.2f}", x);
//
// println / eprintln append a trailing '\n' (and the no-argument cc::println() writes just a newline).
// Output goes through std::fwrite to stdout / stderr (see print.cc); no <iostream>, no locale handling.
// =========================================================================================================

namespace cc::impl
{
// raw byte writers to the standard streams (defined in print.cc, the only TU that includes <cstdio>)
void write_stdout(string_view s);
void write_stderr(string_view s);
} // namespace cc::impl

namespace cc
{
// -----------------------------------------------------------------------------------------------------
// stdout
// -----------------------------------------------------------------------------------------------------

/// Writes s to stdout verbatim (no format interpretation).
inline void print(string_view s)
{
    cc::impl::write_stdout(s);
}

/// Writes s followed by '\n' to stdout; cc::println() writes just a newline.
inline void println(string_view s = {})
{
    cc::impl::write_stdout(s);
    cc::impl::write_stdout("\n");
}

/// Formats the arguments with cc::format and writes the result to stdout.
template <class Arg0, class... Args>
void print(format_string<std::type_identity_t<Arg0>, std::type_identity_t<Args>...> fmt, Arg0&& arg0, Args&&... args)
{
    cc::impl::write_stdout(cc::format(fmt, cc::forward<Arg0>(arg0), cc::forward<Args>(args)...));
}

/// Like print, but appends a trailing '\n'.
template <class Arg0, class... Args>
void println(format_string<std::type_identity_t<Arg0>, std::type_identity_t<Args>...> fmt, Arg0&& arg0, Args&&... args)
{
    auto s = cc::format(fmt, cc::forward<Arg0>(arg0), cc::forward<Args>(args)...);
    s.push_back('\n');
    cc::impl::write_stdout(s);
}

// -----------------------------------------------------------------------------------------------------
// stderr
// -----------------------------------------------------------------------------------------------------

/// Writes s to stderr verbatim (no format interpretation).
inline void eprint(string_view s)
{
    cc::impl::write_stderr(s);
}

/// Writes s followed by '\n' to stderr; cc::eprintln() writes just a newline.
inline void eprintln(string_view s = {})
{
    cc::impl::write_stderr(s);
    cc::impl::write_stderr("\n");
}

/// Formats the arguments with cc::format and writes the result to stderr.
template <class Arg0, class... Args>
void eprint(format_string<std::type_identity_t<Arg0>, std::type_identity_t<Args>...> fmt, Arg0&& arg0, Args&&... args)
{
    cc::impl::write_stderr(cc::format(fmt, cc::forward<Arg0>(arg0), cc::forward<Args>(args)...));
}

/// Like eprint, but appends a trailing '\n'.
template <class Arg0, class... Args>
void eprintln(format_string<std::type_identity_t<Arg0>, std::type_identity_t<Args>...> fmt, Arg0&& arg0, Args&&... args)
{
    auto s = cc::format(fmt, cc::forward<Arg0>(arg0), cc::forward<Args>(args)...);
    s.push_back('\n');
    cc::impl::write_stderr(s);
}
} // namespace cc
