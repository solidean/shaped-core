#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <versioned-document-file/assets.hh>
#include <versioned-document-file/fwd.hh>
#include <versioned-document/op.hh>

/// What a load found wrong, and could carry on past.
///
/// **Soft failures never block a load.** The damaged part is dropped and reported, and the rest of the file opens.
/// A corrupt op lands on exactly the same downstream path as a pruned one, which is what makes that path get exercised rather than rot.
///
/// A hard failure never reaches here: it rides the open's failure channel instead.
/// The complete list is in [format.md](../../docs/format.md#loading), and is short on purpose.

/// Why part of a file would not load.
///
/// String-free by design — a kind plus the id it concerns.
/// A caller localizes the message and a test asserts on the kind exactly, neither of which survives a formatted string.
enum class vdoc::file::load_issue_kind : vdoc::file::u8
{
    /// An op row's bytes would not decode.
    op_decode_failed,

    /// The bytes do not hash to the stored id — corruption or tampering.
    /// A skeleton op is unverifiable by construction and is **never** reported here.
    op_hash_mismatch,

    /// An op names a parent not in the file.
    /// Informational, and normal after pruning.
    missing_parent,

    /// A snapshot row would not decode.
    /// A *required* one that will not decode is a hard failure instead, because the history it stood in for is gone.
    missing_snapshot,

    /// A ref names an op that is not in the file — usually one this load dropped.
    /// The ref is kept anyway: discarding somebody's ref is not a loader's decision.
    dangling_ref,

    /// An asset row's `parts` blob would not decode, or is not the array of part objects it must be.
    asset_decode_failed,

    /// An asset names a content hash with no blob row.
    asset_blob_missing,

    /// The blob row exists but its chunks do not all.
    asset_blob_incomplete,

    /// A blob names an encoding this build does not have.
    /// The blob is skipped, never the open.
    unknown_encoding,

    /// A workspace row's value would not decode.
    /// The row is left in place, because only dirty keys are ever written.
    workspace_decode_failed,

    /// A table this build does not know; ignored, and left untouched.
    unknown_table,

    /// A column this build does not know on a table it does know; ignored, and preserved.
    unknown_column,
};

/// One thing that would not load, and which op, asset, blob or table it concerns.
///
/// Which member carries the subject depends on `kind`, and the rest read as their empty value:
///   op_decode_failed / op_hash_mismatch / missing_snapshot — `op`
///   missing_parent                                          — `op` is the child, `parent` the id it names
///   dangling_ref                                            — `name` is the ref, `op` the id it names
///   asset_decode_failed                                     — `name` is the asset id
///   asset_blob_missing / asset_blob_incomplete              — `name` is the asset id, `blob` the hash it names
///   unknown_encoding                                        — `blob`, and `name` is the encoding it asked for
///   workspace_decode_failed                                 — `name` is the key
///   unknown_table                                           — `name` is the table
///   unknown_column                                          — `name` is "<table>.<column>"
struct vdoc::file::load_issue
{
    load_issue_kind kind = load_issue_kind::op_decode_failed;
    vdoc::op_id op;
    vdoc::op_id parent;
    blob_hash blob;
    cc::string name;

    [[nodiscard]] friend bool operator==(load_issue const& a, load_issue const& b) = default;
};

/// Everything a load found.
///
/// The order is the load order, which is [format.md](../../docs/format.md#loading)'s, so it is deterministic without being sorted.
struct vdoc::file::load_report
{
    cc::vector<load_issue> issues;

    [[nodiscard]] bool is_empty() const { return issues.empty(); }

    [[nodiscard]] isize count_of(load_issue_kind kind) const;
    [[nodiscard]] bool contains(load_issue_kind kind) const { return count_of(kind) > 0; }

    /// The first issue of this kind, or null.
    /// What a test reaches for when it wants to name the op or asset a kind was reported against.
    [[nodiscard]] load_issue const* find_first(load_issue_kind kind) const;

    void clear() { issues.clear(); }
};
