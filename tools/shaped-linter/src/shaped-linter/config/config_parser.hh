#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/config/config_value.hh>
#include <shaped-linter/fwd.hh>

namespace scl
{

/// Parse the `.shaped-lint.yml` format: a strict YAML subset, small enough to hand-roll on clean-core.
/// shaped-linter-core links clean-core alone, so babel's readers are out of reach here — the same
/// reasoning `markdown_scanner` records.
///
/// What it accepts:
///  - `#` to end of line is a comment, unless it sits inside quotes; blank lines are ignored.
///  - The top level is a mapping of `key: value` entries.
///  - A value is a scalar (the rest of the line, trimmed, optionally `'`/`"` quoted), a flow list
///    `[a, b, c]`, or — when the line ends after the `:` — an indented block on the lines below.
///  - A block is a list of `- ` items or a nested mapping, and its items may themselves be mappings.
///
/// Indentation is spaces only: **a tab is an error**, never a silent 8 columns.
/// Siblings must align exactly and a nested block must be strictly deeper; the width is otherwise free,
/// though everything we write uses two.
///
/// What it does NOT accept, deliberately: anchors, tags, multi-line scalars, flow mappings, and a block list at its key's own indentation.
/// Every one of those is an error rather than a quiet reinterpretation.
///
/// Errors carry a line and column, and the caller prefixes the path.
cc::result<config_document> parse_config(cc::string_view text);

} // namespace scl
