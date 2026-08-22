#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/args/diagnostic.hh>
#include <nexus/args/fwd.hh>
#include <nexus/args/impl/command.hh>

// The token grammar, and the only place that walks a command line.
// libs/base/nexus/docs/args.md carries the same rules as prose; this file is where they are enforced.

namespace nx::impl
{
struct parse_engine
{
    static args_result run(args_builder& builder, cc::span<cc::string_view const> tokens);

    /// Name lookup, walking up through enclosing builders for anything marked `global()`.
    /// A subcommand's own options win over an inherited one of the same name.
    static binding* find_long(args_builder& builder, cc::string_view name, bool normalize);
    static binding* find_short(args_builder& builder, char c);

    /// Every spelling a typo could have meant at this depth, inherited globals included.
    static cc::vector<cc::string> suggestion_space(args_builder& builder, bool want_long);

    /// The command answering to `name`, or null.
    static command_node* find_command(args_builder& builder, cc::string_view name);

    /// The child builder for `node`, declaring it on first use.
    /// Idempotent: help and completion force every subtree, and a parse forces the selected one.
    static args_builder& force_declare(args_builder& builder, command_node& node);
};
} // namespace nx::impl
