#pragma once

#include <shaped-graphics-language/fwd.hh>

/// A run of bytes in one file's source.
/// Every tree node and every token is addressed this way and owns no text, so a span means nothing without its file.
/// A file is limited to 4 GiB.
struct sgl::source_span
{
    u32 offset = 0;
    u32 length = 0;

    [[nodiscard]] constexpr u32 end() const { return offset + length; }
    [[nodiscard]] constexpr bool empty() const { return length == 0; }

    constexpr bool operator==(source_span const&) const = default;
};
