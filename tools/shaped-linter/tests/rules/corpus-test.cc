#include "lint_corpus.hh"

#include <clean-core/common/utility.hh> // cc::min, cc::move
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/rules/engine.hh>

#include <algorithm> // std::sort
#include <filesystem>

// The data-driven half of rule testing: every markdown file under tests/rules/corpus/ becomes one
// invocation, addressable by its relative path. See lint_corpus.hh for the annotation format and
// ../../docs/coding-guidelines.md for which cases belong here rather than in a rule's smoke tests.

#ifndef SCL_CORPUS_DIR
#error "SCL_CORPUS_DIR must be defined by the build (see tools/shaped-linter/CMakeLists.txt)"
#endif

using namespace scl;

namespace
{
struct corpus_file
{
    cc::string path;          // absolute, for opening
    cc::string relative_path; // what the invocation is named after
};

/// Every `*.md` under the corpus root, in a stable order so invocation names never shuffle.
cc::vector<corpus_file> corpus_files()
{
    namespace fs = std::filesystem;

    auto out = cc::vector<corpus_file>();
    auto const root = fs::path(SCL_CORPUS_DIR);

    auto ec = std::error_code();
    for (auto const& e : fs::recursive_directory_iterator(root, ec))
    {
        if (!e.is_regular_file() || e.path().extension() != ".md")
            continue;
        auto const rel = fs::relative(e.path(), root, ec).generic_string();
        out.push_back({.path = cc::string(e.path().string().c_str()), .relative_path = cc::string(rel.c_str())});
    }

    std::sort(out.begin(), out.end(),
              [](corpus_file const& a, corpus_file const& b)
              {
                  auto const x = cc::string_view(a.relative_path);
                  auto const y = cc::string_view(b.relative_path);
                  auto const n = cc::min(x.size(), y.size());
                  for (auto i = isize(0); i < n; ++i)
                      if (x[i] != y[i])
                          return x[i] < y[i];
                  return x.size() < y.size();
              });
    return out;
}

/// How many findings named `id` are in `found`.
isize count_of(cc::span<finding const> found, cc::string_view id)
{
    auto n = isize(0);
    for (auto const& f : found)
        if (f.rule_id == id)
            ++n;
    return n;
}

/// How many times `id` appears in a list of annotated rule ids.
isize count_in(cc::span<cc::string const> ids, cc::string_view id)
{
    auto n = isize(0);
    for (auto const& x : ids)
        if (x == id)
            ++n;
    return n;
}
} // namespace

INVOCABLE_TEST("shaped-linter - corpus cases", (lint_corpus_group const& group))
{
    for (auto const& c : group.cases)
    {
        SECTION("{} (L{})", c.title, c.line)
        {
            // Both sides of every comparison carry the corpus location. nexus renders a failed
            // comparison as `lhs op rhs` built from the captured operands and drops a chained
            // `.context()`, so folding the location into the values is what makes a failure say which
            // markdown block broke instead of an anonymous `1 == 0`.
            auto const at = [&](auto const& value) { return cc::format("{} @ {}:{}", value, group.path, c.line); };

            auto const found = run_rules_on_text(c.source);

            // Every finding must be accounted for. A rule firing without an annotation is real signal
            // (rules cross-talk), so it fails until the block names it.
            CHECK(at(found.size()) == at(c.expect.size()));

            for (auto const& id : c.expect)
                CHECK(at(cc::format("{}x {}", count_of(found, id), id))
                      == at(cc::format("{}x {}", count_in(c.expect, id), id)));
            for (auto const& id : c.forbid)
                CHECK(at(cc::format("{}x {}", count_of(found, id), id)) == at(cc::format("0x {}", id)));

            if (c.fix.has_value())
            {
                auto fixes = cc::vector<cc::string_view>();
                for (auto const& f : found)
                    if (f.suggested_fix.has_value() && f.suggested_fix.value().edits.size() == 1)
                        fixes.push_back(f.suggested_fix.value().edits[0].replacement);

                REQUIRE(at(fixes.size()) == at(isize(1)));
                CHECK(at(fixes[0]) == at(c.fix.value()));
            }
        }
    }
}

TEST("shaped-linter - corpus files")
{
    auto const files = corpus_files();
    REQUIRE(files.size() > 0); // a broken SCL_CORPUS_DIR must never look like a clean run

    for (auto const& f : files)
    {
        auto group = load_lint_corpus(f.path, f.relative_path);
        REQUIRE(group.has_value());
        REQUIRE(group.value().cases.size() > 0); // a corpus file with no annotated block is a mistake

        nx::invoke_tests(f.relative_path, cc::move(group.value()));
    }
}
