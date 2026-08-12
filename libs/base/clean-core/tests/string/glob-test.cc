#include <clean-core/string/glob.hh>
#include <nexus/test.hh>

using namespace cc;

TEST("glob - a literal pattern is an exact match")
{
    CHECK(glob_matches("src/a.hh", "src/a.hh", {}));
    CHECK(!glob_matches("src/a.hh", "src/a.cc", {}));
    CHECK(!glob_matches("src/a.hh", "src/sub/a.hh", {}));
}

TEST("glob - a star stays inside one path segment")
{
    CHECK(glob_matches("src/*.hh", "src/a.hh", {}));
    CHECK(!glob_matches("src/*.hh", "src/sub/a.hh", {}));
    CHECK(glob_matches("*.hh", "a.hh", {}));
    CHECK(!glob_matches("*.hh", "src/a.hh", {}));
}

TEST("glob - a globstar crosses them")
{
    CHECK(glob_matches("src/**", "src/a.hh", {}));
    CHECK(glob_matches("src/**", "src/deep/deeper/a.hh", {}));
    CHECK(!glob_matches("src/**", "tests/a.hh", {}));
    CHECK(glob_matches("**/*.hh", "src/deep/a.hh", {}));
}

TEST("glob - the separator after a globstar is optional")
{
    // `src/**/x.hh` has to cover the zero-directory case, or every rule would need a second pattern.
    CHECK(glob_matches("src/**/x.hh", "src/x.hh", {}));
    CHECK(glob_matches("src/**/x.hh", "src/a/x.hh", {}));
    CHECK(glob_matches("src/**/x.hh", "src/a/b/x.hh", {}));
}

TEST("glob - a question mark is one character, never a separator")
{
    CHECK(glob_matches("a?c.hh", "abc.hh", {}));
    CHECK(!glob_matches("a?c.hh", "ac.hh", {}));
    CHECK(!glob_matches("a?c", "a/c", {}));
}

TEST("glob - a trailing slash means the subtree")
{
    CHECK(glob_matches("tests/", "tests/a.cc", {}));
    CHECK(glob_matches("tests/", "tests/deep/a.cc", {}));
    CHECK(!glob_matches("tests/", "tests", {}));
    CHECK(!glob_matches("tests/", "src/a.cc", {}));
}

TEST("glob - include spellings are globbable too")
{
    CHECK(glob_matches("<atomic>", "<atomic>", {}));
    CHECK(glob_matches("<d3d12*.h>", "<d3d12sdklayers.h>", {}));
    CHECK(glob_matches("<d3d12*.h>", "<d3d12.h>", {}));
    CHECK(!glob_matches("<d3d12*.h>", "<dxgi1_6.h>", {}));
}

TEST("glob - matching is case sensitive unless asked otherwise")
{
    // Case folding is opt-in, and only a caller comparing against a real file system wants it.
    CHECK(!glob_matches("<windows.h>", "<Windows.h>", {}));
}

TEST("glob - ignore_case folds both sides")
{
    constexpr auto fold = cc::flags(glob_option::ignore_case);
    CHECK(glob_matches("<windows.h>", "<Windows.h>", fold));
    CHECK(glob_matches("**/Function_Ref-Test.cc", "c:/projects/tests/function_ref-test.cc", fold));
    CHECK(glob_matches("C:/PROJECTS/**", "c:/projects/a/b.cc", fold));
    CHECK(!glob_matches("*.HH", "a.cc", fold));
}

TEST("glob - normalize folds both sides' spelling first")
{
    constexpr auto norm = cc::flags(glob_option::normalize);

    // Without it the two spellings of the same path do not match at all.
    CHECK(!glob_matches("libs/base/**", "libs\\base\\a.hh", {}));
    CHECK(glob_matches("libs/base/**", "libs\\base\\a.hh", norm));
    CHECK(glob_matches("/c/Projects/**", "C:\\Projects\\a.hh", norm));

    // The trailing-slash subtree shorthand survives, though normalization drops that very slash.
    CHECK(glob_matches("tests/", "tests\\a.cc", norm));
    CHECK(!glob_matches("tests/", "tests", norm));

    // The options compose.
    CHECK(glob_matches("LIBS\\base\\*.hh", "libs/base/a.hh", glob_option::normalize | glob_option::ignore_case));
}

TEST("glob_normalize_path - separators, duplicates and a trailing slash")
{
    CHECK(glob_normalize_path("libs\\base\\clean-core") == "libs/base/clean-core");
    CHECK(glob_normalize_path("libs//base/") == "libs/base");
    CHECK(glob_normalize_path("a.hh") == "a.hh");
}

TEST("glob_normalize_path - every spelling of a drive lands on the same one")
{
    // Only the drive letter is case-folded: the rest of a path may well be case-significant, and ignore_case is there for callers it is not.
    CHECK(glob_normalize_path("C:\\Projects\\x\\") == "c:/Projects/x");
    CHECK(glob_normalize_path("c:/Projects/x") == "c:/Projects/x");
    CHECK(glob_normalize_path("/c/Projects/x") == "c:/Projects/x");
    CHECK(glob_normalize_path("/C/Projects/x") == "c:/Projects/x");

    // A bare drive, with or without its slash.
    CHECK(glob_normalize_path("D:\\") == "d:");
    CHECK(glob_normalize_path("/d") == "d:");

    // Only a single-letter root is a drive; a longer one is an ordinary directory.
    CHECK(glob_normalize_path("/usr/lib") == "/usr/lib");
    CHECK(glob_normalize_path("/cc/lib") == "/cc/lib");
}
