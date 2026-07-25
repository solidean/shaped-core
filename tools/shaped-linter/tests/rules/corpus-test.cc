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

bool has(cc::span<cc::string_view const> haystack, cc::string_view needle)
{
    for (auto const& x : haystack)
        if (x == needle)
            return true;
    return false;
}

/// Every distinct rule id the block names, in first-mention order — one comparison per rule, so a failure
/// says which rule went wrong rather than only that some total was off.
cc::vector<cc::string_view> mentioned_rules(cc::span<lint_corpus_expectation const> expect)
{
    auto out = cc::vector<cc::string_view>();
    for (auto const& e : expect)
        if (!has(out, e.rule_id))
            out.push_back(e.rule_id);
    return out;
}

/// The set of replacement texts, rendered sorted and comma-joined so two sets compare as one readable
/// string. Each is bracketed, not quoted — a fix normally starts with a space, and nexus already quotes
/// the whole rendered string. Duplicates are merged: what is pinned is WHICH rewrites a rule offers.
cc::string render_fix_set(cc::span<cc::string_view const> texts)
{
    auto sorted = cc::vector<cc::string_view>();
    for (auto const& t : texts)
        if (!has(sorted, t))
            sorted.push_back(t);
    std::sort(sorted.begin(), sorted.end(),
              [](cc::string_view a, cc::string_view b)
              {
                  auto const n = cc::min(a.size(), b.size());
                  for (auto i = isize(0); i < n; ++i)
                      if (a[i] != b[i])
                          return a[i] < b[i];
                  return a.size() < b.size();
              });

    auto out = cc::string();
    for (auto const& t : sorted)
    {
        if (!out.empty())
            out += ", ";
        out += cc::format("[{}]", t);
    }
    return out.empty() ? cc::string("(none)") : out;
}

/// Every replacement `id` produced here — over all its findings and all their edits.
cc::vector<cc::string_view> fixes_produced_by(cc::span<finding const> found, cc::string_view id)
{
    auto out = cc::vector<cc::string_view>();
    for (auto const& f : found)
    {
        if (f.rule_id != id || !f.suggested_fix.has_value())
            continue;
        for (auto const& e : f.suggested_fix.value().edits)
            out.push_back(e.replacement);
    }
    return out;
}

/// Every replacement the block wrote for `id`, gathered across all of that rule's annotations.
cc::vector<cc::string_view> fixes_pinned_for(cc::span<lint_corpus_expectation const> expect, cc::string_view id)
{
    auto out = cc::vector<cc::string_view>();
    for (auto const& e : expect)
    {
        if (e.rule_id != id)
            continue;
        for (auto const& f : e.fixes)
            out.push_back(f);
    }
    return out;
}
} // namespace

INVOCABLE_TEST("shaped-linter - corpus cases", (lint_corpus_group const& group))
{
    for (auto const& c : group.cases)
    {
        SECTION("{} (L{})", c.title, c.line)
        {
            auto const where = cc::format("{}:{}", group.path, c.line); // the block a failure came from
            auto const found = run_rules_on_text(c.source);

            // How many findings each named rule owes: one per positive annotation, none for a `~[…]`.
            auto expected_total = isize(0);
            for (auto const& e : c.expect)
                expected_total += e.negated ? 0 : 1;

            // Every finding must be accounted for. A rule firing without an annotation is real signal
            // (rules cross-talk), so it fails until the block names it.
            CHECK(found.size() == expected_total).context(where);

            for (auto const& id : mentioned_rules(c.expect))
            {
                auto expected = isize(0);
                for (auto const& e : c.expect)
                    if (e.rule_id == id && !e.negated)
                        ++expected;

                CHECK(count_of(found, id) == expected).context(where).dump("rule", id);

                // Naming one fix for a rule means naming them all: the sets must match exactly.
                auto const pinned = fixes_pinned_for(c.expect, id);
                if (!pinned.empty())
                    CHECK(render_fix_set(fixes_produced_by(found, id)) == render_fix_set(pinned))
                        .context(where)
                        .dump("rule", id);
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
