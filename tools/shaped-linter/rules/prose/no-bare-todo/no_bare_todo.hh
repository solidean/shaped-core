#pragma once

#include <shaped-linter/rules/rule.hh>

namespace scl
{

/// The `no-bare-todo` rule.
///
/// A `TODO`, `FIXME`, `HACK` or `XXX` in a C++ or Python comment.
/// Open work is an issue in a `docs/issues/` folder, and the code names it with `issue: <id>`, or `TEMPORARY(<id>)` on a workaround.
/// A note that leaves nothing open is a plain comment, and needs none of these words.
///
/// It matches the four words as whole words, in capitals, so `todo_list` and `Hackathon` are left alone.
/// A word inside backticks is a mention rather than a marker, which is how a comment talks about the convention itself.
/// Markdown is not checked: docs talk about TODOs, and an issue's own text is prose about open work.
///
/// No `fix`: the replacement names an issue, and only a human can file it or decide nothing is left open.
rule const& no_bare_todo_rule();

} // namespace scl
