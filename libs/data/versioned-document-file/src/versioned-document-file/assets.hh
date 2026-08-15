#pragma once

#include <clean-core/common/hash256.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <versioned-document-file/fwd.hh>
#include <versioned-document/value.hh>

#include <compare>

/// The asset vocabulary: a name pointing at an ordered list of parts, each part pointing at a blob.
///
/// Milestone 4 lands these types and the load path that fills them; the asset *machinery* — storing, resolving and reclaiming — is milestone 5.
/// The shape is fixed here so a file written today is readable by that code without a format change.
///
/// **Only history is immutable.** Blobs are content-addressed and shared, but the name -> asset mapping is mutable and remapping is retroactive, on purpose.
/// So op ids do not commit to asset content, and a document is reproducible only relative to an asset resolution.
/// The argument is [decisions.md](../../../versioned-document/docs/decisions.md#the-asset-mapping-is-mutable-and-remapping-is-retroactive).

/// Content hash of a blob: a 32-byte BLAKE3 digest over the DECODED bytes.
///
/// A distinct type from vdoc::op_id rather than a reuse of it, because the two are never interchangeable and a mix-up
/// would be a silent data error instead of a compile error.
/// Ordering is by the canonical 32 bytes, for the same reason op_id's is: it is a wire-format property.
struct vdoc::file::blob_hash
{
    /// The 32 canonical bytes a blob hash is, as stored and as compared.
    static constexpr isize byte_size = 32;

    cc::hash256 digest;

    /// The all-zero hash, which no real blob has: it is what an unset part reads as.
    constexpr blob_hash() = default;
    explicit constexpr blob_hash(cc::hash256 d) : digest(d) {}

    [[nodiscard]] friend constexpr bool operator==(blob_hash const&, blob_hash const&) = default;

    /// Writes the canonical form; `out` must be exactly byte_size bytes.
    void to_bytes(cc::span<byte> out) const { digest.to_bytes(out); }

    /// Reads the canonical form; `data` must be exactly byte_size bytes.
    [[nodiscard]] static blob_hash from_bytes(cc::span<byte const> data)
    {
        return blob_hash(cc::hash256::from_bytes(data));
    }

    /// The content hash of `bytes` — the one definition of blob content addressing.
    /// Hashed at import, never on a read path.
    [[nodiscard]] static blob_hash of(cc::span<byte const> bytes);

    /// Orders by the canonical 32 bytes, which is the order the hex digests sort in.
    [[nodiscard]] std::strong_ordering compare_bytes(blob_hash const& rhs) const;

    /// Sort predicate over the canonical bytes, so a durable set has a reproducible order.
    struct by_bytes
    {
        [[nodiscard]] bool operator()(blob_hash const& lhs, blob_hash const& rhs) const
        {
            return lhs.compare_bytes(rhs) < 0;
        }
    };

    [[nodiscard]] friend u64 hash(blob_hash const& v) { return hash(v.digest); }
};

/// One part of an asset.
///
/// **Order is the contract; the name is for humans.**
/// Nothing may key behaviour on a part name, because the moment something does, renaming a part becomes a format change.
struct vdoc::file::asset_part
{
    blob_hash hash;
    /// What the bytes are, e.g. "png" — selects a parser downstream.
    cc::string format;
    /// Optional, and DEBUG ONLY.
    cc::string name;

    [[nodiscard]] friend bool operator==(asset_part const& a, asset_part const& b) = default;
};

/// One row of the asset index: what the asset is, its parts, and its informational metadata.
///
/// An empty `parts` is legal, and means an asset that has metadata but no bytes.
struct vdoc::file::asset_record
{
    /// The same string a document property holds.
    cc::string asset_id;
    /// Load-bearing: what the asset is.
    cc::string kind;
    /// ORDERED — see asset_part.
    cc::vector<asset_part> parts;
    /// Informational; nothing keys behaviour on it.
    vdoc::value meta;

    /// False once a load found a part whose blob is missing or incomplete.
    /// Set by the load and never by a caller, and publishing ignores it.
    bool is_resolvable = true;
};

/// A blob offered for storage, in whatever encoding it is stored as.
struct vdoc::file::blob_upload
{
    /// Over the DECODED bytes, whatever `encoding` says the stored ones are.
    blob_hash hash;
    cc::string format;
    /// `raw` is the only encoding v1 writes; the field is the seam compression attaches to.
    cc::string encoding = "raw";
    /// Decoded size, which is what a reader allocates for.
    i64 size = 0;

    /// False means "you already have this", so nothing is read back just to be rewritten.
    /// False plus a hash naming no stored blob is a publish error, never a silently missing blob.
    bool has_data = true;

    /// The bytes AS STORED, i.e. after `encoding`.
    cc::vector<byte> data;
};
