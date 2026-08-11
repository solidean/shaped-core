#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>

// The lint corpus: a rule's breadth coverage written as an ordinary, readable markdown file.
//
// A corpus file is normal prose with C++ code blocks, and the expectations ride on each fence's info string:
//
//   ```cpp [default-init-assignment] fix=" = {}"
//   struct S { int value{}; };
//   ```
//
// The annotation spellings, the escape rules, the set semantics of `fix=` / `hint=` and the `path=` language dispatch are all specified in
// ../../docs/coding-guidelines.md, section "The corpus format" — which also says which cases belong in a corpus rather than in a rule's smoke tests.
// The types below are the parse of exactly that format.

namespace scl
{
struct lint_corpus_expectation;
struct lint_corpus_case;
struct lint_corpus_group;
} // namespace scl

/// One `[rule-id]` / `~[rule-id]` annotation, with whatever `fix="…"` / `hint="…"` was chained onto it.
struct scl::lint_corpus_expectation
{
    cc::string rule_id;
    bool negated = false;         // `~[rule-id]`: this rule must not fire at all
    cc::vector<cc::string> fixes; // replacement texts, merged per rule id with every other annotation's
    cc::vector<cc::string> hints; // the same, for the rewrites `--fix` does not apply
};

/// One annotated code block from a corpus file.
struct scl::lint_corpus_case
{
    cc::string title; // the nearest preceding heading — names the section this case runs under
    i32 line = 0;     // 1-based line of the opening fence, so a failure points into the file
    cc::string source;
    cc::string path; // what to lint the block AS; empty means the default `<memory>`

    cc::vector<lint_corpus_expectation> expect; // in annotation order
};

/// Every case in one corpus file.
/// This is the invocable key type — a struct, never a bare primitive, so `nx::invoke_tests` matches it unambiguously.
struct scl::lint_corpus_group
{
    cc::string path; // relative to the corpus root (tools/shaped-linter/rules/), so the group folder is part of it
    cc::vector<lint_corpus_case> cases;
    isize skipped = 0; // unannotated lintable blocks; counted here, but nothing reads it yet
};

namespace scl
{
/// Parse an already-loaded corpus file.
/// Fails on a malformed annotation, because a typo must not read as "no expectations".
cc::result<lint_corpus_group> parse_lint_corpus(cc::string_view text, cc::string_view relative_path);

/// Read and parse a corpus file from disk.
cc::result<lint_corpus_group> load_lint_corpus(cc::string_view file_path, cc::string_view relative_path);
} // namespace scl
