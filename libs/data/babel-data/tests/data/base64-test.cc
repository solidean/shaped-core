#include <babel-data/data/base64.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
cc::string_view as_text(cc::span<byte const> bytes)
{
    return cc::string_view(reinterpret_cast<char const*>(bytes.data()), bytes.size());
}

cc::span<byte const> as_bytes(cc::string_view s)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(s.data()), s.size());
}
} // namespace

TEST("base64 - decode rfc 4648 test vectors")
{
    CHECK(babel::base64::decode("").value().empty());
    CHECK(as_text(babel::base64::decode("Zg==").value()) == "f");
    CHECK(as_text(babel::base64::decode("Zm8=").value()) == "fo");
    CHECK(as_text(babel::base64::decode("Zm9v").value()) == "foo");
    CHECK(as_text(babel::base64::decode("Zm9vYg==").value()) == "foob");
    CHECK(as_text(babel::base64::decode("Zm9vYmE=").value()) == "fooba");
    CHECK(as_text(babel::base64::decode("Zm9vYmFy").value()) == "foobar");
}

TEST("base64 - encode rfc 4648 test vectors")
{
    CHECK(babel::base64::encode(as_bytes("")) == "");
    CHECK(babel::base64::encode(as_bytes("f")) == "Zg==");
    CHECK(babel::base64::encode(as_bytes("fo")) == "Zm8=");
    CHECK(babel::base64::encode(as_bytes("foo")) == "Zm9v");
    CHECK(babel::base64::encode(as_bytes("foob")) == "Zm9vYg==");
    CHECK(babel::base64::encode(as_bytes("fooba")) == "Zm9vYmE=");
    CHECK(babel::base64::encode(as_bytes("foobar")) == "Zm9vYmFy");
}

TEST("base64 - round-trips every byte value")
{
    auto all = cc::vector<byte>();
    for (auto v = 0; v < 256; ++v)
        all.push_back(byte(u8(v)));

    auto const text = babel::base64::encode(all);
    auto const back = babel::base64::decode(text).value();

    REQUIRE(back.size() == all.size());
    for (auto i = isize(0); i < all.size(); ++i)
        CHECK(back[i] == all[i]);
}

TEST("base64 - url-safe alphabet decodes to the same bytes")
{
    // 0xFB 0xFF 0xBF is "+/+/" in the standard alphabet and "-_-_" URL-safe.
    auto const standard = babel::base64::decode("+/+/").value();
    auto const url_safe = babel::base64::decode("-_-_").value();

    REQUIRE(standard.size() == 3);
    REQUIRE(url_safe.size() == 3);
    for (auto i = isize(0); i < 3; ++i)
        CHECK(standard[i] == url_safe[i]);
}

TEST("base64 - whitespace is skipped and padding is optional")
{
    CHECK(as_text(babel::base64::decode("Zm9v\nYmFy").value()) == "foobar");
    CHECK(as_text(babel::base64::decode(" Zm 9v Ym Fy ").value()) == "foobar");
    CHECK(as_text(babel::base64::decode("Zm9vYg").value()) == "foob");   // "Zm9vYg==" unpadded
    CHECK(as_text(babel::base64::decode("Zm9vYmE").value()) == "fooba"); // "Zm9vYmE=" unpadded
}

TEST("base64 - decoded_size matches decode")
{
    for (auto const text : {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg", "Zm9v\nYmFy"})
    {
        auto const size = babel::base64::decoded_size(text);
        REQUIRE(size.has_value());
        CHECK(size.value() == babel::base64::decode(text).value().size());
    }

    CHECK(!babel::base64::decoded_size("Zm9vY").has_value());
    CHECK(!babel::base64::decoded_size("!!!!").has_value());
}

TEST("base64 - decode_into reports the write and refuses a short buffer")
{
    byte storage[8] = {};

    auto const written = babel::base64::decode_into("Zm9vYmFy", storage);
    REQUIRE(written.has_value());
    CHECK(written.value() == 6);
    CHECK(as_text(cc::span<byte const>(storage, 6)) == "foobar");

    CHECK(babel::base64::decode_into("Zm9vYmFy", cc::span<byte>(storage, 5)).has_error());
    CHECK(babel::base64::decode_into("Zm9vYmFy", cc::span<byte>()).has_error());
    CHECK(babel::base64::decode_into("", cc::span<byte>()).value() == 0);
}

TEST("base64 - errors")
{
    CHECK(babel::base64::decode("Zm9v*").has_error());  // character outside both alphabets
    CHECK(babel::base64::decode("Zm9vY").has_error());  // single-character final quantum
    CHECK(babel::base64::decode("=").has_error());      // padding closing nothing
    CHECK(babel::base64::decode("Zm9v=").has_error());  // padding after a complete quantum
    CHECK(babel::base64::decode("Zg===").has_error());  // more padding than a quantum can hold
    CHECK(babel::base64::decode("Zg==Zg").has_error()); // data after padding
}
