#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/span.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

/// Relocating a link of a library's chain onto a link before it that changed, rather than checking it again (the
/// compilation model's early cutoff).
///
/// A link's part of every list is contiguous, and an id or a position names an entry of one list.
/// So a link's state checked over the old links is the new links' state and its own part, every id in that part moved:
/// one below the first changed link's part stays, one in a part moves with that part, and one into what a changed link
/// no longer holds refuses the relocation, which then checks the link again.
namespace sgl::check::impl
{
[[nodiscard]] table_sizes sizes_of(checker const& c);

/// The entries `from` to `to` of every list an id names, written with no place in a file in them, hashed: two checks
/// of a module are the same interface when these agree, wherever its declarations stand in its files.
[[nodiscard]] cc::hash128 interface_hash(checker& c, table_sizes const& from, table_sizes const& to);

/// Where one link's part of the lists moved.
/// An entry from `old_start` to `old_kept` is the same entry at `new_start` on; one from `old_kept` to `old_end` is
/// gone, and an id naming it refuses the relocation.
struct moved_part
{
    table_sizes old_start;
    table_sizes old_kept;
    table_sizes old_end;
    table_sizes new_start;
};

/// The state of `link`, checked over `old_base`, as it is over `new_base`, `moved` holding every part from the first
/// that changed up to `old_base`'s own; none where the move would make anything another fresh check would not.
[[nodiscard]] cc::unique_ptr<checker> relocated_state(checker const& link,
                                                      checker const& old_base,
                                                      checker const& new_base,
                                                      cc::span<moved_part const> moved);

/// Ends a layer's state: what a program's check behind it starts from, and the lists it walks only past what it holds.
/// `files` is how many files it declared.
[[nodiscard]] resume_point seal_layer(checker& c, i32 files);
} // namespace sgl::check::impl
