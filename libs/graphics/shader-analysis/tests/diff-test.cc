#include <nexus/test.hh>
#include <shader-analysis/diff.hh>
#include <shader-analysis/metrics.hh>

using namespace cc::primitive_defines;

TEST("shan diff - what two modules differ in comes worst first, and what they share is left out")
{
    auto a = shan::module_metrics();
    auto b = shan::module_metrics();
    a.total.instructions = 110;
    b.total.instructions = 100;
    a.total.max_live_words = 40;
    b.total.max_live_words = 20;
    a.total.kinds[isize(shan::op_kind::load)] = 7;
    b.total.kinds[isize(shan::op_kind::load)] = 7;
    auto const d = shan::diff(a, b);
    REQUIRE(d.size() == 2);
    CHECK(d[0].name == "max_live_words");
    CHECK(d[0].ratio() == 2.0);
    CHECK(d[1].name == "instructions");
    CHECK(shan::to_text(d).contains("max_live_words"));
}
