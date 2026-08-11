#include <nexus/test.hh>
#include <shaped-linter/config/glob.hh>

using namespace scl;

TEST("glob - a literal pattern is an exact match")
{
    CHECK(glob_matches("src/a.hh", "src/a.hh"));
    CHECK(!glob_matches("src/a.hh", "src/a.cc"));
    CHECK(!glob_matches("src/a.hh", "src/sub/a.hh"));
}

TEST("glob - a star stays inside one path segment")
{
    CHECK(glob_matches("src/*.hh", "src/a.hh"));
    CHECK(!glob_matches("src/*.hh", "src/sub/a.hh"));
    CHECK(glob_matches("*.hh", "a.hh"));
    CHECK(!glob_matches("*.hh", "src/a.hh"));
}

TEST("glob - a globstar crosses them")
{
    CHECK(glob_matches("src/**", "src/a.hh"));
    CHECK(glob_matches("src/**", "src/deep/deeper/a.hh"));
    CHECK(!glob_matches("src/**", "tests/a.hh"));
    CHECK(glob_matches("**/*.hh", "src/deep/a.hh"));
}

TEST("glob - the separator after a globstar is optional")
{
    // `src/**/x.hh` has to cover the zero-directory case, or every rule would need a second pattern.
    CHECK(glob_matches("src/**/x.hh", "src/x.hh"));
    CHECK(glob_matches("src/**/x.hh", "src/a/x.hh"));
    CHECK(glob_matches("src/**/x.hh", "src/a/b/x.hh"));
}

TEST("glob - a question mark is one character, never a separator")
{
    CHECK(glob_matches("a?c.hh", "abc.hh"));
    CHECK(!glob_matches("a?c.hh", "ac.hh"));
    CHECK(!glob_matches("a?c", "a/c"));
}

TEST("glob - a trailing slash means the subtree")
{
    CHECK(glob_matches("tests/", "tests/a.cc"));
    CHECK(glob_matches("tests/", "tests/deep/a.cc"));
    CHECK(!glob_matches("tests/", "tests"));
    CHECK(!glob_matches("tests/", "src/a.cc"));
}

TEST("glob - include spellings are globbable too")
{
    CHECK(glob_matches("<atomic>", "<atomic>"));
    CHECK(glob_matches("<d3d12*.h>", "<d3d12sdklayers.h>"));
    CHECK(glob_matches("<d3d12*.h>", "<d3d12.h>"));
    CHECK(!glob_matches("<d3d12*.h>", "<dxgi1_6.h>"));
}

TEST("glob - matching is case sensitive")
{
    // Case folding is the caller's job, and only include spellings want it — a path does not.
    CHECK(!glob_matches("<windows.h>", "<Windows.h>"));
}

TEST("normalize_path - separators, duplicates and a trailing slash")
{
    CHECK(normalize_path("libs\\base\\clean-core") == "libs/base/clean-core");
    CHECK(normalize_path("libs//base/") == "libs/base");
    CHECK(normalize_path("C:\\Projects\\x\\") == "C:/Projects/x");
    CHECK(normalize_path("a.hh") == "a.hh");
}
