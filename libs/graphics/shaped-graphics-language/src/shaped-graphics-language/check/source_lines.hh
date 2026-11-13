#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/check/flat.hh>
#include <shaped-graphics-language/fwd.hh>

/// Where the statements of one compile came from, read out of the files it was given.
struct sgl::check::source_lines
{
    checked_module const& m;
    cc::span<module_file const> files;
    cc::span<cc::string_view const> names;
    /// Per file, the offset each of its lines starts at; built on the first line asked of the file.
    cc::vector<cc::vector<u32>> line_starts;

    /// The source line of an expression or a statement of `file`; none where the file is not one of `files`.
    [[nodiscard]] source_line line_of(i32 file, ast::expr_id expr, ast::stmt_id stmt);
    /// The source line `offset` of `file` stands on; none where the file is not one of `files`.
    [[nodiscard]] source_line line_at(i32 file, u32 offset);
    /// Where `s` of `e` came from: its own line, and the calls it was inlined through.
    /// The prelude's frames are left out wherever another frame remains.
    [[nodiscard]] line_origin origin_of(flat_entry_point const& e, flat_stmt const& s);
    [[nodiscard]] cc::string_view name_of(i32 file) const;
};
