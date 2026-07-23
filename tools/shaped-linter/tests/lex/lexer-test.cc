#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <nexus/test.hh>
#include <shaped-linter/lex/lexer.hh>
#include <shaped-linter/lex/source_buffer.hh>

using namespace scl;

namespace
{
/// Owns the buffer AND its tokens: tokens borrow the buffer's text, so the buffer must outlive them.
/// The buffer is heap-boxed (stable address) — small strings are SSO, so a plain move would relocate
/// the bytes out from under the token views. Keep a `lexed` as a named local for the test's lifetime.
struct lexed
{
    cc::unique_ptr<source_buffer> buf;
    token_stream ts;

    /// The significant (non-trivia) tokens, dropping the trailing end_of_file.
    cc::vector<token> sig() const
    {
        cc::vector<token> out;
        for (auto const& t : ts.tokens)
            if (!t.is_trivia() && t.kind != token_kind::end_of_file)
                out.push_back(t);
        return out;
    }
};

lexed lex_text(cc::string_view s)
{
    auto buf = cc::make_unique<source_buffer>(source_buffer::from_text(cc::string(s), "<mem>", 0));
    auto ts = lex(*buf).value();
    return {.buf = cc::move(buf), .ts = cc::move(ts)};
}
} // namespace

TEST("shaped-linter - lexer - identifiers and keywords")
{
    auto const lx = lex_text("int x = y;");
    auto const t = lx.sig();
    REQUIRE(t.size() == 5);
    CHECK(t[0].kind == token_kind::keyword);
    CHECK(t[0].text == "int");
    CHECK(t[1].kind == token_kind::identifier);
    CHECK(t[1].text == "x");
    CHECK(t[2].is_punct("="));
    CHECK(t[3].text == "y");
    CHECK(t[4].is_punct(";"));
}

TEST("shaped-linter - lexer - numbers")
{
    SECTION("digit separators + suffix")
    {
        auto const lx = lex_text("1'000u");
        auto const t = lx.sig();
        REQUIRE(t.size() == 1);
        CHECK(t[0].kind == token_kind::integer_literal);
        CHECK(t[0].text == "1'000u");
    }
    SECTION("leading-dot float")
    {
        auto const lx = lex_text(".5");
        auto const t = lx.sig();
        REQUIRE(t.size() == 1);
        CHECK(t[0].kind == token_kind::floating_literal);
        CHECK(t[0].text == ".5");
    }
    SECTION("exponent + float suffix")
    {
        auto const lx = lex_text("1.5e9f");
        auto const t = lx.sig();
        REQUIRE(t.size() == 1);
        CHECK(t[0].kind == token_kind::floating_literal);
    }
    SECTION("hex integer")
    {
        auto const lx = lex_text("0x1f");
        auto const t = lx.sig();
        REQUIRE(t.size() == 1);
        CHECK(t[0].kind == token_kind::integer_literal);
        CHECK(t[0].text == "0x1f");
    }
}

TEST("shaped-linter - lexer - multi-char punctuators")
{
    auto const lx = lex_text("a <=> b :: c >> d");
    auto const t = lx.sig();
    REQUIRE(t.size() == 7);
    CHECK(t[1].is_punct("<=>"));
    CHECK(t[3].is_punct("::"));
    CHECK(t[5].is_punct(">>"));
}

TEST("shaped-linter - lexer - strings and chars")
{
    SECTION("plain string with escape")
    {
        auto const lx = lex_text(R"("he\"llo")");
        auto const t = lx.sig();
        REQUIRE(t.size() == 1);
        CHECK(t[0].kind == token_kind::string_literal);
        CHECK(t[0].text == R"("he\"llo")");
    }
    SECTION("raw string with delimiter")
    {
        auto const lx = lex_text(R"(R"xx(a)b)xx")");
        auto const t = lx.sig();
        REQUIRE(t.size() == 1);
        CHECK(t[0].kind == token_kind::string_literal);
        CHECK(t[0].text == R"(R"xx(a)b)xx")");
    }
    SECTION("string with suffix")
    {
        auto const lx = lex_text(R"("x"sv)");
        auto const t = lx.sig();
        REQUIRE(t.size() == 1);
        CHECK(t[0].kind == token_kind::string_literal);
        CHECK(t[0].text == R"("x"sv)");
    }
    SECTION("char with escaped quote")
    {
        auto const lx = lex_text(R"('\'')");
        auto const t = lx.sig();
        REQUIRE(t.size() == 1);
        CHECK(t[0].kind == token_kind::char_literal);
    }
    SECTION("prefixed string")
    {
        auto const lx = lex_text(R"(u8"hi")");
        auto const t = lx.sig();
        REQUIRE(t.size() == 1);
        CHECK(t[0].kind == token_kind::string_literal);
        CHECK(t[0].text == R"(u8"hi")");
    }
}

TEST("shaped-linter - lexer - comments and directives are trivia/opaque")
{
    SECTION("line and block comments")
    {
        auto const lx = lex_text("a // c\nb /* d */ c");
        auto const t = lx.sig();
        REQUIRE(t.size() == 3); // comments are trivia
        CHECK(t[0].text == "a");
        CHECK(t[1].text == "b");
        CHECK(t[2].text == "c");
    }
    SECTION("directive is one opaque token")
    {
        auto const lx = lex_text("#include <x>\nint y;");
        auto const t = lx.sig();
        REQUIRE(t.size() >= 1);
        CHECK(t[0].kind == token_kind::preprocessor_directive);
        CHECK(t[0].text == "#include <x>");
    }
}

TEST("shaped-linter - lexer - line continuation splices")
{
    // A backslash-newline in a line comment continues it; the whole '// a \ <nl> ...' is one comment.
    auto const lx = lex_text("x // a \\\n still comment\ny");
    auto const t = lx.sig();
    REQUIRE(t.size() == 2);
    CHECK(t[0].text == "x");
    CHECK(t[1].text == "y");
}

TEST("shaped-linter - lexer - the atomic member corpus line")
{
    auto const lx = lex_text("alignas(64) cc::atomic<cc::i64> _top{0};");
    auto const t = lx.sig();
    // alignas ( 64 ) cc :: atomic < cc :: i64 > _top { 0 } ;
    REQUIRE(t.size() == 17);
    CHECK(t[0].text == "alignas");
    CHECK(t[1].is_punct("("));
    CHECK(t[2].text == "64");
    CHECK(t[3].is_punct(")"));
    CHECK(t[4].text == "cc");
    CHECK(t[5].is_punct("::"));
    CHECK(t[6].text == "atomic");
    CHECK(t[7].is_punct("<"));
    CHECK(t[8].text == "cc");
    CHECK(t[9].is_punct("::"));
    CHECK(t[10].text == "i64");
    CHECK(t[11].is_punct(">"));
    CHECK(t[12].text == "_top");
    CHECK(t[13].is_punct("{"));
    CHECK(t[14].text == "0");
    CHECK(t[15].is_punct("}"));
    CHECK(t[16].is_punct(";"));
}

TEST("shaped-linter - lexer - spans tile the file gap-free")
{
    auto const lx = lex_text("a b");
    u32 expected = 0;
    for (auto const& tok : lx.ts.tokens)
    {
        CHECK(tok.span.byte_begin == expected);
        expected = tok.span.byte_end;
    }
    CHECK(expected == 3);
}
