#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/check/impl/archive.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

/// Moving a link of a library's chain onto links before it that changed, rather than checking it again (the
/// compilation model's early cutoff): past an edit in memory, and from an artifact.
///
/// A link's part of every list is contiguous, and an id or a position names an entry of one list.
/// So a link's state checked over the old links is the new links' state and its own part, every id in that part moved:
/// one below the first changed link's part stays, one in a part moves with that part, and one into what a changed link
/// no longer holds refuses the move, which then checks the link again.
/// A link's own part is written as bytes and read back over the new links, so a move in memory and the load of an
/// artifact are one path.
namespace sgl::check::impl
{
[[nodiscard]] table_sizes sizes_of(checker const& c);

/// The entries `from` to `to` of every list an id names, written with no place in a file in them, hashed: two checks
/// of a module are the same interface when these agree, wherever its declarations stand in its files.
[[nodiscard]] cc::hash128 interface_hash(checker& c, table_sizes const& from, table_sizes const& to);

/// Where one link's part of the lists moved.
/// An entry from `old_start` to `old_kept` is the same entry at `new_start` on; one from `old_kept` to `old_end` is
/// gone, and an id naming it refuses the move.
struct moved_part
{
    table_sizes old_start;
    table_sizes old_kept;
    table_sizes old_end;
    table_sizes new_start;
};

/// `a` moved by `to - from` in every list.
[[nodiscard]] table_sizes plus(table_sizes a, table_sizes const& from, table_sizes const& to);
/// Whether `a_from` to `a_to` spans as many entries of every list as `b_from` to `b_to`.
[[nodiscard]] bool same_shape(table_sizes const& a_from,
                              table_sizes const& a_to,
                              table_sizes const& b_from,
                              table_sizes const& b_to);

/// What `link` added to `old_base`'s state, which it was checked over, written into `a` as a move reads it back.
/// `a.sources` must hold the prelude's files and the link's own, and `a.elsewhere` the files of the links between.
void write_own_part(archive<true>& a, checker const& link, checker const& old_base);

/// What `write_own_part` wrote, read from `a` into `state`, a copy of the new base's, every id moved through `moved`:
/// its last part the link's own.
/// False where an id names what a part no longer holds, a fresh check could have found a new base's entry instead,
/// or the bytes are not such a part, all of them read.
[[nodiscard]] bool read_own_part(archive<false>& a, checker& state, cc::span<moved_part const> moved);

/// Ends a layer's state: what a program's check behind it starts from, and the lists it walks only past what it holds.
/// `files` is how many files it declared; `is_link` is a link of a library's chain.
[[nodiscard]] resume_point seal_layer(checker& c, i32 files, bool is_link);

/// What makes a layer of a chain without checking it, for the functions that do: `check_next_module`,
/// `relocate_link` and `load_link_artifact`.
struct layer_maker
{
    struct next_link
    {
        checked_prelude layer;
        module_plan plan;
    };

    /// The layer `check_next_module` makes of `module` over `base`, all of it but its state; none where `module` may not
    /// be the next link.
    [[nodiscard]] static cc::optional<next_link> plan_next(checked_prelude const& base,
                                                           cc::span<module_file const> library,
                                                           cc::string_view module);

    /// The sources a link's own part names a place in, the prelude's files and its own: the first `prelude` and those
    /// from `own_from` on of `files`.
    [[nodiscard]] static cc::vector<cc::string_view> own_sources(cc::span<module_file const> files,
                                                                 i32 prelude,
                                                                 isize own_from);
    /// The sources of `files` from `own_from` on, hashed: a link's `chain_part::sources_digest`.
    [[nodiscard]] static cc::hash128 sources_digest(cc::span<module_file const> files, isize own_from);
    /// The sources of the files of the links between: those of `files` from `prelude` to `own_from`.
    [[nodiscard]] static cc::vector<cc::string_view> sources_between(cc::span<module_file const> files,
                                                                     i32 prelude,
                                                                     isize own_from);

    /// `layer`'s state: the own part `a` reads of a link checked over the links `old_chain` names, its own being
    /// `own`, moved onto `new_base`; false where a fresh check could differ, or the bytes are not such a part.
    /// `bodies` are the functions of earlier links whose bodies the link read.
    [[nodiscard]] static bool move_onto(checked_prelude& layer,
                                        checked_prelude const& new_base,
                                        archive<false>& a,
                                        cc::span<checker::chain_part const> old_chain,
                                        checker::chain_part const& own,
                                        cc::span<symbol_id const> bodies);
};
} // namespace sgl::check::impl
