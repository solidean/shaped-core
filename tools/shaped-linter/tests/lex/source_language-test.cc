#include <nexus/test.hh>
#include <shaped-linter/lex/source_language.hh>

using namespace scl;

TEST("shaped-linter - source_language - extensions pick the front end")
{
    CHECK(language_from_path("libs/base/clean-core/src/vector.hh") == source_language::cpp);
    CHECK(language_from_path("a.cc") == source_language::cpp);
    CHECK(language_from_path("dev.py") == source_language::python);
    CHECK(language_from_path("tools/dev/lib/quality/format.pyi") == source_language::python);
    CHECK(language_from_path("docs/coding-guidelines.md") == source_language::markdown);
}

TEST("shaped-linter - source_language - anything unrecognized is C++")
{
    SECTION("no extension at all")
    {
        CHECK(language_from_path("readme") == source_language::cpp);
    }
    SECTION("the in-memory buffer of run_rules_on_text")
    {
        CHECK(language_from_path("<memory>") == source_language::cpp);
    }
    SECTION("a dot in a directory does not make an extension")
    {
        CHECK(language_from_path("a.b/readme") == source_language::cpp);
        CHECK(language_from_path("a.md/readme") == source_language::cpp);
    }
    SECTION("an empty path")
    {
        CHECK(language_from_path("") == source_language::cpp);
    }
}

TEST("shaped-linter - source_language - the language mask")
{
    CHECK((k_cpp_only & language_bit(source_language::cpp)) != 0);
    CHECK((k_cpp_only & language_bit(source_language::markdown)) == 0);

    // Every language must be in the all-languages set, or a rule that asks for it silently never runs.
    for (auto const l : {source_language::cpp, source_language::python, source_language::markdown})
        CHECK((k_all_languages & language_bit(l)) != 0);
}
