#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/fwd.hh>
#include <nexus/args/impl/binding.hh>

// =========================================================================================================
// One subcommand.
//
// A subcommand is a nested args_builder rather than a special case, so nesting (`app remote add`) costs
// nothing and --help at any depth is the same code path.
//
// Declaration is DEFERRED: the callback that declares a command's arguments runs only when that command is
// selected, or when help or completion actually needs that subtree.
// A tool with twenty commands therefore pays for one on a normal run, and the short description — which is
// all a command list needs — is stored eagerly.
//
// The contract this places on a declare callback is worth stating plainly, because forcing is invisible
// from the call site: it must be pure declaration, callable at any point between the builder being
// constructed and the parse returning, and idempotent.
//
// A DELEGATE is the other kind: a command this program does not own at all, whose tail goes untouched to
// another library's parser.
// It cannot be introspected, so help shows it as opaque rather than pretending to know its options.
// =========================================================================================================

namespace nx::impl
{
struct command_node
{
    cc::vector<arg_name> names; // long spellings only; a subcommand is a word, not a letter
    cc::string canonical;
    cc::string desc;
    bool hidden = false;

    /// Fills in the child builder, and is null for a delegate.
    cc::unique_function<void(args_builder&)> declare;

    /// Index into the root's subtree storage, or -1 until `declare` has been forced.
    isize subtree = -1;
    bool declared = false;

    /// Everything after this command's own token, handed to somebody else entirely.
    /// Its return value becomes the process exit code.
    cc::unique_function<int(cc::span<cc::string_view const>)> delegate;

    [[nodiscard]] bool is_delegate() const { return delegate.is_valid(); }
};
} // namespace nx::impl
