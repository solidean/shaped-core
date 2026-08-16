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
/// **The arena is filled once and never grown afterwards.** Every value_view in the document points into it, and
/// growing a cc::vector reallocates, which would dangle every one of them at once.
///
/// Moving is safe: cc::allocation holds only pointers and its move is a steal, so the buffer keeps its address.
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

    /// The value bytes this snapshot owns, which is what a cache budget counts.
    [[nodiscard]] isize owned_byte_size() const { return _arena.size(); }

    snapshot_document(snapshot_document&&) noexcept = default;
    snapshot_document& operator=(snapshot_document&&) noexcept = default;
    snapshot_document(snapshot_document const&) = delete;
    snapshot_document& operator=(snapshot_document const&) = delete;

private:
    cc::vector<byte> _arena;
    raw_document _document;
};
