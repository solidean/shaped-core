#include "blessed_includes.hh"

#include <clean-core/string/format.hh>
#include <clean-core/string/glob.hh>
#include <shaped-linter/lex/directive.hh>

namespace scl
{
namespace
{
constexpr cc::string_view k_id = "blessed-includes";
constexpr cc::string_view k_rationale
    = "every standard, platform or third-party header a library reaches for is a dependency it owes an argument for, "
      "so each one is "
      "blessed by name in a .shaped-lint.yml — see tools/shaped-linter/docs/configuration.md.";

/// Is this one of ours?
/// Two spellings say so, and neither can collide with a header we do not own: a path (`<clean-core/fwd.hh>`), and a bare `.hh`, which
/// is our header extension and no SDK's.
/// The second is what keeps the generated shader headers (`<sv_shaders.hh>`) out of every library's config.
bool is_project_include(cc::string_view target)
{
    auto const inner = target.subview({.start = 1, .end = target.size() - 1});
    return inner.contains('/') || inner.ends_with(".hh");
}
} // namespace

namespace
{
void check(lint_context& ctx)
{
    // No config above this file means nothing was said about it, which is not the same as denying everything.
    if (!ctx.config.checks_includes())
        return;

    auto const path = cc::glob_normalize_path(ctx.source.path());

    for (auto const& t : ctx.tokens.tokens)
    {
        if (!t.is(token_kind::preprocessor_directive))
            continue;

        auto const target = include_target(t.text);
        if (target.empty() || !target.starts_with('<'))
            continue; // not an include, a macro spells it, or it is the file's own `"sibling.hh"`
        if (is_project_include(target))
            continue;

        auto const decision = ctx.config.classify_include(path, target);
        if (decision.verdict == include_verdict::allowed)
            continue;

        // `target` is a subview of the token's text, which views the buffer — so its offset is the token's plus the distance between them.
        auto const offset = t.span.byte_begin + u32(target.data() - t.text.data());
        auto const span
            = source_span{.file_id = t.span.file_id, .byte_begin = offset, .byte_end = offset + u32(target.size())};

        auto const help
            = decision.verdict == include_verdict::denied
                ? cc::string(decision.reason)
                : cc::format("bless it with an allow-include entry in {}, carrying the reason it is worth it",
                             ctx.config.nearest_config_path.empty() ? cc::string_view(".shaped-lint.yml")
                                                                    : cc::string_view(ctx.config.nearest_config_path));

        ctx.report({
            .rule_id = k_id,
            .span = span,
            .message = cc::format("{} is not blessed here", target),
            .sev = severity::warning,
            .suggested_hint = hint{.message = help},
        });
    }
}
} // namespace

rule const& blessed_includes_rule()
{
    static rule const r = {
        .id = k_id,
        .rationale = k_rationale,
        .layer = rule_layer::tokens,
        .languages = k_cpp_only,
        .default_severity = severity::warning,
        .check = &check,
    };
    return r;
}
} // namespace scl
