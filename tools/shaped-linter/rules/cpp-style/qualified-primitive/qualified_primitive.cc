#include "qualified_primitive.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/string.hh>
#include <shaped-linter/lex/directive.hh>

namespace scl
{
namespace
{
constexpr cc::string_view k_id = "qualified-primitive";

constexpr cc::string_view k_rationale
    = "the sized aliases (`u32`, `isize`, `byte`, …) are vocabulary — as close to language-provided as we "
      "get — and a `cc::` prefix on them is noise, not information. Pull them into your namespace once, in "
      "its fwd.hh (`namespace my_lib { using namespace cc::primitive_defines; }`), and write them bare "
      "everywhere after that; in a test file one such directive at the top of the file suffices.";

/// Everything `cc::primitive_defines` declares — clean-core/fwd.hh is its one declaration site.
constexpr cc::string_view k_primitives[]
    = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "byte", "isize", "nullptr_t"};

/// The directive this rule inserts to make the bare spelling reachable at a `.cc`'s file scope.
constexpr cc::string_view k_using_directive = "using namespace cc::primitive_defines;";

/// The extensions that mark a translation unit rather than a header.
/// Everything else — `.hh`, and a path with no extension at all — counts as a header, because only a TU can take the directive privately.
constexpr cc::string_view k_implementation_extensions[] = {".cc", ".cpp", ".cxx", ".c"};

/// The namespaces that re-export the aliases with a `using namespace cc::primitive_defines;` in their own fwd.hh.
/// Qualified lookup searches a nominated namespace, so `sg::u32` names the same alias as `cc::u32` and reads exactly as wrong.
/// The list is spelled out because that directive lives in a header, and a single-file linter never sees the include.
/// A file that nominates the namespace itself is picked up on top of this, so a new library needs no edit here to be covered inside its own fwd.hh.
constexpr cc::string_view k_reexporting_namespaces[]
    = {"cc", "tg", "nx", "babel", "sg", "sr", "sv", "slib", "ssc", "scl", "itrace"};

bool is_primitive(cc::string_view name)
{
    for (auto const p : k_primitives)
        if (p == name)
            return true;
    return false;
}

cc::string_view trimmed(cc::string_view s)
{
    isize b = 0;
    isize e = s.size();
    while (b < e && cc::is_space(s[b]))
        ++b;
    while (e > b && cc::is_space(s[e - 1]))
        --e;
    return s.subview({.start = b, .end = e});
}

/// The `::`-separated components of a name as written — `cc::impl` becomes `cc`, `impl`, and a leading
/// `::` contributes an empty first component.
cc::vector<cc::string_view> name_components(cc::string_view name)
{
    cc::vector<cc::string_view> out;
    isize start = 0;
    isize i = 0;
    while (i < name.size())
    {
        if (name[i] == ':' && i + 1 < name.size() && name[i + 1] == ':')
        {
            out.push_back(trimmed(name.subview({.start = start, .end = i})));
            i += 2;
            start = i;
            continue;
        }
        ++i;
    }
    out.push_back(trimmed(name.subview({.start = start, .end = name.size()})));
    return out;
}

/// Does this namespace name reach the aliases unqualified? Any component counts: inside `cc::impl` or
/// `sg::dx12` they are just as visible as inside `cc` or `sg`.
bool is_reexporting(cc::span<cc::string_view const> known, cc::string_view ns_name)
{
    for (auto const c : name_components(ns_name))
        for (auto const k : known)
            if (c == k)
                return true;
    return false;
}

/// Is this using-directive the one that pulls the aliases in? Plain spellings only — no alias tracing,
/// so `namespace pd = cc::primitive_defines; using namespace pd;` is deliberately not followed.
bool nominates_primitives(lint_context const& ctx, node const& n)
{
    auto const parts = name_components(ctx.source.span_text(n.name));
    return parts.size() >= 2 && parts[parts.size() - 1] == "primitive_defines" && parts[parts.size() - 2] == "cc";
}

bool covers(source_span s, u32 offset)
{
    return offset >= s.byte_begin && offset < s.byte_end;
}

/// The namespace `id` sits directly in, or -1 at file scope.
isize enclosing_namespace(syntax_tree const& tree, isize id)
{
    for (auto n = isize(0); n < tree.nodes.size(); ++n)
        if (tree.nodes[n].kind == node_kind::namespace_definition)
            for (auto const c : tree.nodes[n].children)
                if (c == id)
                    return n;
    return -1;
}

/// The namespaces through which the aliases are reachable in this file: the ones that re-export them
/// repo-wide, plus any namespace this very file shows nominating `cc::primitive_defines`.
cc::vector<cc::string_view> reexporting_namespaces(lint_context const& ctx)
{
    cc::vector<cc::string_view> out;
    for (auto const n : k_reexporting_namespaces)
        out.push_back(n);

    for (auto d = isize(0); d < ctx.tree.nodes.size(); ++d)
    {
        if (ctx.tree.nodes[d].kind != node_kind::using_directive || !nominates_primitives(ctx, ctx.tree.nodes[d]))
            continue;

        // Only the namespace the directive sits DIRECTLY in gains the names, and of a nested-name form
        // only its innermost component: a `using namespace` inside `a::b` does nothing for `a`. So this
        // follows the parent link rather than asking which bodies span the directive.
        auto const ns = enclosing_namespace(ctx.tree, d);
        if (ns < 0)
            continue; // at file scope it puts the aliases in force, but makes no namespace reach them

        auto const parts = name_components(ctx.source.span_text(ctx.tree[ns].name));
        if (!parts[parts.size() - 1].empty())
            out.push_back(parts[parts.size() - 1]);
    }
    return out;
}

/// Is the declarator-id in front of the `(` at `paren` qualified by a namespace that reaches the aliases?
/// `cc::async_thread_pool::try_get_work(` — yes, through `cc`. A bare `work(` is not, and neither is
/// `f(cc::u32 x)`: only what sits BEFORE the parameter list can name the scope the definition belongs to.
bool declarator_is_reexporting(cc::span<token const> toks,
                               cc::span<isize const> sig,
                               isize paren,
                               cc::span<cc::string_view const> reexporters)
{
    auto k = paren - 1;
    if (k < 0 || !toks[sig[k]].is(token_kind::identifier))
        return false;

    for (--k; k >= 1 && toks[sig[k]].is_punct("::") && toks[sig[k - 1]].is(token_kind::identifier); k -= 2)
        if (is_reexporting(reexporters, toks[sig[k - 1]].text))
            return true;
    return false;
}

/// The bodies of out-of-line definitions whose declarator-id is qualified by a re-exporting namespace —
/// `cc::async_thread_pool::try_get_work(…) { … }`.
///
/// Such a definition sits at file scope lexically, but a qualified declarator-id carries its namespace in
/// with it: everything written AFTER it is looked up in `cc`, so the bare spelling is already reachable.
/// Without this the rule would offer a whole file-scope using-directive for names that never needed one.
///
/// The span therefore starts at the parameter list, not at the brace — a parameter type is as much "after the declarator-id" as the body is.
/// What comes before it, the return type, is left out and correctly so: that one really is looked up at file scope.
cc::vector<source_span> out_of_line_bodies(lint_context const& ctx,
                                           cc::span<isize const> sig,
                                           cc::span<cc::string_view const> reexporters)
{
    auto const toks = cc::span<token const>(ctx.tokens.tokens);

    cc::vector<source_span> out;
    auto brace_depth = 0;
    auto paren_depth = 0;
    auto qualified = false;
    auto scope_begin = u32(0);

    for (auto k = isize(0); k < sig.size(); ++k)
    {
        auto const& t = toks[sig[k]];

        if (t.is_punct("("))
        {
            // Only until one qualifies: a constructor's member-init list is a run of further depth-0
            // parens (`: _x(1)`), and reading those as declarators would undo what the real one found.
            if (brace_depth == 0 && paren_depth == 0 && !qualified && declarator_is_reexporting(toks, sig, k, reexporters))
            {
                qualified = true;
                scope_begin = t.span.byte_begin;
            }
            ++paren_depth;
            continue;
        }
        if (t.is_punct(")"))
        {
            paren_depth = paren_depth > 0 ? paren_depth - 1 : 0;
            continue;
        }
        if (paren_depth > 0)
            continue;

        if (t.is_punct("{"))
        {
            ++brace_depth;
            continue;
        }
        if (t.is_punct("}"))
        {
            brace_depth = brace_depth > 0 ? brace_depth - 1 : 0;
            if (brace_depth == 0)
            {
                if (qualified)
                    out.push_back({.file_id = t.span.file_id, .byte_begin = scope_begin, .byte_end = t.span.byte_end});
                qualified = false;
            }
            continue;
        }
        if (brace_depth == 0 && t.is_punct(";"))
            qualified = false; // a declaration with no body must not carry into the next `{`
    }
    return out;
}

bool covered_by_any(cc::span<source_span const> spans, u32 offset)
{
    for (auto const& s : spans)
        if (covers(s, offset))
            return true;
    return false;
}

/// Is `offset` outside every NAMED namespace — reachable from the file's own scope?
///
/// An anonymous namespace is not a barrier here.
/// It is this file's own, and unqualified lookup inside it escapes outward to the global namespace, which is exactly where a file-scope using-directive nominates.
/// So the helper block at the top of a test file is fixed by the same one line as the `TEST(…)` bodies below it.
/// A named namespace is a library's, and its fwd.hh is where the directive belongs instead — a judgement call about that library, so the rule only ever hints there.
bool at_file_scope(lint_context const& ctx, u32 offset)
{
    for (auto const& n : ctx.tree.nodes)
        if (n.kind == node_kind::namespace_definition && covers(n.body, offset) && n.name.byte_begin != n.name.byte_end)
            return false;
    return true;
}

bool is_implementation_file(cc::string_view path)
{
    for (auto const ext : k_implementation_extensions)
        if (path.ends_with(ext))
            return true;
    return false;
}

/// Where a file-scope `using namespace cc::primitive_defines;` can go, and the text to put there.
///
/// The anchor is the file's leading `#…` block, taken as the last directive that sits at conditional depth 0 — not the last `#include`.
/// Depth is what makes the offset safe.
/// A prologue that opens `#if CC_HAS_THREADS` and runs into the code without closing it would otherwise anchor inside that branch.
/// The aliases would then be defined in one configuration only.
/// Falling back to the last depth-0 directive puts the line before the conditional instead, where it holds for every build.
///
/// Nothing comes back when no directive reaches depth 0 at all.
/// A file that opens with `#ifdef __EMSCRIPTEN__` and never leaves it has no such common ground, and the rule then stays quiet about it.
///
/// Nothing comes back either when any `#include` follows the anchor.
/// `cc::primitive_defines` has to be declared before the directive can nominate it, and which include declares it is what a single-file linter cannot know.
/// So a file that keeps including past the anchor — its real includes nested inside `#if CC_HAS_THREADS`, say — is left alone rather than guessed at.
///
/// A file with no directives at all takes offset 0. The insertion leaves a blank line on each side, so
/// the directive reads as its own paragraph between the includes and the code.
cc::optional<text_edit> using_directive_insertion(lint_context const& ctx)
{
    auto const& toks = ctx.tokens.tokens;
    auto const fid = ctx.source.file_id();

    auto depth = 0;
    auto anchor = isize(-1); // the last depth-0 directive of the leading block
    auto saw_directive = false;

    for (auto i = isize(0); i < toks.size(); ++i)
    {
        auto const& t = toks[i];
        if (t.is_trivia())
            continue;
        if (!t.is(token_kind::preprocessor_directive))
            break; // the first real token ends the prologue

        saw_directive = true;
        auto const word = directive_word(t.text);
        if (word == "if" || word == "ifdef" || word == "ifndef")
        {
            ++depth;
            continue;
        }
        if (word == "endif")
        {
            if (depth > 0)
                --depth;
            if (depth == 0)
                anchor = i; // a balanced block just closed — past it is common ground again
            continue;
        }
        if (depth == 0)
            anchor = i;
    }

    if (anchor < 0 && saw_directive)
        return {};

    // An `#include` past the anchor may be the one that declares the aliases, and the directive has to sit after that.
    // Which include it is cannot be told from this file alone, so the answer is to not guess.
    for (auto j = anchor + 1; j < toks.size(); ++j)
        if (toks[j].is(token_kind::preprocessor_directive) && directive_word(toks[j].text) == "include")
            return {};

    if (!saw_directive)
        return text_edit{.span = {.file_id = fid, .byte_begin = 0, .byte_end = 0},
                         .replacement = cc::string(k_using_directive) + "\n\n"};

    // Past the end of the anchor's LOGICAL line: the lexer already folds backslash-continuations into the
    // one directive token, and a trailing `// …` lexes after it, so the following newline is the seam.
    auto offset = toks[anchor].span.byte_end;
    for (auto j = anchor + 1; j < toks.size() && toks[j].is_trivia(); ++j)
        if (toks[j].is(token_kind::newline))
        {
            offset = toks[j].span.byte_end;
            break;
        }

    return text_edit{.span = {.file_id = fid, .byte_begin = offset, .byte_end = offset},
                     .replacement = cc::string("\n") + k_using_directive + "\n"};
}

/// Is the bare spelling reachable at `offset`? Either a directive in this file has it in force there, the
/// offset sits inside a namespace whose own fwd.hh re-exports the aliases, or it sits in the body of an
/// out-of-line definition that names such a namespace in its declarator-id.
bool bare_name_reachable(lint_context const& ctx,
                         cc::span<cc::string_view const> reexporters,
                         cc::span<source_span const> out_of_line,
                         u32 offset)
{
    if (covered_by_any(out_of_line, offset))
        return true;

    for (auto const& n : ctx.tree.nodes)
    {
        if (n.kind == node_kind::using_directive && covers(n.effect, offset) && nominates_primitives(ctx, n))
            return true;
        if (n.kind == node_kind::namespace_definition && covers(n.body, offset)
            && is_reexporting(reexporters, ctx.source.span_text(n.name)))
            return true;
    }
    return false;
}

/// Can this token end a nested-name-specifier — so that a `::` behind it qualifies something, rather than
/// rooting the name at global scope?
bool ends_a_qualifier(token const& t)
{
    return t.is(token_kind::identifier) || t.is_punct(">") || t.is_punct(">>") || t.is_punct(")") || t.is_punct("]");
}

void check(lint_context& ctx)
{
    // Trivia only.
    // A comment, a string literal and a whole `#…` line each lex as one token of a kind that is not `identifier`.
    // So a `cc::u32` inside any of them can never match the pattern below, and keeping them in the sequence makes each a barrier rather than something to see through.
    cc::vector<isize> sig;
    for (auto i = isize(0); i < ctx.tokens.tokens.size(); ++i)
        if (!ctx.tokens.tokens[i].is_trivia())
            sig.push_back(i);

    auto const reexporters = reexporting_namespaces(ctx);
    auto const out_of_line = out_of_line_bodies(ctx, sig, reexporters);
    auto const at = [&](isize k) -> token const& { return ctx.tokens.tokens[sig[k]]; };

    for (auto k = isize(0); k + 2 < sig.size(); ++k)
    {
        auto const& qualifier = at(k);
        auto const& primitive = at(k + 2);
        if (!qualifier.is(token_kind::identifier) || !at(k + 1).is_punct("::") || !primitive.is(token_kind::identifier))
            continue;
        if (!is_primitive(primitive.text) || !is_reexporting(reexporters, qualifier.text))
            continue;
        if (k + 3 < sig.size() && at(k + 3).is_punct("::"))
            continue; // used as a scope of its own — whatever that names, it is not the alias

        // What sits in front decides whether this is the name we think it is.
        auto begin = qualifier.span;
        auto preceding = k - 1; // the token before the whole name, once a leading `::` is accounted for
        if (k >= 1)
        {
            auto const& before = at(k - 1);
            if (before.is_punct(".") || before.is_punct("->"))
                continue;
            if (before.is_punct("::"))
            {
                if (k >= 2 && ends_a_qualifier(at(k - 2)))
                    continue; // `a::cc::u32` — a different `cc` entirely

                begin = before.span; // `::cc::u32` — the same name rooted at global scope, `::` and all
                preceding = k - 2;
            }
        }

        // `using cc::i64;` is a using-DECLARATION, where the qualified name is what names the thing being
        // declared — dropping the qualifier leaves `using i64;`, which is not valid C++. (A using-DIRECTIVE
        // never reaches here: `using namespace cc::primitive_defines;` ends in a name that is no alias.)
        if (preceding >= 0 && at(preceding).is_keyword("using"))
            continue;

        auto const span = source_span::join(begin, primitive.span);

        // Dropping the qualifier is byte-exact — from wherever the name starts up to the alias, so odd
        // spacing (`cc :: u32`) and a leading `::` both come out right.
        auto const drop_qualifier = text_edit{
            .span = {.file_id = span.file_id, .byte_begin = span.byte_begin, .byte_end = primitive.span.byte_begin}};

        // Dropping it alone is safe only where the bare name is already reachable.
        // Where it is not, what the second edit should be depends entirely on where the name sits:
        //   - at a `.cc`'s file scope — anonymous namespaces included, since they are the file's own — the directive belongs at the top of that file.
        //     That is mechanical, so the fix carries both edits and `--fix` lands the whole thing.
        //   - at a header's file scope there is no such edit, since a using-directive there leaks the aliases into the global namespace of every TU that includes it.
        //     So the rule says nothing at all.
        //   - inside a namespace the right edit is a `using namespace` in that library's fwd.hh, a judgement call about the library rather than about this line.
        //     That one stays a hint.
        auto suggested_fix = cc::optional<fix>();
        auto suggested_hint = cc::optional<hint>();
        if (bare_name_reachable(ctx, reexporters, out_of_line, span.byte_begin))
            suggested_fix = fix{.edits = {drop_qualifier}};
        else if (at_file_scope(ctx, span.byte_begin))
        {
            if (!is_implementation_file(ctx.source.path()))
                continue;

            auto insertion = using_directive_insertion(ctx);
            if (!insertion.has_value())
                continue; // nowhere in the prologue is outside a conditional — see the helper

            suggested_fix = fix{.edits = {drop_qualifier, cc::move(insertion.value())}};
        }
        else
            suggested_hint = hint{.message = cc::string("no `using namespace cc::primitive_defines;` is in force "
                                                        "here; add one to the enclosing namespace (a library's "
                                                        "fwd.hh is where it belongs, a test file's top line "
                                                        "otherwise), then drop the qualifier")};

        ctx.report({
            .rule_id = k_id,
            .span = span,
            .message = cc::string("`") + ctx.source.span_text(span) + "` should be spelled unqualified as `"
                     + primitive.text + "`",
            .sev = severity::warning,
            .suggested_fix = cc::move(suggested_fix),
            .suggested_hint = cc::move(suggested_hint),
        });
    }
}
} // namespace

rule const& qualified_primitive_rule()
{
    static rule const r = {
        .id = k_id,
        .rationale = k_rationale,
        .layer = rule_layer::syntax_tree,
        .default_severity = severity::warning,
        .check = &check,
    };
    return r;
}
} // namespace scl
