#include "check-test-support.hh"

using namespace sgl_test;

namespace
{
/// The `(enum …)` line of the module dump, which carries every case and its value.
cc::string enum_line(sgl::check::checked_module const& m, cc::string_view name)
{
    auto const text = sgl::check::dump(m);
    auto const head = cc::format("(enum {}", name);
    auto const at = cc::string_view(text).find(cc::string_view(head));
    if (at < 0)
        return {};
    auto const rest = cc::string_view(text).subview({.offset = at, .size = text.size() - at});
    auto const end = rest.find('\n');
    return cc::string(end < 0 ? rest : rest.subview({.offset = 0, .size = end}));
}
} // namespace

TEST("sgl check - an enum is a type of its own, and its cases count from 0")
{
    auto const s = check_sources(read_prelude(), "enum light_kind:\n    point\n    spot\n    sun\n");
    CHECK(reports_of(s) == "");
    CHECK(enum_line(s.module, "light_kind") == "(enum light_kind (point = 0) (spot = 1) (sun = 2))");
}

TEST("sgl check - a written case value sets the count on from there, and a repeat is a legal alias")
{
    auto const s = check_sources(read_prelude(), "enum mask:\n    red = 1\n    green = 2\n    blue = 4\n    all = 7\n");
    CHECK(reports_of(s) == "");
    CHECK(enum_line(s.module, "mask") == "(enum mask (red = 1) (green = 2) (blue = 4) (all = 7))");

    // The count goes on from what was written, so `b` is 5 and not 1.
    auto const counted = check_sources(read_prelude(), "enum e:\n    a = 4\n    b\n    c\n");
    CHECK(enum_line(counted.module, "e") == "(enum e (a = 4) (b = 5) (c = 6))");

    // An alias is legal; two cases of one NAME are not.
    CHECK(reports_for("enum e:\n    a = 1\n    b = 1\n") == "");
    CHECK(reports_for("enum e:\n    a\n    a\n") == "duplicate-declaration user:[a] a\n");
}

TEST("sgl check - a case value is an int computed before anything runs, as a const's is")
{
    CHECK(reports_for("enum e:\n    a = 1.5\n") == "literal-not-representable user:[1.5] int does not hold 1.5 exactly\n");
    CHECK(reports_for("enum e:\n    a = b\n") == "unknown-name user:[b] b\n");
    CHECK(reports_for("const k = 4\nenum e:\n    a = k * 2\n    b = 1 << 4\n    c\n") == "");
    // the enum is one type for every compile, so a case cannot follow an option
    CHECK(reports_for("@option const n = 2\nenum e:\n    a = n\n")
          == "unsupported-yet user:[n] a case value that reaches an option\n");
    // a case is read through the enum it is a case of, which is still being declared
    CHECK(reports_for("enum e:\n    a = 1\n    b = e.a as int\n") == "dependency-cycle user:[e] e -> e\n");

    // An implicit value follows the one before it, and the last int has none after it.
    CHECK(reports_for("enum e:\n    a = 2147483647\n    b\n")
          == "unsupported-yet user:[b] an implicit case value past the last int\n");
    CHECK(reports_for("enum e:\n    a = 2147483647\n    b = 0\n") == "");
}

TEST("sgl check - an enum has properties and methods, and a nested declaration is unsupported-yet")
{
    CHECK(reports_for("enum e:\n    a\n    b\n    is_a => self == e.a\n    fun first() => e.a\n"
                      "fun f(x: e) -> bool => x.is_a and e.first() == x\n")
          == "");
    CHECK(reports_for("enum e:\n    a\n    const k = 1\n")
          == "unsupported-yet user:[const k = 1] a declaration in an enum\n");
}

TEST("sgl check - enum.case is a value of the enum, and an unknown case is unknown-member")
{
    auto const source = "enum light_kind:\n    point\n    sun\n\nfun pick() -> light_kind => light_kind.point\n";
    CHECK(reports_for(source) == "");

    CHECK(reports_for("enum light_kind:\n    point\n\nfun pick() -> light_kind => light_kind.moon\n")
          == "unknown-member user:[moon] the enum light_kind has no case moon\n");

    // An enum is no value by itself (CHK-148), the way a struct's name is not.
    CHECK(
        reports_for("enum light_kind:\n    point\n\nfun pick() -> light_kind => light_kind\n").contains("unsupported-yet"));
}

TEST("sgl check - an enum compares with == and != and converts to nothing")
{
    auto const equal = "enum k:\n    a\n    b\n\nfun same(x: k) -> bool => x == k.a\n";
    CHECK(reports_for(equal) == "");
    auto const unequal = "enum k:\n    a\n    b\n\nfun other(x: k) -> bool => x != k.a\n";
    CHECK(reports_for(unequal) == "");

    // Neither direction converts on its own, and no other operator resolves (CHK-150).
    CHECK(reports_for("enum k:\n    a\n\nfun n(x: k) -> int => x\n").contains("type-mismatch"));
    CHECK(reports_for("enum k:\n    a\n\nfun n(x: int) -> k => x\n").contains("type-mismatch"));
    CHECK(reports_for("enum k:\n    a\n\nfun n(x: k) -> bool => x < k.a\n").contains("no-matching-overload"));
    CHECK(reports_for("enum k:\n    a\n\nfun n(x: k) -> k => x + k.a\n").contains("no-matching-overload"));

    // `as int` is the one conversion, and it does not come back.
    CHECK(reports_for("enum k:\n    a\n\nfun n(x: k) -> int => x as int\n") == "");
    CHECK(reports_for("enum k:\n    a\n\nfun n(x: k) -> uint => x as uint\n").contains("no-matching-overload"));
    CHECK(reports_for("enum k:\n    a\n\nfun n(x: int) -> k => x as k\n").contains("no-matching-overload"));

    // Two enums are two types, so one never compares with the other.
    CHECK(reports_for("enum a:\n    x\nenum b:\n    x\n\nfun n(p: a, q: b) -> bool => p == q\n")
              .contains("no-matching-overload"));
}

TEST("sgl check - an enum stands in a type position like a struct")
{
    CHECK(reports_for("enum k:\n    a\n\nstruct hit:\n    kind: k\n") == "");
    CHECK(reports_for("enum k:\n    a\n\nfun f(x: k) -> k => x\n") == "");
}
