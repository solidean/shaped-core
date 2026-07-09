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
