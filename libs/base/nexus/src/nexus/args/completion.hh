#pragma once

#include <clean-core/string/string.hh>
#include <nexus/args/fwd.hh>

// =========================================================================================================
// Shell completion, generated from the same declaration everything else comes from.
//
// This is where deferred subcommands pay for themselves the other way round: generating a completion script
// forces EVERY subtree, because the script has to know about commands the program may never run.
//
// A delegate contributes only its name.
// Its options belong to a parser we cannot see, and guessing at them would be worse than offering nothing.
// =========================================================================================================

enum class nx::completion_shell
{
    bash,
    zsh,
    fish,
    powershell,
};

namespace nx
{
/// Parse a `--completion` argument, or nothing when it names no shell we emit.
[[nodiscard]] cc::optional<completion_shell> completion_shell_from_name(cc::string_view name);

/// A completion script for `shell`, ready to be sourced.
/// Writes nothing and reads nothing: the caller prints it wherever it wants.
[[nodiscard]] cc::string generate_completion(args_builder& builder, completion_shell shell);

} // namespace nx
