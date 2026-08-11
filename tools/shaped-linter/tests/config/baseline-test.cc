#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-linter/config/baseline.hh>
#include <shaped-linter/config/lint_config.hh>

using namespace scl;

namespace
{
cc::vector<cc::string> includes_of(cc::string_view a, cc::string_view b = "", cc::string_view c = "")
{
    cc::vector<cc::string> out;
    out.push_back(cc::string(a));
    if (!b.empty())
        out.push_back(cc::string(b));
    if (!c.empty())
        out.push_back(cc::string(c));
    return out;
}
} // namespace

TEST("baseline - one entry blesses both spellings of an SDK header")
{
    cc::vector<baseline_group> groups;
    add_to_baseline(groups, "a/.shaped-lint.yml", "<DbgHelp.h>");
    add_to_baseline(groups, "a/.shaped-lint.yml", "<dbghelp.h>");

    REQUIRE(groups.size() == 1);
    REQUIRE(groups[0].includes.size() == 1);
    CHECK(groups[0].includes[0] == "<DbgHelp.h>"); // the first spelling seen
}

TEST("baseline - each config collects its own")
{
    cc::vector<baseline_group> groups;
    add_to_baseline(groups, "b/.shaped-lint.yml", "<vector>");
    add_to_baseline(groups, "a/.shaped-lint.yml", "<atomic>");
    add_to_baseline(groups, "b/.shaped-lint.yml", "<string>");

    sort_baseline(groups);

    REQUIRE(groups.size() == 2);
    CHECK(groups[0].config_path == "a/.shaped-lint.yml");
    CHECK(groups[1].config_path == "b/.shaped-lint.yml");
    CHECK(groups[1].includes[0] == "<string>"); // sorted, so a re-run reproduces the block
    CHECK(groups[1].includes[1] == "<vector>");
}

TEST("baseline - the rendered block parses as a config")
{
    auto const block = render_baseline_block(includes_of("<atomic>", "<vector>"));
    auto const directives = load_include_directives(cc::format("rules:\n{}", block), "");

    REQUIRE(directives.has_value());
    REQUIRE(directives.value().size() == 2);
    CHECK(directives.value()[0].allow);
    CHECK(directives.value()[0].reason == "baseline");
}

TEST("baseline - a first block is appended under a rules key added for it")
{
    auto const text = apply_baseline_block("", includes_of("<atomic>"));

    CHECK(text.contains("rules:"));
    CHECK(text.contains(k_baseline_begin));
    CHECK(text.contains("value: <atomic>"));
    CHECK(load_include_directives(text, "").has_value());
}

TEST("baseline - a re-run rewrites only the generated block")
{
    auto const curated = cc::string(R"(rules:
  - kind: deny-include
    value: <mutex>
    reason: use clean-core/thread/mutex.hh
)");

    auto const first = apply_baseline_block(curated, includes_of("<atomic>"));
    auto const second = apply_baseline_block(first, includes_of("<vector>"));

    // The curated entry above the markers is a human's and survives; the block below is the tool's.
    CHECK(second.contains("value: <mutex>"));
    CHECK(second.contains("value: <vector>"));
    CHECK(!second.contains("value: <atomic>"));
    CHECK(load_include_directives(second, "").has_value());
}

TEST("baseline - the same input reproduces the same file")
{
    auto const once = apply_baseline_block("rules:\n", includes_of("<atomic>", "<vector>"));
    CHECK(apply_baseline_block(once, includes_of("<atomic>", "<vector>")) == once);
}

TEST("baseline - blessing nothing removes the block rather than leaving an empty one")
{
    auto const with = apply_baseline_block("rules:\n", includes_of("<atomic>"));
    auto const without = apply_baseline_block(with, {});

    CHECK(!without.contains(k_baseline_begin));
    CHECK(!without.contains("<atomic>"));
}
