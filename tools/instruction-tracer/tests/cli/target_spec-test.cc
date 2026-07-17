#include <instruction-tracer/cli/target_spec.hh>
#include <nexus/test.hh>

using namespace itrace;

TEST("target_spec - bare symbol")
{
    auto r = parse_target_spec("foo::bar");
    REQUIRE(r.has_value());
    CHECK(r.value().form == target_spec::kind::symbol);
    CHECK(r.value().symbol == "foo::bar");
    CHECK(r.value().module.empty());
}

TEST("target_spec - absolute address")
{
    auto r = parse_target_spec("0x7ff611203410");
    REQUIRE(r.has_value());
    CHECK(r.value().form == target_spec::kind::address);
    CHECK(r.value().address == 0x7ff611203410ull);
}

TEST("target_spec - module!symbol")
{
    auto r = parse_target_spec("mymodule.exe!foo::bar");
    REQUIRE(r.has_value());
    CHECK(r.value().form == target_spec::kind::module_symbol);
    CHECK(r.value().module == "mymodule.exe");
    CHECK(r.value().symbol == "foo::bar");
}

TEST("target_spec - module+offset")
{
    auto r = parse_target_spec("mymodule.exe+0x3410");
    REQUIRE(r.has_value());
    CHECK(r.value().form == target_spec::kind::module_offset);
    CHECK(r.value().module == "mymodule.exe");
    CHECK(r.value().address == 0x3410);
}

TEST("target_spec - a hex-looking name without 0x stays a symbol")
{
    // `abc` is a plausible function name; only an explicit 0x makes it an address.
    auto r = parse_target_spec("abc");
    REQUIRE(r.has_value());
    CHECK(r.value().form == target_spec::kind::symbol);
    CHECK(r.value().symbol == "abc");
}

TEST("target_spec - '!' wins over '+', since a mangled name may contain '+'")
{
    auto r = parse_target_spec("mod.dll!operator+");
    REQUIRE(r.has_value());
    CHECK(r.value().form == target_spec::kind::module_symbol);
    CHECK(r.value().module == "mod.dll");
    CHECK(r.value().symbol == "operator+");
}

TEST("target_spec - rejects malformed specs")
{
    CHECK(parse_target_spec("").has_error());
    CHECK(parse_target_spec("!foo").has_error());         // no module
    CHECK(parse_target_spec("mod.exe!").has_error());     // no symbol
    CHECK(parse_target_spec("+0x10").has_error());        // no module
    CHECK(parse_target_spec("mod.exe+").has_error());     // no offset
    CHECK(parse_target_spec("mod.exe+0xzz").has_error()); // bad hex
    CHECK(parse_target_spec("0xzz").has_error());
}

TEST("target_spec - to_string round-trips the form")
{
    CHECK(parse_target_spec("foo::bar").value().to_string() == "foo::bar");
    CHECK(parse_target_spec("mod.exe!foo").value().to_string() == "mod.exe!foo");
    CHECK(parse_target_spec("mod.exe+0x3410").value().to_string() == "mod.exe+0x3410");
    CHECK(parse_target_spec("0x1000").value().to_string() == "0x1000");
}

TEST("parse_address - prefixes, case, and grouping")
{
    CHECK(parse_address("0x10").value() == 0x10);
    CHECK(parse_address("10").value() == 0x10); // bare hex, no prefix
    CHECK(parse_address("0XdeadBEEF").value() == 0xdeadbeef);

    // windbg-style grouping, which people paste in verbatim.
    CHECK(parse_address("00007ff6`11203410").value() == 0x7ff611203410ull);
}

TEST("parse_address - rejects empty, garbage and overflow")
{
    CHECK(parse_address("").has_error());
    CHECK(parse_address("0x").has_error());
    CHECK(parse_address("0xg").has_error());
    CHECK(parse_address("ffffffffffffffff").has_value());  // exactly 64 bits
    CHECK(parse_address("1ffffffffffffffff").has_error()); // one nibble too far
}
