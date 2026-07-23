#include "parser.hh"

#include <clean-core/common/utility.hh>

namespace scl
{
namespace
{
record_keyword record_keyword_of(cc::string_view kw)
{
    if (kw == "class")
        return record_keyword::class_;
    if (kw == "union")
        return record_keyword::union_;
    return record_keyword::struct_;
}

/// A '(' immediately following one of these is NOT a parameter list — it is an attribute-ish group
/// (`alignas(64)`, `explicit(cond)`, `noexcept(expr)`, `decltype(x)`, …) and must not mark the segment
/// as a function.
bool is_paren_neutralizing(token const& t)
{
    return t.is_keyword("alignas") || t.is_keyword("decltype") || t.is_keyword("noexcept") || t.is_keyword("explicit")
        || t.is_keyword("sizeof") || t.is_keyword("alignof") || t.is_keyword("static_assert")
        || t.is_keyword("operator") || t.text == "__declspec" || t.text == "__attribute__";
}

/// The recursive-descent driver over a flat array of significant tokens (trivia already dropped). It
/// walks declaration-by-declaration, tracking bracket depth by skipping balanced groups, and emits
/// record and member nodes into an arena.
struct parser_impl
{
    u32 file_id = 0;
    cc::vector<token> toks; // significant tokens, ending with an end_of_file sentinel
    syntax_tree tree;

    token const& tk(isize i) const { return toks[i < toks.size() ? i : toks.size() - 1]; }
    bool is_eof(isize i) const { return tk(i).is(token_kind::end_of_file); }
    bool punct(isize i, cc::string_view p) const { return tk(i).is_punct(p); }
    bool kw(isize i, cc::string_view k) const { return tk(i).is_keyword(k); }

    isize add_node(node n)
    {
        tree.nodes.push_back(cc::move(n));
        return tree.nodes.size() - 1;
    }

    /// Given `open` at a `o`/`c` bracket pair, return the index just past the matching close. Nesting
    /// on the same bracket is tracked. Runs to end on an unbalanced input (best-effort).
    isize skip_balanced(isize open, cc::string_view o, cc::string_view c) const
    {
        isize depth = 0;
        for (isize i = open; !is_eof(i) && i < toks.size(); ++i)
        {
            if (punct(i, o))
                ++depth;
            else if (punct(i, c))
            {
                --depth;
                if (depth == 0)
                    return i + 1;
            }
        }
        return toks.size();
    }

    /// Given `open` at a `<`, return the index just past the matching `>`. A `>>` token closes two
    /// levels. Bails at a `;` or `{` so a stray `<` (not really a template) cannot run away.
    isize skip_angles(isize open) const
    {
        isize depth = 0;
        for (isize i = open; !is_eof(i) && i < toks.size(); ++i)
        {
            if (punct(i, "<"))
                ++depth;
            else if (punct(i, ">>"))
            {
                depth -= 2;
                if (depth <= 0)
                    return i + 1;
            }
            else if (punct(i, ">"))
            {
                --depth;
                if (depth == 0)
                    return i + 1;
            }
            else if (punct(i, ";") || punct(i, "{"))
                return i;
        }
        return toks.size();
    }

    /// Parse the declarations in the half-open index range [begin, end). `member_scope` controls the
    /// one scope-dependent behavior: a brace-initialized declaration becomes a member_declaration only
    /// in a record body. Records and namespaces are descended in both scopes.
    void parse_scope(isize begin, isize end, bool member_scope, isize parent)
    {
        isize pos = begin;
        while (pos < end && !is_eof(pos))
            pos = scan_one(pos, end, member_scope, parent);
    }

    /// Consume exactly one declaration starting at `begin`; return the index just past it.
    isize scan_one(isize begin, isize end, bool member_scope, isize parent)
    {
        // A namespace: descend its body as a declaration scope; skip an alias (`namespace A = B;`).
        if (kw(begin, "namespace"))
        {
            for (isize i = begin + 1; i < end && !is_eof(i); ++i)
            {
                if (punct(i, "{"))
                {
                    auto const body_close = skip_balanced(i, "{", "}") - 1; // index of the matching '}'
                    parse_scope(i + 1, body_close, /*member_scope*/ false, parent);
                    return body_close + 1;
                }
                if (punct(i, ";"))
                    return i + 1;
            }
            return end;
        }

        bool paren_group_seen = false;
        bool def_head = false; // saw a type-defining keyword (class/struct/union/enum) at top level
        bool is_record = false;
        record_keyword rec_kw = record_keyword::struct_;
        isize rec_name_index = -1;
        bool expect_record_name = false;
        isize declarator_index = -1; // last top-level identifier — the declarator-id candidate
        isize prev_top_index = -1;   // previous top-level significant token (for the neutralize check)

        isize pos = begin;
        while (pos < end && !is_eof(pos))
        {
            token const& t = tk(pos);

            if (t.is_keyword("class") || t.is_keyword("struct") || t.is_keyword("union"))
            {
                def_head = true;
                is_record = true;
                rec_kw = record_keyword_of(t.text);
                expect_record_name = true;
                prev_top_index = pos;
                ++pos;
                continue;
            }
            if (t.is_keyword("enum"))
            {
                def_head = true;
                prev_top_index = pos;
                ++pos;
                if (kw(pos, "class") || kw(pos, "struct")) // `enum class` / `enum struct`
                    ++pos;
                continue;
            }

            if (t.is_punct("{"))
            {
                if (paren_group_seen || def_head)
                {
                    if (is_record)
                        return finish_record(begin, pos, end, rec_kw, rec_name_index, parent);

                    // A function body / enum body / other definition body — skip it whole.
                    auto after = skip_balanced(pos, "{", "}");
                    if (after < end && punct(after, ";"))
                        ++after;
                    return after;
                }
                return finish_brace_init(begin, pos, end, member_scope, declarator_index, parent);
            }

            if (t.is_punct("=")) // a top-level '=' is assignment-form init (never a comparison — those lex whole)
                return run_to_semicolon(pos + 1, end);

            if (t.is_punct(";")) // a plain declaration / forward decl with no initializer
                return pos + 1;

            if (t.is_punct("("))
            {
                bool const neutralize = prev_top_index >= 0 && is_paren_neutralizing(tk(prev_top_index));
                auto const after = skip_balanced(pos, "(", ")");
                if (!neutralize)
                    paren_group_seen = true;
                prev_top_index = after - 1;
                pos = after;
                continue;
            }
            if (t.is_punct("["))
            {
                auto const after = skip_balanced(pos, "[", "]");
                prev_top_index = after - 1;
                pos = after;
                continue;
            }
            if (t.is_punct("<"))
            {
                auto const after = skip_angles(pos);
                prev_top_index = after - 1;
                pos = after;
                continue;
            }

            if (t.is(token_kind::identifier))
            {
                if (expect_record_name)
                {
                    rec_name_index = pos;
                    expect_record_name = false;
                }
                declarator_index = pos;
            }
            prev_top_index = pos;
            ++pos;
        }
        return pos;
    }

    /// `open_brace` is the record body '{'. Emit the record_definition, recurse into its body, and
    /// consume any trailing declarator up to the terminating ';'.
    isize finish_record(isize begin, isize open_brace, isize end, record_keyword rec_kw, isize rec_name_index, isize parent)
    {
        auto const body_close = skip_balanced(open_brace, "{", "}") - 1; // matching '}'

        node rn;
        rn.kind = node_kind::record_definition;
        rn.rec_keyword = rec_kw;
        if (rec_name_index >= 0)
            rn.name = tk(rec_name_index).span;
        rn.span = source_span::join(tk(begin).span, tk(body_close).span);
        auto const id = add_node(cc::move(rn));
        tree.nodes[parent].children.push_back(id);

        parse_scope(open_brace + 1, body_close, /*member_scope*/ true, id);

        isize j = body_close + 1;
        while (j < end && !is_eof(j) && !punct(j, ";"))
            ++j;
        return (j < end && punct(j, ";")) ? j + 1 : j;
    }

    /// `open_brace` is an initializer '{'. Skip it, emit a member_declaration in a record body, and run
    /// to the terminating ';'.
    isize finish_brace_init(isize begin, isize open_brace, isize end, bool member_scope, isize declarator_index, isize parent)
    {
        auto const after = skip_balanced(open_brace, "{", "}");
        auto const close_brace = after - 1;

        // Run to the statement's ';', skipping any further balanced groups.
        isize j = after;
        while (j < end && !is_eof(j) && !punct(j, ";"))
        {
            if (punct(j, "{"))
                j = skip_balanced(j, "{", "}");
            else if (punct(j, "("))
                j = skip_balanced(j, "(", ")");
            else
                ++j;
        }
        auto const semi = j;

        if (member_scope && declarator_index >= 0)
        {
            node mn;
            mn.kind = node_kind::member_declaration;
            mn.init_form = member_init_form::brace;
            mn.name = tk(declarator_index).span;
            mn.init_span = source_span::join(tk(open_brace).span, tk(close_brace).span);
            mn.init_inner = {.file_id = file_id,
                             .byte_begin = tk(open_brace).span.byte_end,
                             .byte_end = tk(close_brace).span.byte_begin};
            auto const last = (semi < end && !is_eof(semi)) ? semi : close_brace;
            mn.span = source_span::join(tk(begin).span, tk(last).span);
            auto const id = add_node(cc::move(mn));
            tree.nodes[parent].children.push_back(id);
        }

        return (semi < end && punct(semi, ";")) ? semi + 1 : semi;
    }

    /// Skip an assignment initializer up to and including its ';' (balanced groups skipped).
    isize run_to_semicolon(isize from, isize end)
    {
        isize j = from;
        while (j < end && !is_eof(j) && !punct(j, ";"))
        {
            if (punct(j, "{"))
                j = skip_balanced(j, "{", "}");
            else if (punct(j, "("))
                j = skip_balanced(j, "(", ")");
            else if (punct(j, "["))
                j = skip_balanced(j, "[", "]");
            else
                ++j;
        }
        return (j < end && punct(j, ";")) ? j + 1 : j;
    }
};
} // namespace

cc::result<syntax_tree> parse(source_buffer const& buffer, token_stream const& tokens)
{
    parser_impl p;
    p.file_id = buffer.file_id();

    // Significant tokens only. Preprocessor directives are dropped here too: they are opaque to the
    // grammar (a `#include` / `#pragma` has no `;`, so leaving it in would glue the following
    // declaration onto it and, at file scope, make the first `namespace {` look like an initializer
    // brace). The directives remain in the token stream for future macro-placement rules.
    for (auto const& t : tokens.tokens)
        if (!t.is_trivia() && !t.is(token_kind::preprocessor_directive))
            p.toks.push_back(t); // keeps the trailing end_of_file as a sentinel

    node root;
    root.kind = node_kind::translation_unit;
    root.span = {.file_id = buffer.file_id(), .byte_begin = 0, .byte_end = u32(buffer.text().size())};
    p.tree.root = p.add_node(cc::move(root));

    auto const eof_index = p.toks.size() - 1; // parse everything before the sentinel
    p.parse_scope(0, eof_index, /*member_scope*/ false, p.tree.root);

    return cc::move(p.tree);
}
} // namespace scl
