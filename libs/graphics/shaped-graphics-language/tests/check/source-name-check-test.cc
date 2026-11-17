#include "source-flat-test-support.hh"

using namespace sgl_test;

namespace
{
[[nodiscard]] bool holds(cc::vector<cc::string> const& names, cc::string_view name)
{
    for (auto const& n : names)
        if (n == name)
            return true;
    return false;
}

/// The source names of an entry point's locals, each once, in the order the locals stand.
cc::vector<cc::string> source_names_of(sgl::check::flat_entry_point const& e)
{
    auto names = cc::vector<cc::string>();
    for (auto const& l : e.locals)
        if (!l.source_name.empty() && !holds(names, l.source_name))
            names.push_back(l.source_name);
    return names;
}
} // namespace

TEST("sgl check - a local keeps the name its declaration wrote, for a debugger (EMIT-267)")
{
    auto const source = grey_source("fun scaled(v: float) -> float:\n"
                                    "    let w = v * 2.0\n"
                                    "    return w + 1.0\n"
                                    "\n",
                                    "let mut acc = p.a\n"
                                    "for i in 0 ..< (p.b as int):\n"
                                    "    acc += scaled(acc * p.b)\n"
                                    "let x = acc\n");
    auto const checked = check_program(source);
    REQUIRE(reports_of(checked) == "");
    REQUIRE(checked.module.entry_points.size() == 1);
    auto const& e = checked.module.entry_points[0];

    auto const names = source_names_of(e);
    for (auto const* const written : {"p", "acc", "i", "v", "w", "x"})
        CHECK(holds(names, written)).dump("written", written).dump("described", names.size());

    // a minted name begins with what was written, and a local a pass made carries none
    for (auto const& l : e.locals)
        if (!l.source_name.empty())
            CHECK(cc::string_view(l.name).starts_with(l.source_name)).dump("minted", l.name);
}

TEST("sgl check - a local legalizing introduces has no source name, and the written ones keep theirs")
{
    // legalizing carries a block's value out in a local of its own, which the IR then lowers
    auto const checked = check_program(grey_source("", "let v = if p.a > 0.0 => p.a else p.b\n"
                                                       "let x = v * 2.0\n"));
    REQUIRE(reports_of(checked) == "");
    REQUIRE(checked.module.entry_points.size() == 1);
    auto const e = sgl::check::legalize(checked.module, checked.module.entry_points[0]);
    auto passes_own = 0;
    auto all = cc::string();
    for (auto const& l : e.locals)
    {
        all.appendf("{}={} ", l.name, l.source_name);
        if (l.source_name.empty())
            ++passes_own;
    }
    CHECK(passes_own > 0).dump("locals", all);
    CHECK(holds(source_names_of(e), "v"));
}
