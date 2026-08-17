#pragma once

#include <clean-core/container/vector.hh>
#include <versioned-document/raw_document.hh>

/// A materialized document that owns every byte it points at.
///
/// The design is [snapshots](../../docs/concepts/snapshots.md).

/// One materialized document, owning every value byte it points at.
///
/// A raw_document borrows the payload of the op that wrote each value.
/// A snapshot must survive those ops being pruned away, so its values live in an arena of its own.
///
/// **The arena is a chunk list, and a chunk is never grown past the capacity it was reserved with.**
/// Every value_view in the document points into one, and growing a cc::vector reallocates, which would dangle every
/// view in that chunk at once.
/// Appending a new chunk is safe: the outer vector reallocating moves the inner vector OBJECTS, and a cc::vector's
/// move is a steal, so the bytes keep their address.
///
/// Chunks rather than one buffer are what makes advancing a snapshot possible — see `set_single_writer`.
class vdoc::snapshot_document
{
public:
    snapshot_document() = default;

    /// Deep-copies `doc` — the same entities, components, properties and writers, over bytes this object owns.
    ///
    /// `doc` must be the UNFILTERED materialization of a SINGLE head, or it is a projection rather than a document,
    /// and every sweep that later terminates on it is silently truncated.
    [[nodiscard]] static snapshot_document create_owning_copy(raw_document const& doc);

    /// Adopts an arena and a document already pointing into it — the decoder's one-copy path.
    /// Every value_view in `doc` must point inside `arena`, which is not checked.
    [[nodiscard]] static snapshot_document create_from_owned_arena(cc::vector<byte> arena, raw_document doc);

    [[nodiscard]] raw_document const& document() const { return _document; }

    /// Replaces every writer of `path` with `writer` alone, over a copy of `bytes`.
    /// The entity, component or property is inserted in sorted position where absent.
    ///
    /// **This is a document edit and says nothing about whether it is a sound one.**
    /// The only place it is — where the result is still `surviving` of some op — is a single-parent child's
    /// assignments applied to its parent's snapshot, which is what `advance_snapshot` does and checks.
    void set_single_writer(property_path const& path, op_id const& writer, cc::span<byte const> bytes);

    /// `set_single_writer`, but a no-op reporting false when the stored bytes already equal `bytes`.
    ///
    /// **One walk rather than a lookup plus a write**, which is what a producer rewriting its whole layer every frame
    /// pays per property.
    /// A multi-valued path always counts as changed, for the same reason it always does in an op's diff.
    ///
    /// `out_inserted` reports whether the path was not there before.
    /// A caller doing mark-and-sweep needs that to know whether the path SET changed, which is a different question from
    /// whether any value did — and answering it here is what lets the sweep be skipped entirely.
    bool set_single_writer_if_changed(property_path const& path,
                                      op_id const& writer,
                                      cc::span<byte const> bytes,
                                      bool* out_inserted = nullptr);

    /// How many property paths carry a writer.
    /// Maintained rather than counted, because a mark-and-sweep asks it once per rebuild.
    [[nodiscard]] isize property_count() const { return _property_count; }

    /// Removes every writer of `path`, and then the component and entity it leaves empty.
    ///
    /// **Pruning the empty parents is the load-bearing half.** A component entry with zero properties is not what a
    /// fresh materialization would ever produce, and a parse would *select* it — schema found, `$alive` absent so alive,
    /// version 0 — and construct an all-defaults component out of nothing.
    ///
    /// Same soundness caveat as `set_single_writer`: this is a document edit, and the one place it is a sound one is a
    /// single-parent child's abstention applied to its parent's snapshot.
    void clear_writers(property_path const& path);

    /// The value bytes this snapshot owns, which is what a cache budget counts.
    [[nodiscard]] isize owned_byte_size() const { return _owned_bytes; }

    /// Bytes no writer points at any more, left behind by `set_single_writer`.
    /// A caller advancing a snapshot repeatedly watches this and rebuilds once it outgrows the live bytes.
    [[nodiscard]] isize dead_byte_size() const { return _dead_bytes; }

    snapshot_document(snapshot_document&&) noexcept = default;
    snapshot_document& operator=(snapshot_document&&) noexcept = default;
    snapshot_document(snapshot_document const&) = delete;
    snapshot_document& operator=(snapshot_document const&) = delete;

private:
    /// Copies `bytes` into the chunk list and returns a view of the copy.
    [[nodiscard]] value_view impl_append(cc::span<byte const> bytes);

    /// Each reserved once and never grown, so appending cannot dangle a view into an earlier chunk.
    cc::vector<cc::vector<byte>> _chunks;

    isize _owned_bytes = 0;
    isize _dead_bytes = 0;

    /// Maintained by every mutator, so `property_count` is O(1) rather than a walk.
    isize _property_count = 0;

    raw_document _document;
};
