#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>

// The lint corpus: a rule's breadth coverage written as an ordinary, readable markdown file.
//
// A corpus file is normal prose with C++ code blocks; the checks ride on each fence's info string:
//
//   ```cpp [default-init-assignment] fix=" = {}"
//   struct S { int value{}; };
//   ```
//
//   ```cpp ~[default-init-assignment]
//   void f() { g({1, 2}); }
//   ```
//
//   [rule-id]   the rule must produce one finding here (repeat the annotation for N findings)
//   ~[rule-id]  the rule must produce NO finding here
//   fix="…"     exactly one finding carries a suggested fix, whose replacement is this text
//
// A `cpp` block with no annotation, and any non-`cpp` block, is illustration and is not checked.
// See ../../docs/coding-guidelines.md for which cases belong here rather than in a rule's smoke tests.

namespace scl
{
/// One annotated code block from a corpus file.
struct lint_corpus_case
{
    cc::string title; // the nearest preceding heading — names the section this case runs under
    i32 line = 0;     // 1-based line of the opening fence, so a failure points into the file
    cc::string source;

    cc::vector<cc::string> expect; // one entry per expected finding, by rule id
    cc::vector<cc::string> forbid; // rule ids that must not fire
    cc::optional<cc::string> fix;  // the expected replacement text, when the block pins one
};

/// Every case in one corpus file. This is the invocable key type — a struct, never a bare primitive,
/// so `nx::invoke_tests` matches it unambiguously.
struct lint_corpus_group
{
    cc::string path; // relative to the corpus root, e.g. "default_init_assignment.md"
    cc::vector<lint_corpus_case> cases;
    isize skipped = 0; // unannotated code blocks, reported so silent skipping stays visible
};

/// Parse an already-loaded corpus file. Fails on a malformed annotation — a typo must not read as
/// "no expectations".
cc::result<lint_corpus_group> parse_lint_corpus(cc::string_view text, cc::string_view relative_path);

/// Read and parse a corpus file from disk.
cc::result<lint_corpus_group> load_lint_corpus(cc::string_view file_path, cc::string_view relative_path);
} // namespace scl
