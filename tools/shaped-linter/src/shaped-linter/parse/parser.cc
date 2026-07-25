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

/// A statement can carry a brace group that is an expression, not an initializer — `return T{1};`,
/// `throw e{2};`, `case k:`. Seeing one of these at the top of a segment disqualifies the whole segment
/// from being a declaration, which is what keeps function-scope parsing free of false positives.
bool is_statement_keyword(token const& t)
{
    return t.is_keyword("return") || t.is_keyword("throw") || t.is_keyword("co_return") || t.is_keyword("co_yield")
        || t.is_keyword("co_await") || t.is_keyword("case") || t.is_keyword("default") || t.is_keyword("else")
        || t.is_keyword("do") || t.is_keyword("try") || t.is_keyword("catch") || t.is_keyword("goto")
        || t.is_keyword("break") || t.is_keyword("continue") || t.is_keyword("new") || t.is_keyword("delete")
        || t.is_keyword("using");
}

/// The recursive-descent driver over a flat array of significant tokens (trivia already dropped). It
/// walks declaration-by-declaration, tracking bracket depth by skipping balanced groups, and emits
/// record and variable nodes into an arena.
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

    /// Parse the declarations in the half-open index range [begin, end). `scope` is what a
    /// brace-initialized declaration found here becomes — a data member, a namespace-scope variable, or
    /// a local. Records, namespaces, function bodies, nested blocks and lambda bodies are all descended.
    void parse_scope(isize begin, isize end, decl_scope scope, isize parent)
    {
        isize pos = begin;
        while (pos < end && !is_eof(pos))
            pos = scan_one(pos, end, scope, parent);
    }

    /// Consume exactly one declaration or statement starting at `begin`; return the index just past it.
    isize scan_one(isize begin, isize end, decl_scope scope, isize parent)
    {
        // A namespace: descend its body as a declaration scope; skip an alias (`namespace A = B;`).
        if (kw(begin, "namespace"))
        {
            for (isize i = begin + 1; i < end && !is_eof(i); ++i)
            {
                if (punct(i, "{"))
                {
                    auto const body_close = skip_balanced(i, "{", "}") - 1; // index of the matching '}'
                    parse_scope(i + 1, body_close, decl_scope::namespace_scope, parent);
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
        bool stmt_head = false; // saw a statement keyword — this segment cannot be a declaration
        record_keyword rec_kw = record_keyword::struct_;
        isize rec_name_index = -1;
        bool expect_record_name = false;
        isize declarator_index = -1; // last top-level identifier — the declarator-id candidate
        isize prev_top_index = -1;   // previous top-level significant token (for the neutralize check)
        isize top_token_count = 0;   // significant top-level tokens before the '{'

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
                ++top_token_count;
                ++pos;
                continue;
            }
            if (t.is_keyword("enum"))
            {
                def_head = true;
                prev_top_index = pos;
                ++top_token_count;
                ++pos;
                if (kw(pos, "class") || kw(pos, "struct")) // `enum class` / `enum struct`
                    ++pos;
                continue;
            }
            if (is_statement_keyword(t))
                stmt_head = true;

            if (t.is_punct("{"))
            {
                if (paren_group_seen || def_head)
                {
                    if (is_record)
                        return finish_record(begin, pos, end, rec_kw, rec_name_index, parent);

                    auto after = skip_balanced(pos, "{", "}");

                    // A constructor's mem-initializer list looks exactly like this: `S() : a{1}, b{2} {}`.
                    // Only the LAST brace group of the segment is the body, and a mem-initializer is always
                    // followed by ',' or by that body's '{' — so keep scanning when either follows.
                    if (after < end && !is_eof(after) && (punct(after, ",") || punct(after, "{")))
                    {
                        prev_top_index = after - 1;
                        pos = after;
                        continue;
                    }

                    // The definition body. A function body is real code we descend into; an enum body
                    // (def_head without a paren group) holds no declarations we model.
                    if (!def_head)
                        parse_scope(pos + 1, after - 1, decl_scope::function_scope, parent);

                    if (after < end && punct(after, ";"))
                        ++after;
                    return after;
                }

                // At function scope a brace group with no declarator in front of it, or one behind a
                // statement keyword, is a nested block (`{ … }`, an `else` / `do` / `try` body) or an
                // expression (`return P{a, b};`) — never an initializer. Descend rather than running past
                // it looking for a ';' that belongs to a later statement.
                if (scope == decl_scope::function_scope && (declarator_index < 0 || stmt_head))
                {
                    auto const after = skip_balanced(pos, "{", "}");
                    parse_scope(pos + 1, after - 1, decl_scope::function_scope, parent);
                    return after;
                }

                return finish_brace_init(begin, pos, end, scope, declarator_index, top_token_count, stmt_head, parent);
            }

            if (t.is_punct("=")) // a top-level '=' is assignment-form init (never a comparison — those lex whole)
                return run_to_semicolon(pos + 1, end, scope, parent);

            if (t.is_punct(";")) // a plain declaration / forward decl with no initializer
                return pos + 1;

            if (t.is_punct("("))
            {
                bool const neutralize = prev_top_index >= 0 && is_paren_neutralizing(tk(prev_top_index));
                auto const after = skip_balanced(pos, "(", ")");
                if (!neutralize)
                    paren_group_seen = true;
                descend_lambdas(pos, after, scope, parent);
                prev_top_index = after - 1;
                ++top_token_count;
                pos = after;
                continue;
            }
            if (t.is_punct("["))
            {
                auto const after = skip_balanced(pos, "[", "]");
                prev_top_index = after - 1;
                ++top_token_count;
                pos = after;
                continue;
            }
            if (t.is_punct("<"))
            {
                auto const after = skip_angles(pos);
                prev_top_index = after - 1;
                ++top_token_count;
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
            ++top_token_count;
            ++pos;
        }
        return pos;
    }

    /// Walk a group we are otherwise skipping and descend into any lambda body inside it. A lambda
    /// introducer is a `]` followed — past an optional parameter list, `mutable` / `noexcept` / a
    /// trailing return type — by `{`. That `]`-then-`(`-or-`{` shape is what tells a lambda apart from
    /// a subscript `a[i]` and from an attribute `[[nodiscard]]`.
    /// Only meaningful at function scope; a lambda anywhere else has no locals we report on differently.
    void descend_lambdas(isize open, isize after, decl_scope scope, isize parent)
    {
        if (scope != decl_scope::function_scope)
            return;

        for (isize i = open; i + 1 < after && !is_eof(i); ++i)
        {
            if (!punct(i, "]"))
                continue;

            isize j = i + 1;
            if (punct(j, "(")) // the parameter list
                j = skip_balanced(j, "(", ")");
            while (j < after && !is_eof(j) && !punct(j, "{") && !punct(j, ";")) // mutable / noexcept / -> T
                j = punct(j, "(") ? skip_balanced(j, "(", ")") : j + 1;

            if (j >= after || !punct(j, "{"))
                continue;

            auto const body_end = skip_balanced(j, "{", "}");
            parse_scope(j + 1, body_end - 1, decl_scope::function_scope, parent);
            i = body_end - 1; // the body is parsed; do not rescan it for introducers
        }
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

        parse_scope(open_brace + 1, body_close, decl_scope::record_scope, id);

        isize j = body_close + 1;
        while (j < end && !is_eof(j) && !punct(j, ";"))
            ++j;
        return (j < end && punct(j, ";")) ? j + 1 : j;
    }

    /// Emit one brace-initialized declarator. `stmt_begin` / `stmt_last` bound the whole statement (the
    /// node's `span`), `declarator_index` names it, and `open_brace` opens its initializer.
    void add_brace_var(isize stmt_begin,
                       isize stmt_last,
                       isize declarator_index,
                       isize open_brace,
                       decl_scope scope,
                       isize parent)
    {
        auto const close_brace = skip_balanced(open_brace, "{", "}") - 1;

        node vn;
        vn.kind = node_kind::variable_declaration;
        vn.scope = scope;
        vn.form = init_form::brace;
        vn.name = tk(declarator_index).span;
        // Everything from the declarator-id up to the brace belongs to the declarator — an array bound
        // above all. A rewrite that starts at the id's end instead would eat the `[N]`.
        vn.declarator = source_span::join(tk(declarator_index).span, tk(open_brace - 1).span);
        vn.init_span = source_span::join(tk(open_brace).span, tk(close_brace).span);
        vn.init_inner = {.file_id = file_id,
                         .byte_begin = tk(open_brace).span.byte_end,
                         .byte_end = tk(close_brace).span.byte_begin};
        vn.span = source_span::join(tk(stmt_begin).span, tk(stmt_last).span);

        auto const id = add_node(cc::move(vn));
        tree.nodes[parent].children.push_back(id);
    }

    /// `open_brace` is an initializer '{'. Emit a variable_declaration for it and for every FURTHER
    /// declarator in the same statement that is also brace-initialized, then run to the ';'.
    ///
    /// Outside a record body a brace group is only a declaration when the segment actually looks like
    /// one: at least a type and a declarator ahead of the brace (so the temporary `T{1};` is not read as
    /// a declaration of `T`), and no statement keyword in front of it.
    isize finish_brace_init(isize begin,
                            isize open_brace,
                            isize end,
                            decl_scope scope,
                            isize declarator_index,
                            isize top_token_count,
                            bool stmt_head,
                            isize parent)
    {
        bool const looks_like_declaration
            = declarator_index >= 0 && !stmt_head && (scope == decl_scope::record_scope || top_token_count >= 2);

        // Run to the statement's ';', skipping any further balanced group. Along the way, a top-level ','
        // starts the next declarator — `int a{1}, b{2};` declares two variables, and only re-running the
        // brace-vs-`=`-vs-`;` decision per declarator keeps `int a{1}, b = 2, c;` down to just `a`.
        auto const first_close = skip_balanced(open_brace, "{", "}") - 1;
        auto braces = cc::vector<isize>(); // one open-brace index per brace-initialized declarator
        auto names = cc::vector<isize>();  // its declarator-id
        braces.push_back(open_brace);
        names.push_back(declarator_index);

        isize j = first_close + 1;
        while (j < end && !is_eof(j) && !punct(j, ";"))
        {
            if (punct(j, ","))
            {
                ++j;
                j = scan_next_declarator(j, end, looks_like_declaration ? &braces : nullptr,
                                         looks_like_declaration ? &names : nullptr);
                continue;
            }
            if (punct(j, "{"))
                j = skip_balanced(j, "{", "}");
            else if (punct(j, "("))
                j = skip_balanced(j, "(", ")");
            else if (punct(j, "["))
                j = skip_balanced(j, "[", "]");
            else
                ++j;
        }
        auto const semi = j;

        if (looks_like_declaration)
        {
            auto const last = (semi < end && !is_eof(semi)) ? semi : first_close;
            for (auto k = isize(0); k < braces.size(); ++k)
                this->add_brace_var(begin, last, names[k], braces[k], scope, parent);
        }

        return (semi < end && punct(semi, ";")) ? semi + 1 : semi;
    }

    /// Consume one declarator of a comma-separated declaration, starting just past the ','. Records it in
    /// `braces` / `names` (unless null) when it is brace-initialized; an `=` or a bare declarator records
    /// nothing. Returns the index of whatever terminated it — never consuming that token, so the caller's
    /// loop still sees the `=`, `,` or `;`.
    isize scan_next_declarator(isize from, isize end, cc::vector<isize>* braces, cc::vector<isize>* names)
    {
        auto name_index = isize(-1);

        isize j = from;
        while (j < end && !is_eof(j) && !punct(j, ";") && !punct(j, ",") && !punct(j, "=") && !punct(j, "{"))
        {
            if (punct(j, "[")) // an array bound is part of the declarator
                j = skip_balanced(j, "[", "]");
            else if (punct(j, "(")) // a parenthesized init / function declarator: opaque, and not brace form
                j = skip_balanced(j, "(", ")");
            else
            {
                if (tk(j).is(token_kind::identifier))
                    name_index = j; // the last identifier is the declarator-id, as in scan_one
                ++j;
            }
        }

        if (j < end && punct(j, "{") && name_index >= 0 && braces != nullptr)
        {
            braces->push_back(j);
            names->push_back(name_index);
            return skip_balanced(j, "{", "}");
        }
        return j;
    }

    /// Skip an assignment initializer up to and including its ';' (balanced groups skipped). At function
    /// scope the skipped groups are still walked for lambda bodies — `auto f = [] { int y{0}; };` hides
    /// real locals behind a '=' that would otherwise swallow them.
    isize run_to_semicolon(isize from, isize end, decl_scope scope, isize parent)
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

        // The initializer is opaque except for one thing: a lambda body inside it is real code holding
        // real locals (`auto f = [] { int y{0}; };`), so the whole initializer is swept for introducers.
        descend_lambdas(from, j, scope, parent);

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
    p.parse_scope(0, eof_index, decl_scope::namespace_scope, p.tree.root);

    return cc::move(p.tree);
}
} // namespace scl
