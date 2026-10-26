#pragma once

#include <shaped-linter/rules/rule.hh>

namespace scl
{
/// The `no-stock-words` rule.
/// Prose avoids words that overuse has burned: they read as filler now, whatever they meant once.
/// It fires on a short list in its `.cc`, each entry added on its own, by a decision and a measured count: `load-bearing` first.
/// A word whose plain sense is common in code, like `exactly` in exact arithmetic, never qualifies.
///
/// It carries no fix: the replacement says what the word only gestured at, which only a writer knows.
/// It walks the prose layer of every language, a block at a time, so a phrase wrapped across two lines is still found.
rule const& no_stock_words_rule();
} // namespace scl
