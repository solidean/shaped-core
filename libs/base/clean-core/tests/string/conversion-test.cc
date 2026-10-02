#include <clean-core/string/conversion.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

TEST("conversion - utf8_to_utf16")
{
    SECTION("empty")
    {
        auto r = cc::utf8_to_utf16("");
        CHECK(r.empty());
    }

    SECTION("ascii")
    {
        auto r = cc::utf8_to_utf16("abc");
        REQUIRE(r.size() == 3);
        CHECK(r[0] == u'a');
        CHECK(r[1] == u'b');
        CHECK(r[2] == u'c');
    }

    SECTION("bmp multibyte")
    {
        auto ae = cc::utf8_to_utf16("\xC3\xA4"); // U+00E4 LATIN SMALL LETTER A WITH DIAERESIS
        REQUIRE(ae.size() == 1);
        CHECK(ae[0] == 0x00E4);

        auto euro = cc::utf8_to_utf16("\xE2\x82\xAC"); // U+20AC EURO SIGN
        REQUIRE(euro.size() == 1);
        CHECK(euro[0] == 0x20AC);
    }

    SECTION("astral becomes a surrogate pair")
    {
        auto grin = cc::utf8_to_utf16("\xF0\x9F\x98\x80"); // U+1F600
        REQUIRE(grin.size() == 2);
        CHECK(grin[0] == 0xD83D); // high surrogate
        CHECK(grin[1] == 0xDE00); // low surrogate
    }

    SECTION("malformed bytes become U+FFFD")
    {
        auto lone = cc::utf8_to_utf16("\xFF"); // invalid lead byte
        REQUIRE(lone.size() == 1);
        CHECK(lone[0] == 0xFFFD);

        auto truncated = cc::utf8_to_utf16("\xE2\x82"); // 3-byte lead, only 2 bytes present
        REQUIRE(truncated.size() >= 1);
        CHECK(truncated[0] == 0xFFFD);
    }
}

TEST("conversion - utf8_to_utf32")
{
    SECTION("one code point per element, whatever its length")
    {
        auto const r = cc::utf8_to_utf32("a\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80");
        REQUIRE(r.size() == 4);
        CHECK(r[0] == U'a');
        CHECK(r[1] == 0x00E4);
        CHECK(r[2] == 0x20AC);
        CHECK(r[3] == 0x1F600);
    }

    SECTION("a truncated sequence is one U+FFFD per byte left")
    {
        auto const r = cc::utf8_to_utf32("\xE2\x82");
        REQUIRE(r.size() == 2);
        CHECK(r[0] == 0xFFFD);
        CHECK(r[1] == 0xFFFD);
    }

    SECTION("a bad continuation byte ends the sequence and is decoded on its own")
    {
        // 0xC3 announces two bytes, but 'A' is no continuation: the 'A' must survive rather than be folded into an Á
        auto const r = cc::utf8_to_utf32("\xC3"
                                         "A");
        REQUIRE(r.size() == 2);
        CHECK(r[0] == 0xFFFD);
        CHECK(r[1] == U'A');
    }

    SECTION("an overlong encoding is U+FFFD")
    {
        auto const r = cc::utf8_to_utf32("\xC0\xAF"); // '/' in two bytes
        REQUIRE(r.size() == 1);
        CHECK(r[0] == 0xFFFD);
    }

    SECTION("an encoded surrogate is U+FFFD")
    {
        auto const r = cc::utf8_to_utf32("\xED\xA0\x80"); // U+D800
        REQUIRE(r.size() == 1);
        CHECK(r[0] == 0xFFFD);
    }
}

TEST("conversion - utf16_to_utf8")
{
    SECTION("empty")
    {
        CHECK(cc::utf16_to_utf8(cc::span<char16_t const>()).empty());
    }

    SECTION("round-trips every encodable length")
    {
        // one byte, two, three, four — one representative of each UTF-8 length class
        for (auto const* text : {"abc", "\xC3\xA4", "\xE2\x82\xAC", "\xF0\x9F\x98\x80", "a\xC3\xA4\xF0\x9F\x98\x80z"})
            CHECK(cc::utf16_to_utf8(cc::utf8_to_utf16(text)) == cc::string_view(text));
    }

    SECTION("an unpaired surrogate becomes U+FFFD")
    {
        // U+FFFD is the same three bytes whichever side produced it, so the two directions agree on malformed input.
        auto const replacement = cc::string_view("\xEF\xBF\xBD");

        auto const high = char16_t(0xD83D);
        CHECK(cc::utf16_to_utf8(cc::span<char16_t const>(&high, 1)) == replacement);

        auto const low = char16_t(0xDE00);
        CHECK(cc::utf16_to_utf8(cc::span<char16_t const>(&low, 1)) == replacement);

        // a high surrogate followed by something that is not a low one: the pair is not consumed together
        char16_t const dangling[] = {0xD83D, u'a'};
        CHECK(cc::utf16_to_utf8(dangling)
              == cc::string("\xEF\xBF\xBD"
                            "a"));
    }
}
