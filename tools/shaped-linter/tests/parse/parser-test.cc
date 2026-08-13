#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <nexus/test.hh>
#include <shaped-linter/lex/lexer.hh>
#include <shaped-linter/lex/source_buffer.hh>
#include <shaped-linter/parse/parser.hh>

using namespace scl;

namespace
{
/// Owns the buffer, tokens, and tree together (spans are offsets, but text lookups need the buffer).
struct parsed
{
    cc::unique_ptr<source_buffer> buf;
    token_stream ts;
    syntax_tree tree;

    /// Every brace-form variable declaration in the tree, in any scope.
    cc::vector<node const*> brace_vars() const
    {
        cc::vector<node const*> out;
        for (auto const& n : tree.nodes)
            if (n.kind == node_kind::variable_declaration && n.form == init_form::brace)
                out.push_back(&n);
        return out;
    }

    /// Every brace-form variable declaration in a given scope.
    cc::vector<node const*> brace_vars_in(decl_scope scope) const
    {
        cc::vector<node const*> out;
        for (auto const* n : brace_vars())
            if (n->scope == scope)
                out.push_back(n);
        return out;
    }

    isize record_count() const { return nodes_of(node_kind::record_definition).size(); }

    cc::vector<node const*> nodes_of(node_kind k) const
    {
        cc::vector<node const*> out;
        for (auto const& n : tree.nodes)
            if (n.kind == k)
                out.push_back(&n);
        return out;
    }

    cc::string_view text(source_span s) const { return buf->span_text(s); }
};

parsed parse_text(cc::string_view s)
{
    auto buf = cc::make_unique<source_buffer>(source_buffer::from_text(cc::string(s), "<mem>", 0));
    auto ts = lex(*buf).value();
    auto tree = parse(*buf, ts).value();
    return {.buf = cc::move(buf), .ts = cc::move(ts), .tree = cc::move(tree)};
}
} // namespace

TEST("shaped-linter - parser - the chase_lev_deque atomic member")
{
    auto const p = parse_text("struct S { alignas(64) cc::atomic<cc::i64> _top{0}; };");
    auto const m = p.brace_vars();
    REQUIRE(m.size() == 1);
    CHECK(p.text(m[0]->name) == "_top");
    CHECK(p.text(m[0]->init_inner) == "0");
    CHECK(p.text(m[0]->init_span) == "{0}");
    CHECK(p.record_count() == 1);
}

TEST("shaped-linter - parser - several atomic members")
{
    auto const p = parse_text("struct S {\n"
                              "  alignas(64) cc::atomic<cc::i64> _top{0};\n"
                              "  alignas(64) cc::atomic<cc::i64> _bottom{0};\n"
                              "  alignas(64) cc::atomic<ring*> _ring{nullptr};\n"
                              "};");
    auto const m = p.brace_vars();
    REQUIRE(m.size() == 3);
    CHECK(p.text(m[0]->name) == "_top");
    CHECK(p.text(m[1]->name) == "_bottom");
    CHECK(p.text(m[2]->name) == "_ring");
    CHECK(p.text(m[2]->init_inner) == "nullptr");
}

TEST("shaped-linter - parser - assignment form is not a brace init")
{
    auto const p = parse_text("struct S { int x = 0; cc::atomic<int> y = 0; };");
    CHECK(p.brace_vars().size() == 0);
    CHECK(p.record_count() == 1);
}

TEST("shaped-linter - parser - empty and multi-element braces")
{
    SECTION("empty brace")
    {
        auto const p = parse_text("struct S { int y{}; };");
        auto const m = p.brace_vars();
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "y");
        CHECK(p.text(m[0]->init_inner) == "");
    }
    SECTION("multi-element brace")
    {
        auto const p = parse_text("struct S { P p{1, 2}; };");
        auto const m = p.brace_vars();
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "p");
        CHECK(p.text(m[0]->init_inner) == "1, 2");
    }
    SECTION("static inline member")
    {
        auto const p = parse_text("struct S { static inline cc::atomic<int> live{0}; };");
        auto const m = p.brace_vars();
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "live");
    }
    SECTION("mutable member")
    {
        auto const p = parse_text("struct S { mutable cc::atomic<cc::i64> _c{0}; };");
        auto const m = p.brace_vars();
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "_c");
    }
}

TEST("shaped-linter - parser - the declarator span reaches past an array bound")
{
    // `name` identifies the variable; `declarator` is where an initializer rewrite may start.
    // They differ exactly when a suffix sits between the two — an array bound — and conflating them drops it.
    SECTION("array member")
    {
        auto const p = parse_text("struct S { T a[N]{1, 2}; };");
        auto const m = p.brace_vars();
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "a");
        CHECK(p.text(m[0]->declarator) == "a[N]");
    }
    SECTION("two-dimensional array member")
    {
        auto const p = parse_text("struct S { T a[2][3]{1}; };");
        auto const m = p.brace_vars();
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->declarator) == "a[2][3]");
    }
    SECTION("no suffix: declarator is just the name")
    {
        auto const p = parse_text("struct S { int x{0}; };");
        auto const m = p.brace_vars();
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->declarator) == "x");
    }
    SECTION("whitespace before the brace is not part of the declarator")
    {
        auto const p = parse_text("struct S { int x  {0}; };");
        auto const m = p.brace_vars();
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->declarator) == "x");
    }
}

TEST("shaped-linter - parser - every brace-initialized declarator of a statement is recorded")
{
    SECTION("two members in one declaration")
    {
        auto const p = parse_text("struct S { int a{1}, b{2}; };");
        auto const m = p.brace_vars();
        REQUIRE(m.size() == 2);
        CHECK(p.text(m[0]->name) == "a");
        CHECK(p.text(m[1]->name) == "b");
        CHECK(p.text(m[1]->init_inner) == "2");
    }
    SECTION("three locals in one declaration")
    {
        auto const p = parse_text("void f() { int a{1}, b{2}, c{3}; }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 3);
        CHECK(p.text(m[2]->name) == "c");
    }
    SECTION("a later declarator carries its own array bound")
    {
        auto const p = parse_text("struct S { int a{1}, b[2]{3}; };");
        auto const m = p.brace_vars();
        REQUIRE(m.size() == 2);
        CHECK(p.text(m[1]->declarator) == "b[2]");
    }
    SECTION("a pointer declarator")
    {
        auto const p = parse_text("struct S { int a{1}, *p{nullptr}; };");
        auto const m = p.brace_vars();
        REQUIRE(m.size() == 2);
        CHECK(p.text(m[1]->name) == "p");
    }

    // Only brace-initialized declarators count — the rest of the statement stays as opaque as before.
    SECTION("assignment form and a bare declarator alongside")
    {
        auto const p = parse_text("struct S { int a{1}, b = 2, c; };");
        auto const m = p.brace_vars();
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "a");
    }
    SECTION("a braced assignment initializer is still assignment form")
    {
        auto const p = parse_text("struct S { int a{1}, b = {2, 3}; };");
        auto const m = p.brace_vars();
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "a");
    }
    SECTION("a comma inside the initializer does not start a declarator")
    {
        auto const p = parse_text("struct S { P a{f(1, 2)}, b{3}; };");
        auto const m = p.brace_vars();
        REQUIRE(m.size() == 2);
        CHECK(p.text(m[1]->name) == "b");
    }
}

TEST("shaped-linter - parser - locals inside a function are found, tagged function_scope")
{
    SECTION("plain local")
    {
        auto const p = parse_text("struct S { void f() { int x{0}; } };");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "x");
        CHECK(p.text(m[0]->init_inner) == "0");
    }
    SECTION("static local")
    {
        auto const p = parse_text("struct S { void f() { static cc::atomic<int> s{1}; } };");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "s");
    }
    SECTION("local inside a nested block")
    {
        auto const p = parse_text("void f() { if (c) { int y{2}; } }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "y");
    }
    SECTION("local inside a for body")
    {
        auto const p = parse_text("void f() { for (auto const& x : v) { int y{3}; } }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "y");
    }
    SECTION("free function at file scope")
    {
        auto const p = parse_text("void f() { int y{0}; }");
        CHECK(p.brace_vars_in(decl_scope::function_scope).size() == 1);
    }
}

TEST("shaped-linter - parser - a control-flow header is a declaration scope")
{
    SECTION("for init-statement")
    {
        auto const p = parse_text("void f() { for (int i{0}; i < n; ++i) {} }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "i");
        CHECK(p.text(m[0]->init_inner) == "0");
        // The header is its own scope, so the declaration starts at `int` — not at `for` or at the `(`,
        // either of which would put the keyword into the type the rule reconstructs.
        CHECK(p.text(m[0]->declarator) == "i");
        CHECK(p.text(m[0]->span) == "int i{0};");
    }
    SECTION("for init-statement with two declarators")
    {
        auto const p = parse_text("void f() { for (int i{0}, j{1};;) {} }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 2);
        CHECK(p.text(m[0]->name) == "i");
        CHECK(p.text(m[1]->name) == "j");
    }
    SECTION("if init-statement, alongside the body")
    {
        auto const p = parse_text("void f() { if (auto x{g()}; x > 0) { int y{1}; } }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 2);
        CHECK(p.text(m[0]->name) == "x");
        CHECK(p.text(m[1]->name) == "y");
    }
    SECTION("switch init-statement")
    {
        auto const p = parse_text("void f() { switch (auto v{g()}; v) {} }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "v");
    }
    SECTION("if constexpr init-statement")
    {
        // The token ahead of the `(` is `constexpr`, so the header is found from the statement's keyword
        // rather than from whatever sits in front of the paren group.
        auto const p = parse_text("void f() { if constexpr (auto x{g()}; c) {} }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "x");
    }
    SECTION("assignment form in a condition declares nothing we model")
    {
        CHECK(parse_text("void f() { while (auto e = next()) {} }").brace_vars().size() == 0);
    }
    SECTION("range-for")
    {
        CHECK(parse_text("void f() { for (auto const& x : v) {} }").brace_vars().size() == 0);
    }
    SECTION("range-for over a braced list")
    {
        // The `{…}` behind the `:` initializes the range, not the declarator — a range-declaration carries no initializer at all.
        // Reading it as one is what made the linter report itself.
        CHECK(parse_text("void f() { for (auto const p : {1, 2}) {} }").brace_vars().size() == 0);
        CHECK(parse_text("void f() { for (auto& x : cc::vector<int>{1, 2}) {} }").brace_vars().size() == 0);
    }
    SECTION("a conditional's colon does not split a for header")
    {
        auto const p = parse_text("void f() { for (int i{0}; c ? a : b; ++i) {} }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "i");
    }
    SECTION("a lambda in the range is still reached")
    {
        auto const p = parse_text("void f() { for (auto& x : make([] { int y{0}; return y; }())) {} }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "y");
    }
    SECTION("range-for over a structured binding")
    {
        // A corner-cut, pinned as it behaves: the `[a, b]` is skipped as a balanced group, so no
        // declarator-id is ever seen and the binding is invisible rather than misread.
        CHECK(parse_text("void f() { for (auto [a, b] : m) {} }").brace_vars().size() == 0);
    }
}

TEST("shaped-linter - parser - a control-flow body is one statement, braced or not")
{
    SECTION("braceless if body")
    {
        auto const p = parse_text("void f() { if (c) int y{0}; }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "y");
    }
    SECTION("braceless else body")
    {
        auto const p = parse_text("void f() { if (c) {} else int y{0}; }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "y");
    }
    SECTION("braceless do body")
    {
        auto const p = parse_text("void f() { do int y{0}; while (c); }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "y");
    }
    SECTION("a braceless body is one statement, not the rest of the block")
    {
        // The `T{1}` sits in a call after the body has been consumed, so nothing may read it as a header.
        CHECK(parse_text("void f() { if (c) g(a, T{1}); }").brace_vars().size() == 0);
        CHECK(parse_text("void f() { if (c) g(a); h(b, T{1}); }").brace_vars().size() == 0);
    }
    SECTION("do's trailing parens hold an expression, never a declaration")
    {
        CHECK(parse_text("void f() { do {} while (T{1}); }").brace_vars().size() == 0);
    }
    SECTION("a lambda in a header is reached exactly once")
    {
        // The header is parsed as a scope, which reaches the lambda by itself.
        // Sweeping the same group for lambda bodies on top of that would report every declaration inside it twice.
        auto const p = parse_text("void f() { if (any_of(v, [] { int y{0}; return y; })) {} }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "y");
    }
    SECTION("a lambda at the head of a condition is reached exactly once")
    {
        auto const p = parse_text("void f() { if ([] { int y{0}; return y; }()) {} }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "y");
    }
}

TEST("shaped-linter - parser - a condition is not a declaration")
{
    SECTION("a braced temporary as the whole condition")
    {
        CHECK(parse_text("void f() { if (T{1}) {} }").brace_vars().size() == 0);
        CHECK(parse_text("void f() { switch (cc::T{1}) {} }").brace_vars().size() == 0);
    }
    SECTION("a comparison against a braced temporary")
    {
        CHECK(parse_text("void f() { while (x < T{1}) {} }").brace_vars().size() == 0);
        CHECK(parse_text("void f() { if (x > T{1}) {} }").brace_vars().size() == 0);
    }
    SECTION("a for's second and third clauses are expressions")
    {
        CHECK(parse_text("void f() { for (i = 0; i < n; ++i) {} }").brace_vars().size() == 0);
        CHECK(parse_text("void f() { for (auto it = b; it != e; ++it) {} }").brace_vars().size() == 0);
    }
    SECTION("a comparison behind a `&&`")
    {
        // A corner-cut, pinned as it behaves: `&&` is legal declarator punctuation (an rvalue reference),
        // so `n`'s qualified-name run does not start the segment and `T` reads as a declarator-id.
        // Telling the two apart needs a notion of declarator position, which the parser does not have yet.
        CHECK(parse_text("void f() { if (ok && n < T{1}) {} }").brace_vars().size() == 1);
        CHECK(parse_text("void f() { ok && n < T{1}; }").brace_vars().size() == 1);
    }
}

TEST("shaped-linter - parser - lambda bodies are descended")
{
    SECTION("lambda in an initializer")
    {
        auto const p = parse_text("void f() { auto g = [] { int y{1}; }; }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "y");
    }
    SECTION("lambda with a parameter list")
    {
        auto const p = parse_text("void f() { auto g = [](int a) mutable { int y{2}; }; }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->init_inner) == "2");
    }
    SECTION("lambda as a call argument")
    {
        auto const p = parse_text("void f() { run([] { int y{3}; }); }");
        auto const m = p.brace_vars_in(decl_scope::function_scope);
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->init_inner) == "3");
    }
    SECTION("a capture is not a lambda body")
    {
        // `a[i]` and `[[nodiscard]]` both carry a `]` that must not be read as an introducer.
        auto const p = parse_text("void f() { auto v = a[i]; }");
        CHECK(p.brace_vars().size() == 0);
    }
}

TEST("shaped-linter - parser - constructor init-list is not an initializer")
{
    SECTION("single member")
    {
        auto const p = parse_text("struct S { S() : _x{0} {} int _x; };");
        CHECK(p.brace_vars().size() == 0);
    }
    SECTION("several members")
    {
        auto const p = parse_text("struct S { S() : _a{0}, _b{1} {} int _a; int _b; };");
        CHECK(p.brace_vars().size() == 0);
    }
}

TEST("shaped-linter - parser - expressions in statement position are not declarations")
{
    SECTION("braced return value")
    {
        auto const p = parse_text("P f() { return P{1, 2}; }");
        CHECK(p.brace_vars().size() == 0);
    }
    SECTION("braced temporary as a statement")
    {
        auto const p = parse_text("void f() { T{1}; }");
        CHECK(p.brace_vars().size() == 0);
    }
    SECTION("aggregate at a call site")
    {
        auto const p = parse_text("void f() { g({1, 2}); }");
        CHECK(p.brace_vars().size() == 0);
    }
    SECTION("qualified braced temporary as a statement")
    {
        // `cc::T` is one qualified name, so nothing is left over to be the type — counting the tokens
        // ahead of the brace finds three and reads this as a declaration of `T`.
        auto const p = parse_text("void f() { cc::T{1}; }");
        CHECK(p.brace_vars().size() == 0);
    }
    SECTION("qualified temporary that is immediately called")
    {
        auto const p = parse_text("void f() { cc::void_function{}(); }");
        CHECK(p.brace_vars().size() == 0);
    }
    SECTION("assigning through a called temporary")
    {
        auto const p = parse_text("void f() { cc::identify_function{}(x) = 20; }");
        CHECK(p.brace_vars().size() == 0);
    }
    SECTION("temporary of a template type")
    {
        CHECK(parse_text("void f() { cc::vector<int>{1, 2}; }").brace_vars().size() == 0);
        CHECK(parse_text("void f() { vector<int>{1, 2}; }").brace_vars().size() == 0);
    }
    SECTION("temporary rooted at global scope")
    {
        auto const p = parse_text("void f() { ::cc::T{1}; }");
        CHECK(p.brace_vars().size() == 0);
    }
    SECTION("temporary on the right of a compound assignment")
    {
        // `s +=` puts real tokens ahead of the qualified name, so the qualified-name run alone accepts
        // this; the `+=` is the only thing that says it is an expression.
        auto const p = parse_text("void f() { s += cc::string_view{\" world\"}; }");
        CHECK(p.brace_vars().size() == 0);
    }
    SECTION("other operators in front of a temporary")
    {
        CHECK(parse_text("void f() { total = total + P{1, 2}; }").brace_vars().size() == 0);
        CHECK(parse_text("void f() { obj.field->reset(T{1}); }").brace_vars().size() == 0);
        CHECK(parse_text("void f() { g(c ? P{1} : P{2}); }").brace_vars().size() == 0);
    }
    SECTION("a comparison, whose `>` a declaration only ever reaches inside a template skip")
    {
        CHECK(parse_text("void f() { a > T{1}; }").brace_vars().size() == 0);
        CHECK(parse_text("void f() { a < T{1}; }").brace_vars().size() == 0);
    }
}

TEST("shaped-linter - parser - declarator punctuation does not read as an operator")
{
    // The flip side of the operator check: a pointer, a reference and an access specifier all carry
    // punctuation that a declaration legitimately has, so these must still be found.
    SECTION("pointer and reference declarators")
    {
        CHECK(parse_text("struct S { int* p{nullptr}; };").brace_vars().size() == 1);
        CHECK(parse_text("void f() { int& r{x}; }").brace_vars().size() == 1);
        CHECK(parse_text("struct S { int** pp{nullptr}; };").brace_vars().size() == 1);
    }
    SECTION("an access specifier's colon")
    {
        auto const p = parse_text("class C { public: int x{3}; private: int _y{4}; };");
        CHECK(p.brace_vars().size() == 2);
    }
    SECTION("a second declarator after a comma")
    {
        CHECK(parse_text("struct S { int a, b{2}; };").brace_vars().size() == 1);
    }
}

TEST("shaped-linter - parser - an out-of-line static member definition is a declaration")
{
    // The mirror image of a qualified temporary: the declarator-id carries a `::` too, but `int` ahead of
    // it is the type, so this really is a variable — and the rewrite must not eat the `S::`.
    auto const p = parse_text("int S::x{8};");
    auto const m = p.brace_vars();
    REQUIRE(m.size() == 1);
    CHECK(p.text(m[0]->name) == "x");
    CHECK(p.text(m[0]->declarator) == "x");
    CHECK(p.text(m[0]->init_span) == "{8}");
}

TEST("shaped-linter - parser - namespace-scope variable is found, tagged namespace_scope")
{
    auto const p = parse_text("namespace n { cc::atomic<int> g{0}; }");
    auto const m = p.brace_vars_in(decl_scope::namespace_scope);
    REQUIRE(m.size() == 1);
    CHECK(p.text(m[0]->name) == "g");
    CHECK(p.text(m[0]->init_inner) == "0");
}

TEST("shaped-linter - parser - data members are tagged record_scope")
{
    auto const p = parse_text("struct S { int a{1}; };");
    CHECK(p.brace_vars_in(decl_scope::record_scope).size() == 1);
    CHECK(p.brace_vars_in(decl_scope::function_scope).size() == 0);
}

TEST("shaped-linter - parser - nested record members are found")
{
    auto const p = parse_text("struct O { struct I { int a{1}; }; int b{2}; };");
    auto const m = p.brace_vars();
    REQUIRE(m.size() == 2);
    // Both a (nested) and b (outer) are found, in declaration order.
    CHECK(p.text(m[0]->name) == "a");
    CHECK(p.text(m[1]->name) == "b");
    CHECK(p.record_count() == 2);
}

TEST("shaped-linter - parser - directives before a namespace do not swallow it")
{
    // Regression: a real header opens with `#pragma once` + `#include`s (no `;`), which must not glue
    // onto the following `namespace {` and make its body look like an initializer brace.
    auto const p = parse_text("#pragma once\n"
                              "#include <clean-core/fwd.hh>\n"
                              "#include <type_traits>\n"
                              "\n"
                              "namespace cc::impl\n"
                              "{\n"
                              "template <class T>\n"
                              "struct chase_lev_deque\n"
                              "{\n"
                              "    alignas(64) cc::atomic<cc::i64> _top{0};\n"
                              "};\n"
                              "}\n");
    auto const m = p.brace_vars();
    REQUIRE(m.size() == 1);
    CHECK(p.text(m[0]->name) == "_top");
    CHECK(p.text(m[0]->init_inner) == "0");
}

TEST("shaped-linter - parser - directives between members are skipped")
{
    auto const p = parse_text("struct S {\n"
                              "  int a{1};\n"
                              "#if 0\n"
                              "  int skipped{2};\n"
                              "#endif\n"
                              "  int b{3};\n"
                              "};");
    auto const m = p.brace_vars();
    // The directives are opaque; a/b are found (the #if-disabled member is still parsed — a known limit).
    CHECK(m.size() >= 2);
}

TEST("shaped-linter - parser - static_assert and deleted ops before members")
{
    // Mirrors the chase_lev_deque header shape: static_asserts and `= delete` special members, then data.
    auto const p = parse_text("template <class T>\n"
                              "struct D\n"
                              "{\n"
                              "    static_assert(std::is_trivially_copyable_v<T>, \"msg, with comma\");\n"
                              "    D(D const&) = delete;\n"
                              "    D& operator=(D&&) = delete;\n"
                              "    alignas(64) cc::atomic<cc::i64> _top{0};\n"
                              "};");
    auto const m = p.brace_vars();
    REQUIRE(m.size() == 1);
    CHECK(p.text(m[0]->name) == "_top");
}

TEST("shaped-linter - parser - class and union bodies")
{
    SECTION("class with access specifier")
    {
        auto const p = parse_text("class C { public: int x{3}; private: int _y{4}; };");
        auto const m = p.brace_vars();
        REQUIRE(m.size() == 2);
        CHECK(p.text(m[0]->name) == "x");
        CHECK(p.text(m[1]->name) == "_y");
    }
    SECTION("enum body is not descended")
    {
        auto const p = parse_text("enum class E { A = 1, B = 2 };");
        CHECK(p.brace_vars().size() == 0);
        CHECK(p.record_count() == 0);
    }
}

TEST("shaped-linter - parser - namespace definitions")
{
    SECTION("a plain namespace, with its body")
    {
        auto const p = parse_text("namespace cc\n{\nint x = 1;\n}\n");
        auto const ns = p.nodes_of(node_kind::namespace_definition);
        REQUIRE(ns.size() == 1);
        CHECK(p.text(ns[0]->name) == "cc");
        CHECK(p.text(ns[0]->body) == "{\nint x = 1;\n}");
        CHECK(p.text(ns[0]->span) == "namespace cc\n{\nint x = 1;\n}");
    }
    SECTION("a nested-name namespace keeps the name as written")
    {
        auto const p = parse_text("namespace cc::impl { }");
        auto const ns = p.nodes_of(node_kind::namespace_definition);
        REQUIRE(ns.size() == 1);
        CHECK(p.text(ns[0]->name) == "cc::impl");
    }
    SECTION("an anonymous namespace has an empty name")
    {
        auto const p = parse_text("namespace { int x = 1; }");
        auto const ns = p.nodes_of(node_kind::namespace_definition);
        REQUIRE(ns.size() == 1);
        CHECK(ns[0]->name.empty());
    }
    SECTION("nesting, and the body's declarations parent to the innermost namespace")
    {
        auto const p = parse_text("namespace a { namespace b { struct S { int x{0}; }; } }");
        auto const ns = p.nodes_of(node_kind::namespace_definition);
        REQUIRE(ns.size() == 2);
        CHECK(p.text(ns[0]->name) == "a");
        CHECK(p.text(ns[1]->name) == "b");
        REQUIRE(ns[0]->children.size() == 1);
        CHECK(p.tree[ns[0]->children[0]].kind == node_kind::namespace_definition);
        REQUIRE(ns[1]->children.size() == 1);
        CHECK(p.tree[ns[1]->children[0]].kind == node_kind::record_definition);
    }
    SECTION("an inline namespace is a namespace, not a brace-initialized declarator")
    {
        auto const p = parse_text("inline namespace v1 { int x = 1; }");
        auto const ns = p.nodes_of(node_kind::namespace_definition);
        REQUIRE(ns.size() == 1);
        CHECK(p.text(ns[0]->name) == "v1");
        CHECK(p.brace_vars().size() == 0);
    }
    SECTION("an alias produces no node")
    {
        auto const p = parse_text("namespace A = B::C;");
        CHECK(p.nodes_of(node_kind::namespace_definition).size() == 0);
    }
}

TEST("shaped-linter - parser - follows_definition")
{
    // Which type definitions sit next to each other is what decides how big a block a rule moves out of a namespace.
    // Everything the parser does not model breaks the run, which is the direction that costs a fix rather than correctness.
    auto const runs = [](cc::string_view source)
    {
        auto const p = parse_text(source);
        cc::string out;
        for (auto const& n : p.tree.nodes)
            if (n.kind == node_kind::record_definition || n.kind == node_kind::enum_definition)
                out += n.follows_definition ? '+' : '|';
        return out;
    };

    SECTION("a series of type definitions is one run")
    {
        CHECK(runs("namespace cc { struct a { }; class b { }; union c { }; }") == "|++");
        CHECK(runs("namespace cc { struct a { }; enum class e { }; struct b { }; }") == "|++");
    }
    SECTION("a function breaks the run, defined or merely declared")
    {
        CHECK(runs("namespace cc { struct a { }; void f(); struct b { }; }") == "||");
        CHECK(runs("namespace cc { struct a { }; void f() { } struct b { }; }") == "||");
    }
    SECTION("so does any other declaration")
    {
        CHECK(runs("namespace cc { struct a { }; struct fwd; struct b { }; }") == "||");
        CHECK(runs("namespace cc { struct a { }; enum class e : u8; struct b { }; }") == "||");
        CHECK(runs("namespace cc { struct a { }; using x = int; struct b { }; }") == "||");
        CHECK(runs("namespace cc { struct a { }; int k = 3; struct b { }; }") == "||");
        CHECK(runs("namespace cc { struct a { }; namespace inner { } struct b { }; }") == "||");
    }
    SECTION("a member type is its record's own child and joins no run")
    {
        CHECK(runs("namespace cc { struct a { struct inner { }; }; struct b { }; }") == "||+");
        CHECK(runs("namespace cc { struct a { enum class e { }; }; struct b { }; }") == "||+");
    }
}

TEST("shaped-linter - parser - enum definitions")
{
    auto const only_enum = [](cc::string_view source)
    {
        auto const p = parse_text(source);
        auto const es = p.nodes_of(node_kind::enum_definition);
        REQUIRE(es.size() == 1);
        return es[0];
    };

    SECTION("a scoped enum carries its name and its form")
    {
        auto const p = parse_text("enum class direction : u8 { up, down };");
        auto const es = p.nodes_of(node_kind::enum_definition);
        REQUIRE(es.size() == 1);
        CHECK(p.text(es[0]->name) == "direction");
        CHECK(es[0]->enum_scoped);
        CHECK(es[0]->enum_has_base);
        CHECK(p.text(es[0]->span) == "enum class direction : u8 { up, down }");
    }
    SECTION("`enum struct` is the scoped form too")
    {
        CHECK(only_enum("enum struct e { a };")->enum_scoped);
    }
    SECTION("an unscoped enum is told apart by whether it wrote an enum-base")
    {
        CHECK(!only_enum("enum e { a };")->enum_scoped);
        CHECK(!only_enum("enum e { a };")->enum_has_base);
        CHECK(only_enum("enum e : u8 { a };")->enum_has_base);
        CHECK(only_enum("enum e : cc::u8 { a };")->enum_has_base);
    }
    SECTION("an enum-base of one identifier is a name that resolved through the enclosing scope")
    {
        auto const p = parse_text("enum class e : u8 { a };");
        auto const es = p.nodes_of(node_kind::enum_definition);
        REQUIRE(es.size() == 1);
        CHECK(p.text(es[0]->enum_base_name) == "u8");
    }
    SECTION("a base that resolves on its own carries no name to move")
    {
        CHECK(only_enum("enum class e : int { a };")->enum_base_name.byte_end == 0);
        CHECK(only_enum("enum class e : unsigned char { a };")->enum_base_name.byte_end == 0);
        CHECK(only_enum("enum class e : cc::u8 { a };")->enum_base_name.byte_end == 0);
        CHECK(only_enum("enum class e { a };")->enum_base_name.byte_end == 0);
    }
    SECTION("the type behind an enum-base is not the enum's name")
    {
        auto const p = parse_text("enum : u8 { a };");
        auto const es = p.nodes_of(node_kind::enum_definition);
        REQUIRE(es.size() == 1);
        CHECK(p.text(es[0]->name) == "");
    }
    SECTION("an opaque declaration is no definition")
    {
        CHECK(parse_text("enum class e : u8;").nodes_of(node_kind::enum_definition).size() == 0);
    }
    SECTION("a trailing declarator on an enum definition is marked")
    {
        CHECK(only_enum("enum class e : u8 { } v;")->has_declarator);
        CHECK(!only_enum("enum class e : u8 { };")->has_declarator);
    }
    SECTION("the enumerator list holds no declaration to descend into")
    {
        auto const p = parse_text("enum class e : u8 { a = 1, b = 2 };");
        CHECK(p.brace_vars().size() == 0);
        CHECK(p.nodes_of(node_kind::enum_definition).size() == 1);
    }
    SECTION("where it sits is what the scope says")
    {
        CHECK(only_enum("namespace cc { enum class e { }; }")->scope == decl_scope::namespace_scope);
        CHECK(only_enum("struct s { enum class e { }; };")->scope == decl_scope::record_scope);
        CHECK(only_enum("void f() { enum class e { }; }")->scope == decl_scope::function_scope);
    }
    SECTION("an enum in a parameter list leaves the function body a function body")
    {
        auto const p = parse_text("void f(enum e x) { int y{0}; }");
        CHECK(p.nodes_of(node_kind::enum_definition).size() == 0);
        CHECK(p.brace_vars_in(decl_scope::function_scope).size() == 1);
    }
}

TEST("shaped-linter - parser - using-directives")
{
    SECTION("at file scope it is in force to the end of the file")
    {
        auto const src = cc::string_view("using namespace cc::primitive_defines;\nint x = 1;\n");
        auto const p = parse_text(src);
        auto const us = p.nodes_of(node_kind::using_directive);
        REQUIRE(us.size() == 1);
        CHECK(p.text(us[0]->name) == "cc::primitive_defines");
        CHECK(p.text(us[0]->span) == "using namespace cc::primitive_defines;");
        CHECK(p.text(us[0]->effect) == "\nint x = 1;\n");
    }
    SECTION("inside a namespace it stops at the closing brace")
    {
        auto const p = parse_text("namespace a { using namespace cc::primitive_defines; int x = 1; }\nint y = 2;\n");
        auto const us = p.nodes_of(node_kind::using_directive);
        REQUIRE(us.size() == 1);
        CHECK(p.text(us[0]->effect) == " int x = 1; ");
    }
    SECTION("inside a function it stops at the body's closing brace")
    {
        auto const p = parse_text("void f() { using namespace cc::primitive_defines; int x = 1; }\n");
        auto const us = p.nodes_of(node_kind::using_directive);
        REQUIRE(us.size() == 1);
        CHECK(p.text(us[0]->effect) == " int x = 1; ");
    }
    SECTION("a using-declaration and a type alias nominate nothing")
    {
        auto const p = parse_text("using cc::vector;\nusing my_u32 = cc::u32;\n");
        CHECK(p.nodes_of(node_kind::using_directive).size() == 0);
    }
}
