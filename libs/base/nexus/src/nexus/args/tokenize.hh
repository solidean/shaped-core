#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/args/fwd.hh>

// =========================================================================================================
// Turning one string into argument tokens, and splicing response files into a token list.
//
// The quoting rules are OURS and identical on every platform.
// Imitating a platform shell would be the obvious move and the wrong one: a response file written on Linux
// has to mean the same thing on Windows, and cmd.exe's rules are not something to reproduce on purpose.
//
// The grammar, in full:
//   - tokens are separated by whitespace
//   - "..." and '...' group, and may sit in the middle of a token
//   - inside double quotes, \" \\ \n \t \r \0 are escapes; everything else after a backslash is itself
//   - inside single quotes nothing is an escape, so a Windows path survives being pasted in
//   - a '#' where a token would start runs to the end of the line
//
// Used by three callers that must agree: response files, the nexus CLI's --test-args, and the arg line a
// test declares with nx::config::args.
// =========================================================================================================

/// What a response-file splice concluded.
struct nx::args_splice_result
{
    cc::vector<cc::string> tokens;
    cc::vector<cc::string> errors; // an unreadable file or a too-deep chain, one line each

    [[nodiscard]] bool ok() const { return errors.empty(); }
};

namespace nx
{
/// Split `text` into tokens by the rules above.
/// Never fails: an unterminated quote simply runs to the end of the input.
[[nodiscard]] cc::vector<cc::string> args_tokenize(cc::string_view text);

/// Replace every `@file` token with the tokens that file contains, recursively.
///
/// `@@rest` is a literal `@rest`, which is how an argument that genuinely starts with `@` is written.
/// Splicing STOPS at the first bare `--`: everything after it belongs to whoever receives the tail, and
/// rewriting it here would silently change what they are handed.
///
/// A chain deeper than `max_depth` is an error rather than a hang, and so is a file that cannot be read —
/// a response file that silently expands to nothing is the worst available outcome.
[[nodiscard]] args_splice_result args_splice_response_files(cc::span<cc::string_view const> tokens, int max_depth = 8);

} // namespace nx
