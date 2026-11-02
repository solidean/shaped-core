#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh>

/// What one print or assert of a shader writes into the context's debug buffer, and how the host reads it back.
///
/// A record in the buffer is the site's `id`, the record's word count, then each value's lanes in order, one 32-bit word each.
/// Only SGL states sites; a shader from any other compiler has none.
/// libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md, "Diagnostics", is the shader's side.

/// The kind of scalar one lane of a value is, which says how its word reads back.
enum class sg::debug_scalar : sg::u8
{
    float32,
    /// A half, which the shader widened to a float before writing it.
    float16,
    int32,
    uint32,
    /// 0 or 1.
    boolean,
};

/// One value a record holds.
struct sg::debug_value
{
    debug_scalar scalar = debug_scalar::float32;
    /// Each lane one word, x first.
    i32 lanes = 1;
    /// The value's type as the shader's source names it: `float3`, or an enum's name, whose values are int32.
    cc::string type;

    bool operator==(debug_value const&) const = default;
};

/// A line of a shader's source.
struct sg::debug_source_line
{
    cc::string file;
    /// Counted from 1.
    i32 line = 0;

    bool operator==(debug_source_line const&) const = default;
};

/// One print or assert of a shader.
struct sg::shader_debug_site
{
    /// What its records start with; one site has one id on every target.
    u32 id = 0;
    /// An assert's, which writes only where its condition was false.
    bool is_assert = false;
    /// The message around the values, one piece more than `values`; empty for a print of one bare value.
    cc::vector<cc::string> pieces;
    cc::vector<debug_value> values;
    /// Where it stands, and the calls it was inlined through, innermost first; `at.line` is 0 where the source was not named.
    debug_source_line at;
    cc::vector<debug_source_line> inlined_from;

    [[nodiscard]] bool operator==(shader_debug_site const& rhs) const
    {
        auto const same = [](auto const& a, auto const& b)
        {
            if (a.size() != b.size())
                return false;
            for (auto i = isize(0); i < a.size(); ++i)
                if (!(a[i] == b[i]))
                    return false;
            return true;
        };
        return id == rhs.id && is_assert == rhs.is_assert && same(pieces, rhs.pieces) && same(values, rhs.values)
            && at == rhs.at && same(inlined_from, rhs.inlined_from);
    }
};
