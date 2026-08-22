#pragma once

#include <clean-core/string/string.hh>
#include <nexus/args/diagnostic.hh>
#include <nexus/args/fwd.hh>

// The help page, and the usage line the error path borrows.
//
// Width and color arrive in args_render_options rather than being sniffed from the process, which is the
// whole reason help output can be golden-tested at all.

namespace nx::impl
{
struct help_renderer
{
    /// `full` is --help: long descriptions, sections, examples and environment documentation.
    /// Otherwise it is -h: the one-screen form, argument table only.
    static cc::string render(args_builder const& builder, args_render_options const& options, bool full);

    static cc::string usage(args_builder const& builder);
};
} // namespace nx::impl
