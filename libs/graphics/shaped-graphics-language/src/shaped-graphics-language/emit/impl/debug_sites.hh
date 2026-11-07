#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/emit/impl/plan.hh>

namespace sgl::emit::impl
{
/// Where the statements of one compile came from, read out of the files `sources` holds.
struct source_lines
{
    check::checked_module const& m;
    options const& sources;
    /// Per file, the offset each of its lines starts at; built on the first line asked of the file.
    cc::vector<cc::vector<u32>> line_starts;

    /// The source line of an expression or a statement of `file`; none where the file is not one of `sources`.
    [[nodiscard]] source_line line_of(i32 file, ast::expr_id expr, ast::stmt_id stmt);
    /// Where `s` of `e` came from: its own line, and the calls it was inlined through.
    /// The prelude's frames are left out wherever another frame remains.
    [[nodiscard]] line_origin origin_of(check::flat_entry_point const& e, check::flat_stmt const& s);
    [[nodiscard]] cc::string_view name_of(i32 file) const;
};

/// EMIT-183: fills `p`'s site of each print a target writes, so a text and a binary write one id and describe one site.
/// `writes` is false where the stage cannot write the buffer, whose prints are dropped and get no site.
void plan_debug_sites(plan& p, bool writes, options const& sources);
} // namespace sgl::emit::impl
