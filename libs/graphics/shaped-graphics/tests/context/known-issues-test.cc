#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shaped-graphics/context/known_issues.hh>

TEST("sg - every known issue names a reproduction under docs/bugs-external, once")
{
    auto const issues = sg::known_issues();
    REQUIRE(!issues.empty());
    for (auto i = 0; i < issues.size(); ++i)
    {
        auto const& issue = issues[i];
        CHECK(issue.affects != nullptr).dump("id", issue.id);
        CHECK(!issue.symptom.empty()).dump("id", issue.id);
        for (auto j = 0; j < i; ++j)
            CHECK(issues[j].id != issue.id).dump("id", issue.id);
        auto const readme = cc::format("{}/docs/bugs-external/{}/readme.md", nx::source_root(), issue.id);
        CHECK(cc::file_read_stream_adapter::open(readme).has_value()).dump("missing", readme);
    }
}
