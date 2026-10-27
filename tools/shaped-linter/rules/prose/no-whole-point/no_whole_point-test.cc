#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/rules/engine.hh>

// Smoke tests for `no-whole-point`; breadth lives in no_whole_point.md, next to this file.

using namespace scl;

namespace
{
isize count_in(cc::string_view source, cc::string_view path = "a.md")
{
    auto n = isize(0);
    for (auto const& f : run_rules_on_text(source, path))
        if (f.rule_id == "no-whole-point")
            ++n;
    return n;
}
} // namespace

TEST("shaped-linter - no-whole-point - the declarations fire")
{
    CHECK(count_in("Reserved now, which is the whole point.\n") == 1);
    CHECK(count_in("The whole point of the seam is that cc knows no GPU.\n") == 1);
    CHECK(count_in("Opacity is the entire point.\n") == 1);
    CHECK(count_in("Allocation-free, which is the point: a handler builds no vector.\n") == 1);
    CHECK(count_in("The asymmetry is the point: emptying is narrow.\n") == 1);
    CHECK(count_in("That is the point of having them.\n") == 1);
    CHECK(count_in("Cache hits replay the same numbers — that's the point.\n") == 1);
    CHECK(count_in("Cache hits replay the same numbers — that\xE2\x80\x99s the point.\n") == 1);
    CHECK(count_in("The point is to report everything in one pass.\n") == 1);
    CHECK(count_in("Serialized: the point is that the consumer runs last.\n") == 1);
    CHECK(count_in("The widening multiply is the crux of it.\n") == 1);
}

TEST("shaped-linter - no-whole-point - a literal point stays quiet")
{
    CHECK(count_in("Wherever the point is off the surface, they match.\n") == 0);
    CHECK(count_in("The point is the center of the box.\n") == 0);
    CHECK(count_in("Fails if the point is not a valid key.\n") == 0);
    CHECK(count_in("The point is to the left of the plane.\n") == 0);
    CHECK(count_in("The point tested is the very point the traces trace to.\n") == 0);
    CHECK(count_in("Sort the whole point set first.\n") == 0);
    CHECK(count_in("A hit is a parameter (`r.at(t)` is the point).\n") == 0);
    CHECK(count_in("Wrong for ten thousand, which is the point at which it fails.\n") == 0);
}

TEST("shaped-linter - no-whole-point - a block reads as one text")
{
    CHECK(count_in("// Reserved now rather than later, which is the\n// whole point of it.\n", "a.cc") == 1);
    CHECK(count_in("# Mirrored: seeing the output IS the point of this command.\n", "a.py") == 0);
    CHECK(count_in("# Mirrored: seeing the output IS the point.\n", "a.py") == 1);
}
