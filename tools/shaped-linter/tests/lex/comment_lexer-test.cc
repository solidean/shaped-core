#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/lex/comment_lexer.hh>
#include <shaped-linter/lex/source_buffer.hh>
#include <shaped-linter/rules/engine.hh>

// The Rust and Lean lexers exist to find comments, so every case here is about telling one from a
// look-alike: a marker inside a string, a char literal against a lifetime or a prime.

using namespace scl;

namespace
{
/// The comment tokens of `source`, read as the language of `path`, in order.
cc::vector<cc::string> comments_of(cc::string_view source, cc::string_view path)
{
    auto const buf = cc::make_unique<source_buffer>(source_buffer::from_text(cc::string(source), path, 0));
    auto const ts = lex_for(*buf, language_from_path(path)).value();

    auto out = cc::vector<cc::string>();
    for (auto const& t : ts.tokens)
        if (t.kind == token_kind::line_comment || t.kind == token_kind::block_comment)
            out.push_back(cc::string(t.text));
    return out;
}

/// Whether `source` tiles gap-free: every byte belongs to exactly one token, in order.
bool tiles(cc::string_view source, cc::string_view path)
{
    auto const buf = cc::make_unique<source_buffer>(source_buffer::from_text(cc::string(source), path, 0));
    auto const ts = lex_for(*buf, language_from_path(path)).value();

    auto at = u32(0);
    for (auto const& t : ts.tokens)
    {
        if (t.span.byte_begin != at)
            return false;
        at = t.span.byte_end;
    }
    return at == u32(source.size());
}

isize flow_findings(cc::string_view source, cc::string_view path)
{
    auto n = isize(0);
    for (auto const& f : run_rules_on_text(source, path))
        if (f.rule_id == "no-flow-prose")
            ++n;
    return n;
}
} // namespace

TEST("shaped-linter - rust lexer - comments and their look-alikes")
{
    SECTION("line, doc and nested block comments")
    {
        auto const c = comments_of("// a\n/// b\n//! c\n/* d /* nested */ still d */ x\n", "a.rs");
        REQUIRE(c.size() == 4);
        CHECK(c[0] == "// a");
        CHECK(c[1] == "/// b");
        CHECK(c[2] == "//! c");
        CHECK(c[3] == "/* d /* nested */ still d */");
    }
    SECTION("a marker inside a string is not a comment")
    {
        CHECK(comments_of("let s = \"// not a comment\";\n", "a.rs").empty());
        CHECK(comments_of("let s = \"a \\\" // still the string\";\n", "a.rs").empty());
    }
    SECTION("raw strings run to the matching hash count")
    {
        CHECK(comments_of("let s = r#\"a \"# // in\"#; // out\n", "a.rs").size() == 1);
        CHECK(comments_of("let s = br##\"\"#\"## ; // out\n", "a.rs").size() == 1);
    }
    SECTION("a lifetime is not a char literal, so the comment after it is found")
    {
        CHECK(comments_of("fn f<'a>(x: &'a str) {} // after\n", "a.rs").size() == 1);
        CHECK(comments_of("let c = '\"'; // after a quote char\n", "a.rs").size() == 1);
    }
    SECTION("the file tiles gap-free")
    {
        CHECK(tiles("fn f<'a>(s: &'a str) -> char { let r = r#\"x\"#; 'é' } // é\n", "a.rs"));
    }
}

TEST("shaped-linter - lean lexer - comments and their look-alikes")
{
    SECTION("line, block and doc comments, nested")
    {
        auto const c = comments_of("-- a\n/-- doc /- nested -/ still -/\n/-! module -/\ndef x := 1\n", "a.lean");
        REQUIRE(c.size() == 3);
        CHECK(c[0] == "-- a");
        CHECK(c[1] == "/-- doc /- nested -/ still -/");
        CHECK(c[2] == "/-! module -/");
    }
    SECTION("a primed name is not a char literal")
    {
        CHECK(comments_of("theorem t (h' : a = b) : b = a := h'.symm -- after\n", "a.lean").size() == 1);
    }
    SECTION("a marker inside a string is not a comment")
    {
        CHECK(comments_of("def s := \"-- not a comment\"\n", "a.lean").empty());
    }
    SECTION("the file tiles gap-free")
    {
        CHECK(tiles("def f (x' : ℕ) : Char := 'a' -- ε₁\n/- b -/\n", "a.lean"));
    }
}

TEST("shaped-linter - rust and lean - their comments are prose")
{
    CHECK(flow_findings("// One point. And a second one.\nfn f() {}\n", "a.rs") == 1);
    CHECK(flow_findings("/// One point. And a second one.\nfn f() {}\n", "a.rs") == 1);
    CHECK(flow_findings("let s = \"One point. And a second one.\";\n", "a.rs") == 0);
    CHECK(flow_findings("-- One point. And a second one.\n", "a.lean") == 1);
    CHECK(flow_findings("/-- One point. And a second one. -/\ndef x := 1\n", "a.lean") == 1);
    CHECK(flow_findings("/-- One point.\nAnd a second one. -/\ndef x := 1\n", "a.lean") == 0);
}
