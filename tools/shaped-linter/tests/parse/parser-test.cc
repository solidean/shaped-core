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

    /// Every brace-form member declaration in the tree (across all records).
    cc::vector<node const*> brace_members() const
    {
        cc::vector<node const*> out;
        for (auto const& n : tree.nodes)
            if (n.kind == node_kind::member_declaration && n.init_form == member_init_form::brace)
                out.push_back(&n);
        return out;
    }

    isize record_count() const
    {
        isize n = 0;
        for (auto const& x : tree.nodes)
            if (x.kind == node_kind::record_definition)
                ++n;
        return n;
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
    auto const m = p.brace_members();
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
    auto const m = p.brace_members();
    REQUIRE(m.size() == 3);
    CHECK(p.text(m[0]->name) == "_top");
    CHECK(p.text(m[1]->name) == "_bottom");
    CHECK(p.text(m[2]->name) == "_ring");
    CHECK(p.text(m[2]->init_inner) == "nullptr");
}

TEST("shaped-linter - parser - assignment form is not a brace member")
{
    auto const p = parse_text("struct S { int x = 0; cc::atomic<int> y = 0; };");
    CHECK(p.brace_members().size() == 0);
    CHECK(p.record_count() == 1);
}

TEST("shaped-linter - parser - empty and multi-element braces")
{
    SECTION("empty brace")
    {
        auto const p = parse_text("struct S { int y{}; };");
        auto const m = p.brace_members();
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "y");
        CHECK(p.text(m[0]->init_inner) == "");
    }
    SECTION("multi-element brace")
    {
        auto const p = parse_text("struct S { P p{1, 2}; };");
        auto const m = p.brace_members();
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "p");
        CHECK(p.text(m[0]->init_inner) == "1, 2");
    }
    SECTION("static inline member")
    {
        auto const p = parse_text("struct S { static inline cc::atomic<int> live{0}; };");
        auto const m = p.brace_members();
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "live");
    }
    SECTION("mutable member")
    {
        auto const p = parse_text("struct S { mutable cc::atomic<cc::i64> _c{0}; };");
        auto const m = p.brace_members();
        REQUIRE(m.size() == 1);
        CHECK(p.text(m[0]->name) == "_c");
    }
}

TEST("shaped-linter - parser - locals inside a function are not members")
{
    SECTION("plain local")
    {
        auto const p = parse_text("struct S { void f() { int x{0}; } };");
        CHECK(p.brace_members().size() == 0);
    }
    SECTION("static local")
    {
        auto const p = parse_text("struct S { void f() { static cc::atomic<int> s{1}; } };");
        CHECK(p.brace_members().size() == 0);
    }
}

TEST("shaped-linter - parser - constructor init-list is not a member init")
{
    auto const p = parse_text("struct S { S() : _x{0} {} int _x; };");
    CHECK(p.brace_members().size() == 0);
}

TEST("shaped-linter - parser - namespace-scope variable is not a member")
{
    auto const p = parse_text("namespace n { cc::atomic<int> g{0}; }");
    CHECK(p.brace_members().size() == 0);
}

TEST("shaped-linter - parser - nested record members are found")
{
    auto const p = parse_text("struct O { struct I { int a{1}; }; int b{2}; };");
    auto const m = p.brace_members();
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
    auto const m = p.brace_members();
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
    auto const m = p.brace_members();
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
    auto const m = p.brace_members();
    REQUIRE(m.size() == 1);
    CHECK(p.text(m[0]->name) == "_top");
}

TEST("shaped-linter - parser - class and union bodies")
{
    SECTION("class with access specifier")
    {
        auto const p = parse_text("class C { public: int x{3}; private: int _y{4}; };");
        auto const m = p.brace_members();
        REQUIRE(m.size() == 2);
        CHECK(p.text(m[0]->name) == "x");
        CHECK(p.text(m[1]->name) == "_y");
    }
    SECTION("enum body is not descended")
    {
        auto const p = parse_text("enum class E { A = 1, B = 2 };");
        CHECK(p.brace_members().size() == 0);
        CHECK(p.record_count() == 0);
    }
}
