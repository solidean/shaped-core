#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <versioned-document/change_set.hh>
#include <versioned-document/snapshot_document.hh>

/// A layer written directly, with no op graph and no history behind it.
///
/// The design is [layering](../../docs/concepts/layering.md).

namespace vdoc
{
/// A stable synthetic writer id for a directly written layer, derived from its name.
///
/// Domain-separated from `"vdoc::op/v1"`, so it cannot collide with any real op id, and a function of the name alone, so
/// writer sorts and diagnostics are reproducible across runs and machines.
///
/// **These ids name no op**, so a composed document containing one must never be installed into a `snapshot_cache` or
/// written to a file — see [snapshot_cache](snapshot_cache.hh).
[[nodiscard]] op_id synthetic_writer_id(cc::string_view layer_name);
} // namespace vdoc

/// A layer a C++ producer writes property by property, rather than one materialized from ops.
///
/// This is what the base of a [layer_stack](layer_stack.hh) usually is: a per-frame document some other system computes,
/// which has no history worth keeping and no need for one.
/// It owns its bytes, so it outlives nothing and borrows nothing.
///
/// **Its writes are diffed**, so a producer may re-write everything every frame and still hand the stack a small dirty
/// set: `set` compares the new bytes against what is stored and reports nothing changed when they match.
/// That trades O(n) re-interpretation for O(n) property writes.
///
/// **But those writes are not free, and a wholesale rebuild has a scale limit.**
/// Each one is a walk of three nested sorted vectors with an interned-string comparison at every step, measured at
/// roughly 100 ns — so rewriting 8,000 entities of 7 properties costs about 6 ms, which is most of a frame.
/// It is comfortable to a couple of thousand entities and no further; past that a producer should write only what moved,
/// and `mark_dirty` is how one that already knows skips the compares entirely.
/// The numbers are in [the benchmark](../../docs/benchmarks/edit-latency-benchmark.md#layering-per-frame).
///
/// **One layer belongs to one stack.** The stack consumes the accumulated dirty set as it applies, so two stacks sharing
/// a layer would each see half of it.
class vdoc::direct_layer
{
    // lifetime
public:
    /// `name` decides the writer id every value here is attributed to, so two layers wanting distinct provenance need
    /// distinct names.
    explicit direct_layer(cc::string_view name);

    direct_layer(direct_layer&&) noexcept = default;
    direct_layer& operator=(direct_layer&&) noexcept = default;
    direct_layer(direct_layer const&) = delete;
    direct_layer& operator=(direct_layer const&) = delete;

    // writing
public:
    /// Writes a value, and reports the path dirty only if the bytes actually differ.
    void set(property_path const& path, value_view v);

    /// Withdraws this layer's contribution to the path, so a lower layer shows through.
    /// The same meaning as an op's [abstention](op.hh), reached without an op.
    void abstain(property_path const& path);

    /// Reports a path dirty without inspecting it, for a producer that already knows.
    /// Over-reporting is safe — a change set may always claim more than changed.
    void mark_dirty(property_path const& path);

    /// Opens a wholesale rebuild: every path not written again before `finish_rebuild` is dropped.
    ///
    /// This is what lets a producer that recomputes everything express removal without tracking it.
    /// Nesting is not allowed and asserts.
    void begin_rebuild();

    /// Closes a rebuild, withdrawing every path this pass did not write and reporting each dirty.
    void finish_rebuild();

    /// Drops everything, reporting the whole layer dirty.
    void clear();

    // identity
public:
    [[nodiscard]] cc::string_view name() const { return _name; }

    /// The writer id every value in this layer is attributed to.
    [[nodiscard]] op_id writer() const { return _writer; }

    /// Bumped by every mutator, so a stack can tell it moved.
    ///
    /// **Bumped here rather than by the caller**, because "forgot to say it changed" is exactly the silent staleness
    /// this exists to make impossible.
    /// It is process-local and good for nothing but that — a graph-backed layer's version is an op id, which is a real
    /// content address.
    [[nodiscard]] u64 version() const { return _version; }

    [[nodiscard]] raw_document const& document() const { return _doc.document(); }

private:
    /// Hands over the paths dirtied since the last call and starts a fresh set.
    /// The stack's only way in, because consuming it twice would lose half of it.
    [[nodiscard]] change_set impl_take_changes();

    /// Records a path as written during a rebuild, so `finish_rebuild` keeps it.
    void impl_note_written(property_path const& path);

    /// Withdraws the path and reports it dirty, without touching the rebuild bookkeeping.
    void impl_withdraw(property_path const& path);

    cc::string _name;
    op_id _writer;
    u64 _version = 0;

    snapshot_document _doc;
    change_set_builder _changes;

    bool _rebuilding = false;

    /// Whether the open rebuild introduced a path that was not there before.
    ///
    /// With no insert and one write per stored path, the path set cannot have changed — which is what lets
    /// `finish_rebuild` skip its sweep on a steady-state frame, and that sweep is otherwise the frame's whole cost.
    bool _inserted_during_rebuild = false;

    /// Paths written since `begin_rebuild`, unsorted; sorted and searched only when the sweep cannot be skipped.
    cc::vector<property_path> _written_this_rebuild;

    friend class layer_stack;
};
