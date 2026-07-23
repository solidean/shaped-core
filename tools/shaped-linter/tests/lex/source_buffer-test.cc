#include <nexus/test.hh>
#include <shaped-linter/lex/source_buffer.hh>

using namespace scl;

namespace
{
// Local helper: the braces of a `line_col{...}` would confuse the CHECK() macro's comma parsing.
line_col lc(u32 line, u32 column)
{
    return {.line = line, .column = column};
}
} // namespace

TEST("shaped-linter - source_buffer - line/col mapping")
{
    // Offset: a=0 b=1 \n=2 c=3 d=4 e=5 \n=6 f=7
    auto const b = source_buffer::from_text("ab\ncde\nf", "<mem>", 0);

    CHECK(b.line_col_at(0) == lc(1, 1)); // 'a'
    CHECK(b.line_col_at(1) == lc(1, 2)); // 'b'
    CHECK(b.line_col_at(2) == lc(1, 3)); // '\n' still on line 1
    CHECK(b.line_col_at(3) == lc(2, 1)); // 'c'
    CHECK(b.line_col_at(6) == lc(2, 4)); // '\n' after "cde", still line 2
    CHECK(b.line_col_at(7) == lc(3, 1)); // 'f'
}

TEST("shaped-linter - source_buffer - line starts after newline")
{
    auto const b = source_buffer::from_text("x\ny\nz", "<mem>", 0);
    CHECK(b.line_col_at(0) == lc(1, 1)); // x
    CHECK(b.line_col_at(2) == lc(2, 1)); // y
    CHECK(b.line_col_at(4) == lc(3, 1)); // z
}

TEST("shaped-linter - source_buffer - empty and clamping")
{
    auto const b = source_buffer::from_text("", "<mem>", 0);
    CHECK(b.line_col_at(0) == lc(1, 1));
    CHECK(b.line_col_at(999) == lc(1, 1)); // clamped

    auto const b2 = source_buffer::from_text("ab", "<mem>", 0);
    CHECK(b2.line_col_at(999) == lc(1, 3)); // clamped to end
}

TEST("shaped-linter - source_buffer - span_text")
{
    auto const b = source_buffer::from_text("hello world", "<mem>", 7);
    auto const s = source_span{.file_id = 7, .byte_begin = 6, .byte_end = 11};
    CHECK(b.span_text(s) == "world");
    CHECK(b.file_id() == 7);
    CHECK(b.path() == "<mem>");
}
