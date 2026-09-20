#include "check-test-support.hh"

using namespace sgl_test;

namespace
{
/// An enum and a function around `body`, so a test writes the `case` and nothing else.
cc::string over_light_kind(cc::string_view body)
{
    return cc::format("enum light_kind:\n"
                      "    point\n"
                      "    spot\n"
                      "    sun\n"
                      "\n"
                      "fun weight(k: light_kind) -> float:\n"
                      "{}",
                      body);
}
} // namespace

TEST("sgl check - a case over an enum is exhaustive when its arms name every case")
{
    CHECK(reports_for(over_light_kind("    return case k:\n"
                                      "        .point => 1.0\n"
                                      "        .spot => 0.5\n"
                                      "        .sun => 0.25\n"))
          == "");

    // `a or b` is a list of patterns, so one arm may carry two cases.
    CHECK(reports_for(over_light_kind("    return case k:\n"
                                      "        .point => 1.0\n"
                                      "        .spot or .sun => 0.5\n"))
          == "");

    // The type written out is the same constant the leading dot is.
    CHECK(reports_for(over_light_kind("    return case k:\n"
                                      "        light_kind.point => 1.0\n"
                                      "        .spot or .sun => 0.5\n"))
          == "");
}

TEST("sgl check - a case that covers less than every case needs a _")
{
    CHECK(reports_for(over_light_kind("    return case k:\n"
                                      "        .point => 1.0\n"
                                      "        .spot => 0.5\n"))
              .contains("non-exhaustive-case"));
    // The detail names what nobody matched, which is the whole point of reporting it.
    CHECK(reports_for(over_light_kind("    return case k:\n"
                                      "        .point => 1.0\n"
                                      "        .spot => 0.5\n"))
              .contains(".sun"));

    CHECK(reports_for(over_light_kind("    return case k:\n"
                                      "        .point => 1.0\n"
                                      "        _ => 0.5\n"))
          == "");
}

TEST("sgl check - two arms that name one case is duplicate-case-pattern")
{
    CHECK(reports_for(over_light_kind("    return case k:\n"
                                      "        .point => 1.0\n"
                                      "        .point => 0.5\n"
                                      "        _ => 0.25\n"))
              .contains("duplicate-case-pattern"));
}

TEST("sgl check - an arm behind _ never runs")
{
    auto const reported = reports_for(over_light_kind("    return case k:\n"
                                                      "        _ => 1.0\n"
                                                      "        .point => 0.5\n"));
    CHECK(reported.contains("unreachable-code"));
}

TEST("sgl check - every arm of a case that is a value gives one of its type, or leaves")
{
    CHECK(reports_for(over_light_kind("    return case k:\n"
                                      "        .point => 1.0\n"
                                      "        _ => 3\n"))
              .contains("type-mismatch"));

    // A jump is no value, and an arm may carry one instead: that is what CHK-120 no longer refuses.
    CHECK(reports_for(over_light_kind("    return case k:\n"
                                      "        .point => 1.0\n"
                                      "        _ => return 0.0\n"))
          == "");
}

TEST("sgl check - a case arm may be a value block that yields")
{
    CHECK(reports_for(over_light_kind("    return case k:\n"
                                      "        .point =>:\n"
                                      "            let base = 2.0\n"
                                      "            yield base * 0.5\n"
                                      "        _ => 0.0\n"))
          == "");

    // The yielded type is the arm's, so a second yield of another type is a mismatch.
    CHECK(reports_for(over_light_kind("    return case k:\n"
                                      "        .point =>:\n"
                                      "            yield 1\n"
                                      "        _ => 0.0\n"))
              .contains("type-mismatch"));
}

TEST("sgl check - a leading dot needs an enum, and names a case of it")
{
    CHECK(reports_for(over_light_kind("    return case k:\n"
                                      "        .moon => 1.0\n"
                                      "        _ => 0.0\n"))
              .contains("unknown-member"));

    // Outside a pattern there is no type to resolve it against (CHK-152).
    CHECK(reports_for("fun f() -> float => .point\n").contains("unsupported-yet"));
}

TEST("sgl check - the scrutinee is any type whose == resolves")
{
    // `int` has one, so it is a legal scrutinee; it can never be exhaustive without a `_`.
    CHECK(reports_for("fun pick(n: int) -> float:\n"
                      "    return case n:\n"
                      "        0 => 1.0\n"
                      "        1 => 0.5\n"
                      "        _ => 0.0\n")
          == "");
    CHECK(reports_for("fun pick(n: int) -> float:\n"
                      "    return case n:\n"
                      "        0 => 1.0\n"
                      "        1 => 0.5\n")
              .contains("non-exhaustive-case"));

    // A struct declares none, so no arm could match it.
    CHECK(reports_for("struct pair:\n"
                      "    a: float\n"
                      "    b: float\n"
                      "\n"
                      "fun pick(p: pair) -> float:\n"
                      "    return case p:\n"
                      "        _ => 0.0\n")
              .contains("no-matching-overload"));
}

TEST("sgl check - a case is a statement where nothing reads its value")
{
    CHECK(reports_for("enum k:\n"
                      "    a\n"
                      "    b\n"
                      "\n"
                      "fun trace(x: k):\n"
                      "    case x:\n"
                      "        .a => print 0\n"
                      "        .b => print 1\n")
          == "");
}
