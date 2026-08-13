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

/// The statement forms that carry a parenthesized header: `if (…)`, `switch (…)`, `while (…)`, `for (…)`.
/// That header is a declaration scope: its init-statement and its condition may each declare a local.
/// `for (int i{0}; …)` and `if (auto x{g()}; x > 0)` are the two shapes.
///
/// `do` is deliberately absent even though it ends in `while ( … )`: the grammar makes that an expression, never a declaration.
/// `catch` is absent because its body is always a compound statement, so the generic path already handles it.
bool is_control_flow_keyword(token const& t)
{
    return t.is_keyword("if") || t.is_keyword("switch") || t.is_keyword("while") || t.is_keyword("for");
}

/// The punctuation that may appear at top level in a declaration ahead of its initializer.
/// Name qualification, pointer/reference declarators, the `,` of a multi-declarator statement, an access specifier's or mem-initializer's `:`, a destructor `~`, a pack `...`.
///
/// Anything else — `+=`, `.`, `->`, `?`, a comparison — makes the segment an expression, and then a brace group in it is a temporary rather than an initializer.
/// `s += cc::string_view{" world"}` is the shape this catches: it has tokens ahead of the qualified name, so "a type is in front of it" is not enough.
///
/// The closers `)`, `]`, `>` are deliberately absent even though a declaration contains them.
/// Each is consumed by the balanced skip that its opener started, so reaching one here means the segment began mid-expression.
/// Listing `>` as allowed would let `a > T{1};` read as declaring `T`.
bool is_declaration_punct(token const& t)
{
    for (auto const p : {"::", "*", "&", "&&", ",", ":", "~", "..."})
        if (t.is_punct(p))
            return true;
    return false;
}

/// A statement can carry a brace group that is an expression, not an initializer — `return T{1};`, `throw e{2};`, `case k:`.
/// Seeing one of these at the top of a segment disqualifies the whole segment from being a declaration.
/// That is what keeps function-scope parsing free of false positives.
bool is_statement_keyword(token const& t)
{
    return t.is_keyword("return") || t.is_keyword("throw") || t.is_keyword("co_return") || t.is_keyword("co_yield")
        || t.is_keyword("co_await") || t.is_keyword("case") || t.is_keyword("default") || t.is_keyword("else")
        || t.is_keyword("do") || t.is_keyword("try") || t.is_keyword("catch") || t.is_keyword("goto")
        || t.is_keyword("break") || t.is_keyword("continue") || t.is_keyword("new") || t.is_keyword("delete")
        || t.is_keyword("using");
}

/// The recursive-descent driver over a flat array of significant tokens (trivia already dropped).
/// It walks declaration-by-declaration, tracking bracket depth by skipping balanced groups, and emits record and variable nodes into an arena.
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

    /// Given `open` at a `o`/`c` bracket pair, return the index just past the matching close.
    /// Nesting on the same bracket is tracked.
    /// Runs to end on an unbalanced input (best-effort).
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

    /// Given `open` at a `<`, return the index just past the matching `>`.
    /// A `>>` token closes two levels.
    /// Bails at a `;` or `{` so a stray `<` (not really a template) cannot run away.
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

    /// The first index of the `::`-joined name run ending at `id_index`, clamped to `begin`.
    /// `cc::a::b` walks back from `b` to `cc`, a leading `::` included; an unqualified id is its own run.
    /// This is what tells a type-plus-declarator apart from a single qualified name.
    /// In `cc::atomic<int> x` the run at `x` is just `x`, with the type ahead of it; in `cc::void_function` the run is the whole segment and no type is left over.
    isize qualified_name_begin(isize id_index, isize begin) const
    {
        isize i = id_index;
        while (i - 1 >= begin && punct(i - 1, "::"))
        {
            if (i - 2 >= begin && tk(i - 2).is(token_kind::identifier))
                i -= 2; // one more qualifier
            else
                return i - 1; // a leading `::` — the run is rooted at global scope
        }
        return i;
    }

    /// Parse the declarations in the half-open index range [begin, end).
    /// `scope` is what a brace-initialized declaration found here becomes — a data member, a namespace-scope variable, or a local.
    /// Records, namespaces, function bodies, nested blocks and lambda bodies are all descended.
    void parse_scope(isize begin, isize end, decl_scope scope, isize parent)
    {
        // Which type definitions sit next to each other is what tells a rule how big a block moves out of the namespace, so the parser answers that here.
        // Everything it does not model — a function, a macro, a preprocessor line, a nested namespace, a forward declaration — breaks the run, which is the safe direction.
        bool const track = tree.nodes[parent].kind == node_kind::namespace_definition;

        bool prev_was_definition = false;
        isize pos = begin;
        while (pos < end && !is_eof(pos))
        {
            auto const children_before = tree.nodes[parent].children.size();
            pos = scan_one(pos, end, scope, parent);

            auto const is_definition = added_one_definition(parent, children_before);
            if (track && is_definition && prev_was_definition)
                tree.nodes[tree.nodes[parent].children.back()].follows_definition = true;
            prev_was_definition = is_definition;
        }
    }

    /// Whether the statement just scanned contributed exactly one record or enum definition to `parent`.
    bool added_one_definition(isize parent, isize children_before) const
    {
        auto const& children = tree.nodes[parent].children;
        if (children.size() != children_before + 1)
            return false;
        auto const kind = tree.nodes[children.back()].kind;
        return kind == node_kind::record_definition || kind == node_kind::enum_definition;
    }

    /// Consume exactly one declaration or statement starting at `begin`; return the index just past it.
    isize scan_one(isize begin, isize end, decl_scope scope, isize parent)
    {
        // A using-directive nominates a namespace for unqualified lookup over the rest of the enclosing scope.
        // A using-declaration (`using cc::vector;`) and a type alias (`using u = cc::u32;`) nominate nothing.
        // Requiring the `namespace` keyword is what separates the three.
        if (kw(begin, "using") && kw(begin + 1, "namespace"))
            return finish_using_directive(begin, end, parent);

        // A namespace: emit the node, descend its body as a declaration scope; skip an alias (`namespace A = B;`).
        // A leading `inline` belongs to the same declaration — without it the body's '{' reaches the generic scanner and reads as an initializer brace on the namespace name.
        isize const ns_kw = (kw(begin, "inline") && kw(begin + 1, "namespace")) ? begin + 1 : begin;
        if (kw(ns_kw, "namespace"))
        {
            for (isize i = ns_kw + 1; i < end && !is_eof(i); ++i)
            {
                if (punct(i, "{"))
                {
                    auto const body_close = skip_balanced(i, "{", "}") - 1; // index of the matching '}'
                    auto const id = add_namespace(begin, ns_kw, i, body_close, parent);
                    parse_scope(i + 1, body_close, decl_scope::namespace_scope, id);
                    return body_close + 1;
                }
                if (punct(i, ";"))
                    return i + 1;
            }
            return end;
        }

        // A control-flow statement is a form of its own — header, then a body that is one statement.
        // Reaching it here rather than letting its paren group make the following '{' look like a
        // function body is what makes a header declaration visible and a braceless body reachable.
        if (is_control_flow_keyword(tk(begin)) || kw(begin, "else") || kw(begin, "do"))
            return scan_control_flow(begin, end, parent);

        bool paren_group_seen = false;
        bool def_head = false; // saw a type-defining keyword (class/struct/union/enum) at top level
        bool is_record = false;
        bool is_enum = false;
        bool enum_scoped = false;
        isize enum_base_begin = -1; // first token of the enum-base, -1 when none was written
        // Saw something that rules this segment out as a declaration: a statement keyword, or punctuation
        // that only an expression carries.
        bool not_a_declaration = false;
        record_keyword rec_kw = record_keyword::struct_;
        isize def_name_index = -1; // the name a record or enum head spells
        bool expect_def_name = false;
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
                expect_def_name = true;
                prev_top_index = pos;
                ++pos;
                continue;
            }
            if (t.is_keyword("enum"))
            {
                def_head = true;
                is_enum = true;
                prev_top_index = pos;
                ++pos;
                if (kw(pos, "class") || kw(pos, "struct")) // `enum class` / `enum struct`
                {
                    enum_scoped = true;
                    ++pos;
                }
                expect_def_name = true;
                continue;
            }
            // An enum-base ends the head's name, so the type behind the ':' is never mistaken for it — `enum : u8 { … }` stays anonymous.
            // Whether one was written is what decides if an unscoped enum can be declared ahead of its definition at all.
            if (is_enum && t.is_punct(":"))
            {
                enum_base_begin = pos + 1;
                expect_def_name = false;
                prev_top_index = pos;
                ++pos;
                continue;
            }
            if (is_statement_keyword(t))
                not_a_declaration = true;

            if (t.is_punct("{"))
            {
                if (paren_group_seen || def_head)
                {
                    if (is_record)
                        return finish_record(begin, pos, end, rec_kw, def_name_index, scope, parent);

                    // Only the enum's own enumerator list, never a function body that merely has an enum in its parameter list — `void f(enum e x) { }`.
                    if (is_enum && !paren_group_seen)
                        return finish_enum(begin, pos, end, enum_scoped, enum_base_begin, def_name_index, scope, parent);

                    auto after = skip_balanced(pos, "{", "}");

                    // A constructor's mem-initializer list looks exactly like this: `S() : a{1}, b{2} {}`.
                    // Only the last brace group of the segment is the body: a mem-initializer is always followed by ',' or by that body's '{', so keep scanning while either does.
                    if (after < end && !is_eof(after) && (punct(after, ",") || punct(after, "{")))
                    {
                        prev_top_index = after - 1;
                        pos = after;
                        continue;
                    }

                    // The definition body.
                    // A function body is real code we descend into; an enum body (def_head without a paren group) holds no declarations we model.
                    if (!def_head)
                        parse_scope(pos + 1, after - 1, decl_scope::function_scope, parent);

                    if (after < end && punct(after, ";"))
                        ++after;
                    return after;
                }

                // At function scope a brace group with no declarator in front of it, or one behind a statement keyword, is never an initializer.
                // It is a nested block (`{ … }`, an `else` / `do` / `try` body) or an expression (`return P{a, b};`).
                // Descend rather than running past it looking for a ';' that belongs to a later statement.
                if (scope == decl_scope::function_scope && (declarator_index < 0 || not_a_declaration))
                {
                    auto const after = skip_balanced(pos, "{", "}");
                    parse_scope(pos + 1, after - 1, decl_scope::function_scope, parent);
                    return after;
                }

                return finish_brace_init(begin, pos, end, scope, declarator_index, not_a_declaration, parent);
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
                if (expect_def_name)
                {
                    def_name_index = pos;
                    expect_def_name = false;
                }
                declarator_index = pos;
            }
            else if (t.is(token_kind::punctuation) && !is_declaration_punct(t))
                not_a_declaration = true; // an operator at top level — this segment is an expression

            prev_top_index = pos;
            ++pos;
        }
        return pos;
    }

    /// The `:` that splits a range-for header into declaration and range, or -1 when there is none.
    /// `::` lexes whole, so only a real `:` is seen.
    /// A conditional's `:` is paired off against its `?`, which is what keeps `for (int i = c ? a : b; …)` a plain three-clause `for`.
    isize range_for_colon(isize begin, isize end) const
    {
        isize conditionals = 0;
        for (isize i = begin; i < end && !is_eof(i); ++i)
        {
            if (punct(i, "("))
                i = skip_balanced(i, "(", ")") - 1;
            else if (punct(i, "["))
                i = skip_balanced(i, "[", "]") - 1;
            else if (punct(i, "{"))
                i = skip_balanced(i, "{", "}") - 1;
            else if (punct(i, "?"))
                ++conditionals;
            else if (punct(i, ":"))
            {
                if (conditionals == 0)
                    return i;
                --conditionals;
            }
        }
        return -1;
    }

    /// The body of a control-flow statement: a compound statement whose contents are a scope, or — the braceless form — exactly one statement.
    /// That one statement may itself be a declaration, as in `if (c) int y{0};`.
    isize scan_statement_body(isize pos, isize end, isize parent)
    {
        if (punct(pos, "{"))
        {
            auto const after = skip_balanced(pos, "{", "}");
            parse_scope(pos + 1, after - 1, decl_scope::function_scope, parent);
            return after;
        }
        return scan_one(pos, end, decl_scope::function_scope, parent);
    }

    /// `if` / `switch` / `while` / `for`, whose header is a declaration scope.
    /// Plus the two bodies that stand alone: `else`, and `do`'s body ahead of its trailing `while ( expression ) ;`.
    ///
    /// The header is handed to parse_scope, so a `for`'s three clauses and an `if`'s init-statement are each read as a statement.
    /// A declaration where one is written, an expression otherwise.
    /// `do`'s trailing parens are an expression by the grammar and are only swept for lambda bodies.
    /// So is a range-for's range: the `{…}` in `for (auto p : {"a", "b"})` initializes the range, not `p`.
    isize scan_control_flow(isize begin, isize end, isize parent)
    {
        if (kw(begin, "else"))
            return scan_statement_body(begin + 1, end, parent);

        if (kw(begin, "do"))
        {
            auto pos = scan_statement_body(begin + 1, end, parent);
            if (!kw(pos, "while"))
                return pos;

            ++pos;
            if (punct(pos, "("))
            {
                auto const after = skip_balanced(pos, "(", ")");
                descend_lambdas(pos, after, decl_scope::function_scope, parent);
                pos = after;
            }
            return (pos < end && punct(pos, ";")) ? pos + 1 : pos;
        }

        auto pos = begin + 1;
        if (punct(pos, "!")) // `if !consteval`
            ++pos;
        if (kw(pos, "constexpr") || kw(pos, "consteval")) // `if constexpr`, `if consteval` — no header
            ++pos;

        if (punct(pos, "("))
        {
            auto const after = skip_balanced(pos, "(", ")");
            auto const header_end = after - 1; // the ')'
            // A range-for splits at its `:`. Only the declaration ahead of it is a scope; behind it is
            // the range, whose braced-init-list belongs to no declarator.
            auto const colon = kw(begin, "for") ? range_for_colon(pos + 1, header_end) : isize(-1);
            parse_scope(pos + 1, colon >= 0 ? colon : header_end, decl_scope::function_scope, parent);
            if (colon >= 0)
                descend_lambdas(colon, header_end, decl_scope::function_scope, parent);
            pos = after;
        }
        return scan_statement_body(pos, end, parent);
    }

    /// Walk a group we are otherwise skipping and descend into any lambda body inside it.
    /// A lambda introducer is a `]` followed — past an optional parameter list, `mutable` / `noexcept` / a trailing return type — by `{`.
    /// That `]`-then-`(`-or-`{` shape is what tells a lambda apart from a subscript `a[i]` and from an attribute `[[nodiscard]]`.
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

    /// Emit the namespace_definition for `namespace <name> { … }` and return its node id, which becomes the parent of everything the body declares.
    /// The name is the token run between the keyword and the '{', empty for an anonymous namespace.
    /// The nested-name form `a::b` is kept as written, since being inside `cc::impl` is being inside `cc`.
    isize add_namespace(isize stmt_begin, isize ns_kw, isize open_brace, isize body_close, isize parent)
    {
        isize name_begin = ns_kw + 1;
        if (name_begin < open_brace && punct(name_begin, "[")) // `namespace [[deprecated]] a {`
            name_begin = skip_balanced(name_begin, "[", "]");

        node nn;
        nn.kind = node_kind::namespace_definition;
        nn.name = {.file_id = file_id}; // an anonymous namespace has no name, but the empty span is still this file's
        if (name_begin < open_brace)
            nn.name = source_span::join(tk(name_begin).span, tk(open_brace - 1).span);
        nn.body = source_span::join(tk(open_brace).span, tk(body_close).span);
        nn.span = source_span::join(tk(stmt_begin).span, tk(body_close).span);

        auto const id = add_node(cc::move(nn));
        tree.nodes[parent].children.push_back(id);
        return id;
    }

    /// Emit the using_directive for `using namespace <name>;` starting at `begin`; return past the ';'.
    isize finish_using_directive(isize begin, isize end, isize parent)
    {
        isize semi = begin + 2;
        while (semi < end && !is_eof(semi) && !punct(semi, ";"))
            ++semi;
        auto const last = (semi < end && !is_eof(semi)) ? semi : semi - 1; // the ';', or all we got

        node un;
        un.kind = node_kind::using_directive;
        un.name = {.file_id = file_id};
        if (semi > begin + 2)
            un.name = source_span::join(tk(begin + 2).span, tk(semi - 1).span);
        un.span = source_span::join(tk(begin).span, tk(last).span);
        // In force from past the ';' to the end of the enclosing scope.
        // `tk(end)` is that scope's closer — the '}' of a namespace, record or function body, the end_of_file sentinel at file scope.
        auto const from = un.span.byte_end;
        auto const to = tk(end).span.byte_begin;
        un.effect = {.file_id = file_id, .byte_begin = from, .byte_end = to > from ? to : from};

        auto const id = add_node(cc::move(un));
        tree.nodes[parent].children.push_back(id);
        return (semi < end && punct(semi, ";")) ? semi + 1 : semi;
    }

    /// `open_brace` is the record body '{'.
    /// Emit the record_definition, recurse into its body, and consume any trailing declarator up to the terminating ';'.
    isize finish_record(isize begin,
                        isize open_brace,
                        isize end,
                        record_keyword rec_kw,
                        isize rec_name_index,
                        decl_scope scope,
                        isize parent)
    {
        auto const body_close = skip_balanced(open_brace, "{", "}") - 1; // matching '}'

        node rn;
        rn.kind = node_kind::record_definition;
        rn.rec_keyword = rec_kw;
        rn.scope = scope;
        rn.name = {.file_id = file_id}; // likewise for an anonymous record
        if (rec_name_index >= 0)
            rn.name = tk(rec_name_index).span;
        rn.span = source_span::join(tk(begin).span, tk(body_close).span);
        auto const id = add_node(cc::move(rn));
        tree.nodes[parent].children.push_back(id);

        parse_scope(open_brace + 1, body_close, decl_scope::record_scope, id);

        isize j = body_close + 1;
        while (j < end && !is_eof(j) && !punct(j, ";"))
            ++j;
        tree.nodes[id].has_declarator = j > body_close + 1;
        return (j < end && punct(j, ";")) ? j + 1 : j;
    }

    /// `open_brace` is the enumerator list's '{'; `base_begin` the first token of the enum-base, or -1 when none was written.
    /// Emit the enum_definition and consume any trailing declarator up to the terminating ';'.
    ///
    /// The list itself is not descended: an enumerator is not a declaration any rule models, and `= v` in one is an expression rather than an initializer.
    isize finish_enum(isize begin,
                      isize open_brace,
                      isize end,
                      bool scoped,
                      isize base_begin,
                      isize name_index,
                      decl_scope scope,
                      isize parent)
    {
        auto const body_close = skip_balanced(open_brace, "{", "}") - 1; // matching '}'

        node en;
        en.kind = node_kind::enum_definition;
        en.scope = scope;
        en.enum_scoped = scoped;
        en.enum_has_base = base_begin >= 0;
        // A base of exactly one identifier is an unqualified name, so it resolved through whatever scope the definition sits in.
        // That is what a rewrite moving the definition out of its namespace has to carry along; `cc::u8`, `int` and `unsigned char` all resolve on their own and leave this empty.
        en.enum_base_name = {.file_id = file_id};
        if (base_begin >= 0 && base_begin == open_brace - 1 && tk(base_begin).is(token_kind::identifier))
            en.enum_base_name = tk(base_begin).span;
        en.name = {.file_id = file_id}; // an anonymous enum has none
        if (name_index >= 0)
            en.name = tk(name_index).span;
        en.span = source_span::join(tk(begin).span, tk(body_close).span);
        auto const id = add_node(cc::move(en));
        tree.nodes[parent].children.push_back(id);

        isize j = body_close + 1;
        while (j < end && !is_eof(j) && !punct(j, ";"))
            ++j;
        tree.nodes[id].has_declarator = j > body_close + 1;
        return (j < end && punct(j, ";")) ? j + 1 : j;
    }

    /// Emit one brace-initialized declarator.
    /// `stmt_begin` / `stmt_last` bound the whole statement (the node's `span`), `declarator_index` names it, and `open_brace` opens its initializer.
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
        // Everything from the declarator-id up to the brace belongs to the declarator, an array bound above all.
        // A rewrite that starts at the id's end instead would eat the `[N]`.
        vn.declarator = source_span::join(tk(declarator_index).span, tk(open_brace - 1).span);
        vn.init_span = source_span::join(tk(open_brace).span, tk(close_brace).span);
        vn.init_inner = {.file_id = file_id,
                         .byte_begin = tk(open_brace).span.byte_end,
                         .byte_end = tk(close_brace).span.byte_begin};
        vn.span = source_span::join(tk(stmt_begin).span, tk(stmt_last).span);

        auto const id = add_node(cc::move(vn));
        tree.nodes[parent].children.push_back(id);
    }

    /// `open_brace` is an initializer '{'.
    /// Emit a variable_declaration for it and for every further declarator in the same statement that is also brace-initialized, then run to the ';'.
    ///
    /// A brace group is only a declaration when the segment actually looks like one.
    /// That is: a declarator-id, with a type still ahead of it, and nothing in front that only an expression carries.
    ///
    /// "A type ahead of it" is what the declarator-id's qualified-name run decides, and counting tokens cannot.
    /// `cc::void_function{}()` has three tokens before the brace, yet they are one qualified name with nothing left over for a type.
    /// It is a temporary being called, not a declaration of `void_function`.
    /// Same for the bare temporaries `T{1};`, `cc::T{1};` and `cc::vector<int>{1, 2};`.
    /// An out-of-line static member definition (`int S::x{0};`) keeps its `::` and stays a declaration, because the run at `x` starts after the leading `int`.
    ///
    /// The run alone is not enough, though — `s += cc::string_view{" world"}` has tokens ahead of the qualified name too.
    /// `not_a_declaration` is the other half: see `is_declaration_punct`.
    isize finish_brace_init(isize begin,
                            isize open_brace,
                            isize end,
                            decl_scope scope,
                            isize declarator_index,
                            bool not_a_declaration,
                            isize parent)
    {
        bool const looks_like_declaration
            = declarator_index >= 0 && !not_a_declaration && qualified_name_begin(declarator_index, begin) > begin;

        // Run to the statement's ';', skipping any further balanced group.
        // Along the way, a top-level ',' starts the next declarator: `int a{1}, b{2};` declares two variables.
        // Only re-running the brace-vs-`=`-vs-`;` decision per declarator keeps `int a{1}, b = 2, c;` down to just `a`.
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

    /// Consume one declarator of a comma-separated declaration, starting just past the ','.
    /// Records it in `braces` / `names` (unless null) when it is brace-initialized; an `=` or a bare declarator records nothing.
    /// Returns the index of whatever terminated it, never consuming that token — so the caller's loop still sees the `=`, `,` or `;`.
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

    /// Skip an assignment initializer up to and including its ';' (balanced groups skipped).
    /// At function scope the skipped groups are still walked for lambda bodies: `auto f = [] { int y{0}; };` hides real locals behind a '=' that would otherwise swallow them.
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

        // The initializer is opaque except for one thing: a lambda body inside it is real code holding real locals (`auto f = [] { int y{0}; };`).
        // So the whole initializer is swept for introducers.
        descend_lambdas(from, j, scope, parent);

        return (j < end && punct(j, ";")) ? j + 1 : j;
    }
};
} // namespace

cc::result<syntax_tree> parse(source_buffer const& buffer, token_stream const& tokens)
{
    parser_impl p;
    p.file_id = buffer.file_id();

    // Significant tokens only.
    // Preprocessor directives are dropped here too: they are opaque to the grammar, so leaving one in would glue the following declaration onto it.
    // A `#include` / `#pragma` has no `;`, and at file scope that makes the first `namespace {` look like an initializer brace.
    // The directives remain in the token stream for future macro-placement rules.
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
