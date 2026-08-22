#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/args/diagnostic.hh>
#include <nexus/args/fwd.hh>

// The token grammar, and the only place that walks a command line.
// libs/base/nexus/docs/args.md carries the same rules as prose; this file is where they are enforced.

namespace nx::impl
{
struct parse_engine
{
    static args_result run(args_builder& builder, cc::span<cc::string_view const> tokens);
};
} // namespace nx::impl
