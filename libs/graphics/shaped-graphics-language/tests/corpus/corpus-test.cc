#include "../check/check-test-support.hh"

#include <clean-core/algorithm/sort.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/test_source.hh>
#include <shaped-graphics-language/emit/emit.hh>

// clean-core has no directory listing yet, and a corpus file needs no CMake entry of its own only when it is found by one.
#include <filesystem>

// The corpus is how SGL's semantics are tested: one `.sgl` file per topic under tests/corpus/, each a program whose
// `test`s state what the language does.
// A file passes when it checks with no diagnostic at all, every test in it passes, and every entry point it declares is
// written for every target.

#ifndef SGL_CORPUS_DIR
#error "SGL_CORPUS_DIR must be defined by the build (see libs/graphics/shaped-graphics-language/CMakeLists.txt)"
#endif

using namespace sgl_test;

namespace
{
struct sgl_corpus_file
{
    /// Absolute, for opening.
    cc::string path;
    /// What the invocation, and every diagnostic, is named after.
    cc::string relative_path;
};

/// Every `*.sgl` under the corpus root, in a stable order so invocation names never shuffle.
cc::vector<sgl_corpus_file> corpus_files()
{
    namespace fs = std::filesystem;
    auto out = cc::vector<sgl_corpus_file>();
    auto const root = fs::path(SGL_CORPUS_DIR);
    auto ec = std::error_code();
    for (auto const& e : fs::recursive_directory_iterator(root, ec))
    {
        if (!e.is_regular_file() || e.path().extension() != ".sgl")
            continue;
        auto const relative = fs::relative(e.path(), root, ec).generic_string();
        out.push_back({.path = cc::string(e.path().string().c_str()), .relative_path = cc::string(relative.c_str())});
    }
    cc::sort(out, [](sgl_corpus_file const& a, sgl_corpus_file const& b) { return a.relative_path < b.relative_path; });
    return out;
}
} // namespace

INVOCABLE_TEST("sgl corpus - a file checks clean, passes its tests, and writes every entry point",
               (sgl_corpus_file const& f))
{
    auto const source = read_text(f.path);
    auto const tested = sgl::test_source(source, f.relative_path);
    CHECK(tested.errors == "");
    CHECK(tested.warnings == "");
    // a corpus file is there to test something
    CHECK(tested.test_count > 0);
    // and a test the compiler dropped without a diagnostic would otherwise pass unseen
    CHECK(tested.tests_run + tested.tests_expecting_diagnostics == tested.test_count);

    for (auto const& e : tested.entry_points)
        for (auto const t : sgl::emit::all_targets())
        {
            auto const text = sgl::compile_to_text(
                {.source = source, .source_name = f.relative_path, .entry_point = e.name, .target = t});
            CHECK(text.has_value()).dump("entry point", e.name).dump("target", sgl::emit::to_string(t));
            if (text.has_error())
                CHECK(text.error() == "");
        }
}

TEST("sgl corpus - every file")
{
    auto const files = corpus_files();
    // a broken SGL_CORPUS_DIR must never look like a clean run
    REQUIRE(files.size() > 0);
    for (auto const& f : files)
        nx::invoke_tests(f.relative_path, f);
}
