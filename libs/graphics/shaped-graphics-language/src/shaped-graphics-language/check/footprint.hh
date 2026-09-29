#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/check/flat.hh>

/// What an entry point's code does to each binding it lists, which is what sg follows to place barriers.
/// See the spec's bindings file, "Footprint".

/// Which kind of view a slot is bound through, which decides how a host spells a read of it.
enum class sgl::check::slot_view : sgl::u8
{
    /// A binding's constant block, read as a whole.
    constants,
    /// A `buffer` or a `texture`: read-only.
    read_only,
    /// A `mut buffer` or an `image` of any access: a writable view, through which even a read is a storage access.
    storage,
};

/// How the code touches one slot of a binding: one resource member, or the binding's constant block as a whole.
struct sgl::check::slot_footprint
{
    symbol_id binding = symbol_id::none;
    /// A position in the binding's `members`, or -1 for its constant block.
    i32 member = -1;
    /// What the host calls the slot: `work.values` for a member, `work` for the constant block.
    cc::string host_name;
    slot_view view = slot_view::read_only;
    bool reads = false;
    bool writes = false;

    bool operator==(slot_footprint const&) const = default;
};

namespace sgl::check
{
/// Every slot the code of `e` touches, in the order of its binding list and then of each binding's members.
///
/// `e` must be legalized — the tree an emitter prints — so the footprint covers exactly what the text uses:
/// a check or an assert is gone from it, and so is everything they alone read.
/// Samplers are left out, since nothing orders against one, and so is an `@inline` binding, which sg sets as constants.
[[nodiscard]] cc::vector<slot_footprint> footprint_of(checked_module const& m, flat_entry_point const& e);

/// `footprint` as a corpus pin writes it: `work: read, work.values: read write`, in footprint order.
[[nodiscard]] cc::string footprint_text(cc::span<slot_footprint const> footprint);
} // namespace sgl::check
