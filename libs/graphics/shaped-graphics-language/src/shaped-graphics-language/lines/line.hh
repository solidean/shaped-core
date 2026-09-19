#pragma once

#include <shaped-graphics-language/fwd.hh>
#include <shaped-graphics-language/source/source_span.hh>

/// What a line is, which its position in the tree decides before a single token is read.
enum class sgl::line_kind : sgl::u8
{
    /// Empty or whitespace only; never opens or closes anything and carries no tokens.
    blank,
    code,
    /// A child of a comment-only line, however it is indented below it.
    comment,
    /// A child of a line that ended in an open quote: content of a multi-line string.
    string_content,
};

/// One physical source line, and one node of the line tree.
///
/// Lines are stored in source order, which is also pre-order, so a parent always precedes its children and a line
/// always follows its previous sibling's whole subtree.
/// Links are indices into `parsed_file::lines`, -1 for none.
struct sgl::line
{
    /// The line without its terminator, indentation included.
    source_span text;
    /// Bytes of `\n`, `\r\n` or `\r` after `text`; 0 only on a last line that has none.
    u8 terminator_length = 0;
    /// Set by the line tree for `blank`, and by the tokenizer for everything else.
    line_kind kind = line_kind::blank;
    /// Leading whitespace in bytes, and in columns with a tab advancing to the next multiple of four.
    /// A blank line has neither, whatever whitespace it holds.
    u32 indent_bytes = 0;
    u32 indent_columns = 0;

    i32 parent = -1;
    i32 first_child = -1;
    i32 next_sibling = -1;

    /// This line's tokens in `parsed_file::tokens`; empty until tokenized, and always for a blank line.
    u32 first_token = 0;
    u32 token_count = 0;
};
