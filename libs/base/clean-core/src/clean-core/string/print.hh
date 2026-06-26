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
// Output goes through std::fwrite to stdout / stderr (in print.cc, the only TU that includes <cstdio> — this
// header deliberately does not); no <iostream>, no locale handling.
//
// Flushing: println / eprintln ALWAYS flush after writing, so line-oriented output appears promptly even
// when the stream is redirected or piped. print / eprint do NOT flush — stdout stays buffered (line-buffered
// to a terminal, fully buffered when redirected, flushed at normal program exit). For buffered, non-flushing
// line output, use print and append your own '\n'. stderr is unbuffered regardless. cc::flush() / cc::eflush()
// flush a stream explicitly.
// =========================================================================================================

namespace cc
{
// -----------------------------------------------------------------------------------------------------
// raw writers (defined in print.cc)
// -----------------------------------------------------------------------------------------------------

/// Writes s to stdout verbatim (no format interpretation).
void print(string_view s);
/// Writes s followed by '\n' to stdout and flushes; cc::println() writes just a newline.
void println(string_view s = {});
/// Flushes any buffered stdout output. Rarely needed (stdout is line-buffered to a terminal and flushed at
/// exit), but useful for prompt progress output when stdout is redirected/piped.
void flush();

/// Writes s to stderr verbatim (no format interpretation).
void eprint(string_view s);
/// Writes s followed by '\n' to stderr and flushes; cc::eprintln() writes just a newline.
void eprintln(string_view s = {});
/// Flushes stderr. Provided for symmetry; stderr is already unbuffered, so this is normally a no-op.
void eflush();

// -----------------------------------------------------------------------------------------------------
// cc::format overloads (>= 1 argument; route through the raw writers above)
// -----------------------------------------------------------------------------------------------------

/// Formats the arguments with cc::format and writes the result to stdout.
template <class Arg0, class... Args>
void print(format_string<std::type_identity_t<Arg0>, std::type_identity_t<Args>...> fmt, Arg0&& arg0, Args&&... args)
{
    cc::print(cc::format(fmt, cc::forward<Arg0>(arg0), cc::forward<Args>(args)...));
}

/// Like print, but appends a trailing '\n' and flushes.
template <class Arg0, class... Args>
void println(format_string<std::type_identity_t<Arg0>, std::type_identity_t<Args>...> fmt, Arg0&& arg0, Args&&... args)
{
    auto s = cc::format(fmt, cc::forward<Arg0>(arg0), cc::forward<Args>(args)...);
    s.push_back('\n');
    cc::print(s);
    cc::flush();
}

/// Formats the arguments with cc::format and writes the result to stderr.
template <class Arg0, class... Args>
void eprint(format_string<std::type_identity_t<Arg0>, std::type_identity_t<Args>...> fmt, Arg0&& arg0, Args&&... args)
{
    cc::eprint(cc::format(fmt, cc::forward<Arg0>(arg0), cc::forward<Args>(args)...));
}

/// Like eprint, but appends a trailing '\n' and flushes.
template <class Arg0, class... Args>
void eprintln(format_string<std::type_identity_t<Arg0>, std::type_identity_t<Args>...> fmt, Arg0&& arg0, Args&&... args)
{
    auto s = cc::format(fmt, cc::forward<Arg0>(arg0), cc::forward<Args>(args)...);
    s.push_back('\n');
    cc::eprint(s);
    cc::eflush();
}
} // namespace cc
