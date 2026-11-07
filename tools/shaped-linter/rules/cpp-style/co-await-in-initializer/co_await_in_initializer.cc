#include "co_await_in_initializer.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/glob.hh>
#include <shaped-linter/lex/directive.hh>
#include <shaped-linter/lex/lexer.hh>

namespace scl
{
namespace
{
constexpr cc::string_view k_id = "co-await-in-initializer";
constexpr cc::string_view k_rationale
    = "MSVC 14.44 and 14.51 crash (C1001) on a co_await after a built element of an initializer with a destructor, so "
      "await into a named local first. Tokens cannot see types, so a case MSVC compiles is reported too: the local is "
      "free. See docs/bugs-external/msvc-coroutine-await-in-aggregate-init-crash.";

bool is_open(token const& t)
{
    return t.is_punct("(") || t.is_punct("[") || t.is_punct("{");
}

bool is_close(token const& t)
{
    return t.is_punct(")") || t.is_punct("]") || t.is_punct("}");
}

/// Whether the tokens ahead of `open` are a trailing return type, `-> cc::shared_async<int>`.
bool follows_trailing_return(cc::span<token const* const> sig, isize open)
{
    for (auto i = open - 1; i >= 0; --i)
    {
        auto const& t = *sig[i];
        if (t.is_punct("->"))
            return true;
        if (!t.is(token_kind::identifier) && !t.is_keyword("const") && !t.is_punct("::") && !t.is_punct("<")
            && !t.is_punct(">") && !t.is_punct(">>") && !t.is_punct(",") && !t.is_punct("*") && !t.is_punct("&"))
            return false;
    }
    return false;
}

/// Whether the `]` at `close` ends a lambda's captures, rather than an array's extent.
bool closes_captures(cc::span<token const* const> sig, isize close)
{
    auto depth = 0;
    for (auto i = close; i >= 0; --i)
    {
        if (sig[i]->is_punct("]"))
            ++depth;
        else if (sig[i]->is_punct("[") && --depth == 0)
            return i == 0 || !(sig[i - 1]->is(token_kind::identifier) || sig[i - 1]->is_punct("]"));
    }
    return false;
}

/// Whether the `{` at `open` starts a body rather than an initializer.
bool opens_body(cc::span<token const* const> sig, isize open)
{
    if (open == 0)
        return false;
    auto const& before = *sig[open - 1];
    if (before.is_punct(")") || before.is_punct(";") || before.is_punct("}") || before.is_punct(":"))
        return true;
    if (before.is_keyword("else") || before.is_keyword("do") || before.is_keyword("try")
        || before.is_keyword("noexcept") || before.is_keyword("const") || before.is_keyword("mutable")
        || before.text == "override" || before.text == "final")
        return true;
    if (before.is_punct("]"))
        return closes_captures(sig, open - 1);
    return follows_trailing_return(sig, open);
}

/// Whether an enclosing brace initializer of the `co_await` at `await` has an earlier element.
bool follows_an_element(cc::span<token const* const> sig, isize await)
{
    auto depth = 0;
    auto comma = false; // at the level being walked, whose kind is known only once its opener is reached
    for (auto i = await - 1; i >= 0; --i)
    {
        auto const& t = *sig[i];
        if (is_close(t))
            ++depth;
        else if (is_open(t))
        {
            if (depth > 0)
            {
                --depth;
                continue;
            }
            if (t.is_punct("{"))
            {
                if (opens_body(sig, i))
                    return false;
                if (comma)
                    return true;
            }
            comma = false;
        }
        else if (depth == 0 && t.is_punct(";"))
            return false;
        else if (depth == 0 && t.is_punct(","))
            comma = true;
    }
    return false;
}

/// Reports each offending `co_await` in `sig`, whose spans lie `offset` bytes ahead of the file's.
void report_in(lint_context& ctx, cc::span<token const* const> sig, u32 offset)
{
    for (auto i = isize(1); i < sig.size(); ++i)
    {
        // `operator co_await` declares an operator, and awaits nothing
        if (!sig[i]->is_keyword("co_await") || sig[i - 1]->is_keyword("operator") || !follows_an_element(sig, i))
            continue;
        ctx.report({
            .rule_id = k_id,
            .span = {.file_id = ctx.source.file_id(),
                     .byte_begin = sig[i]->span.byte_begin + offset,
                     .byte_end = sig[i]->span.byte_end + offset},
            .message = cc::string("a co_await after an earlier element of an initializer crashes MSVC"),
            .sev = severity::warning,
            .suggested_hint = hint{.message = cc::string("await into a named local first, then use the local here")},
        });
    }
}

cc::vector<token const*> significant(cc::span<token const> tokens)
{
    auto sig = cc::vector<token const*>();
    for (auto const& t : tokens)
        if (!t.is_trivia() && !t.is(token_kind::unknown)) // a macro's continuation backslash lexes as unknown
            sig.push_back(&t);
    return sig;
}

void check(lint_context& ctx)
{
    auto const path = cc::glob_normalize_path(ctx.source.path());
    if (path.contains("docs/bugs-external/") && path.ends_with("/repro.cc"))
        return;

    report_in(ctx, significant(ctx.tokens.tokens), 0);

    for (auto const& t : ctx.tokens.tokens)
    {
        if (!t.is(token_kind::preprocessor_directive) || directive_word(t.text) != "define")
            continue;
        // the words after `define`, lexed apart, because a '#' at the start of a line would lex back into one directive
        auto const word = directive_word(t.text);
        auto const skipped = isize(word.data() - t.text.data()) + word.size();
        auto const body
            = source_buffer::from_text(cc::string(t.text.subview(skipped)), ctx.source.path(), ctx.source.file_id());
        auto const lexed = lex(body);
        if (lexed.has_value())
            report_in(ctx, significant(lexed.value().tokens), t.span.byte_begin + u32(skipped));
    }
}
} // namespace

rule const& co_await_in_initializer_rule()
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
