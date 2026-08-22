#pragma once

#include <nexus/args/diagnostic.hh>
#include <nexus/args/fwd.hh>

// What a DECLARATION must satisfy, as opposed to what a command line must.
//
// Everything here is a bug in the program, never in what the user typed, so it is reported as
// diagnostic_kind::setup_error: no suggestion, no usage line, and its own exit code.
//
// This runs inside every parse, in EVERY preset — deliberately not behind CC_ASSERT, because a duplicate
// short name in a shipped release binary is exactly the case that must not silently do the wrong thing.
// A program's own test suite should call args_builder::validate_setup directly, which is where it is
// cheapest to notice.

namespace nx::impl
{
struct setup_checker
{
    /// Takes the builder MUTABLY because checking a `default_command` means declaring it: the whole point of
    /// that check is to compare the root's names against the command's, and a deferred subtree has none
    /// until it is forced.
    static args_result run(args_builder& builder);

private:
    /// The `default_command` half, which needs the command declared before it can compare anything.
    static void check_default_command(args_builder& builder, args_result& result);
};
} // namespace nx::impl
