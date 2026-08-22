#include "diagnostic.hh"

#include <clean-core/platform/console.hh>
#include <clean-core/string/format.hh>

// One rendered line per diagnostic, plus an indented suggestion when there is one.
// The wording lives here and nowhere else, which is what lets a test assert on `kind` and leave the prose
// free to improve.

namespace
{
using namespace cc::console;

cc::string_view label_of(nx::diagnostic_kind kind)
{
    // A setup error is the program's fault, so it says so rather than reading as a complaint about typing.
    return kind == nx::diagnostic_kind::setup_error ? "internal error" : "error";
}

cc::string where_of(nx::arg_source const& source)
{
    switch (source.origin)
    {
    case nx::arg_origin::response_file:
        return cc::format(" (in {})", source.name);
    case nx::arg_origin::environment:
        return cc::format(" (from {})", source.name);
    case nx::arg_origin::test_args:
        return " (in the test's arguments)";
    case nx::arg_origin::command_line:
        break;
    }

    return {};
}
} // namespace

cc::string nx::impl::render_diagnostic(args_diagnostic const& diagnostic, args_render_options const& options)
{
    auto const label = colorize(color::red, label_of(diagnostic.kind), options.color);

    auto out = cc::format("{}: {}{}", label, diagnostic.message, where_of(diagnostic.source));

    if (!diagnostic.suggestion.empty())
        cc::format_append(out, "\n  did you mean {}?", colorize(color::green, diagnostic.suggestion, options.color));

    return out;
}

cc::string nx::impl::render_diagnostics(cc::span<args_diagnostic const> diagnostics,
                                        cc::string_view usage_hint,
                                        args_render_options const& options)
{
    auto out = cc::string();

    for (auto const& diagnostic : diagnostics)
    {
        if (!out.empty())
            out += "\n";

        out += render_diagnostic(diagnostic, options);
    }

    if (!usage_hint.empty())
    {
        if (!out.empty())
            out += "\n\n";

        out += usage_hint;
    }

    return out;
}

cc::string nx::args_result::diagnostic_text(args_render_options const& options) const
{
    return impl::render_diagnostics(_diagnostics, {}, options);
}
