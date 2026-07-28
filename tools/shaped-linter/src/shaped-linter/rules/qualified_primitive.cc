#include "qualified_primitive.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/string.hh>

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

/// The namespaces that re-export the aliases with a `using namespace cc::primitive_defines;` in their own
/// fwd.hh. Qualified lookup searches a nominated namespace, so `sg::u32` names the same alias as `cc::u32`
/// and reads exactly as wrong. The list is spelled out because that directive lives in a header, and a
/// single-file linter never sees the include — a file that nominates the namespace itself is picked up on
/// top of this, so a new library needs no edit here to be covered inside its own fwd.hh.
constexpr cc::string_view k_reexporting_namespaces[]
    = {"cc", "tg", "nx", "babel", "sg", "sr", "sv", "slib", "scl", "itrace"};

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

/// Is the bare spelling reachable at `offset`? Either a directive in this file has it in force there, or
/// the offset sits inside a namespace whose own fwd.hh re-exports the aliases.
bool bare_name_reachable(lint_context const& ctx, cc::span<cc::string_view const> reexporters, u32 offset)
{
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
    // Trivia only. A comment, a string literal and a whole `#…` line each lex as ONE token of a kind that
    // is not `identifier`, so a `cc::u32` spelled inside any of them can never match the pattern below,
    // and keeping them in the sequence makes each a barrier rather than something to see through.
    cc::vector<isize> sig;
    for (auto i = isize(0); i < ctx.tokens.tokens.size(); ++i)
        if (!ctx.tokens.tokens[i].is_trivia())
            sig.push_back(i);

    auto const reexporters = reexporting_namespaces(ctx);
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
            }
        }

        auto const span = source_span::join(begin, primitive.span);

        // Dropping the qualifier is byte-exact — from wherever the name starts up to the alias, so odd
        // spacing (`cc :: u32`) and a leading `::` both come out right. It is only safe where the bare
        // name is actually reachable; where it is not, the better form needs a second edit in a place this
        // rule cannot pick, and which namespace should nominate the aliases is exactly the judgement call.
        auto suggested_fix = cc::optional<fix>();
        auto suggested_hint = cc::optional<hint>();
        if (bare_name_reachable(ctx, reexporters, span.byte_begin))
            suggested_fix = fix{.edits = {text_edit{.span = {.file_id = span.file_id,
                                                             .byte_begin = span.byte_begin,
                                                             .byte_end = primitive.span.byte_begin}}}};
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
