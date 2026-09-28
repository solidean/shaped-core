#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-shader-library/fwd.hh>

/// Where a shader's text puts the values of a constant block or a buffer element: what SGL states of its own text,
/// and what a compiler reads off its output, which slib compares.

/// One builtin value: its name, dotted into a nested struct, and its offset from the start.
struct slib::block_field
{
    cc::string name;
    isize offset = 0;
};

/// A constant block, or the element of a buffer.
struct slib::block_layout
{
    /// The global the text reads it through.
    cc::string global;
    /// The bytes one element of a buffer takes; 0 for a block.
    isize stride = 0;
    /// Every builtin value, in memory order; empty for a buffer of a builtin, which only its stride describes.
    cc::vector<block_field> fields;
};
