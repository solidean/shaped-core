#pragma once

#include <shaped-graphics-language/syntax/parsed_file.hh>

namespace sgl
{
/// Folds a tokenized line tree into group tokens: `file.root_block` and everything under it.
///
/// How a line ends decides what its children are.
/// After a block colon they are a block, inside an open string they are its content, inside an open paren they are
/// its elements, and otherwise they continue the line.
/// A line that ends with something open obliges its next sibling to start with the closer; a sibling that does not
/// has everything closed for it, with one diagnostic, and is read fresh.
/// So an unbalanced line reaches its children and the first token of its next sibling, and nothing else.
///
/// Must be called once, on a tokenized file.
void group_tokens(parsed_file& file);
} // namespace sgl
