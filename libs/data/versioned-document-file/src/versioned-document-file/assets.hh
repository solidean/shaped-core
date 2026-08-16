#pragma once

#include <clean-core/common/hash256.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <versioned-document-file/fwd.hh>
#include <versioned-document/value.hh>

#include <compare>

/// The asset vocabulary: a name pointing at an ordered list of parts, each part pointing at a blob.
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

/// The name a single-part asset's one part carries, and what `main_part()` looks for.
///
/// It is the DEFAULT of asset_part::name, so the common case costs no ceremony: an asset published as
/// `{.hash = h, .format = "png"}` is reached through main_part().
inline constexpr cc::string_view main_part_name = "$main";

/// One part of an asset.
///
/// **The name is the contract, and order disambiguates within a name.**
/// A part is addressed by `(name, index)`, where the index is its position among the parts sharing that name — so a
/// LOD chain is `("lod", 0..n)` and a lone mesh is just `$main`.
/// Nothing keys behaviour on a part's position in the whole list: reordering an asset's parts changes nothing, and
/// renaming one is the format change it looks like.
///
/// The rule used to be the other way round, and the argument for reversing it is
/// [decisions.md](../../../versioned-document/docs/decisions.md#part-names-are-the-contract-and-position-within-a-name-disambiguates).
struct vdoc::file::asset_part
{
    blob_hash hash;
    /// What the bytes are, e.g. "png" — selects a parser downstream.
    cc::string format;

    /// How this part is addressed, defaulting to `$main`.
    ///
    /// `$` is RESERVED: an application must not invent its own $-name, so `$preview` and friends stay available.
    /// An empty name is reported at load — it is only reachable by asking for it, since the default is not empty.
    cc::string name = cc::string(main_part_name);

    [[nodiscard]] friend bool operator==(asset_part const& a, asset_part const& b) = default;
};

/// Why a part lookup did not produce exactly one part.
///
/// Two distinct bugs, kept distinct: an optional preview that is absent is ordinary, while three parts named `$main`
/// is a broken publish.
/// A caller localizes the message and a test asserts on the kind, neither of which survives a formatted string.
enum class vdoc::file::part_lookup_error : vdoc::file::u8
{
    not_found,
    ambiguous,
};

/// The parts of one asset sharing a name, in the order the asset declares them.
///
/// **Materialized, and it borrows.** It holds pointers into the record it was made from, so that record must outlive
/// it — which is what `resolve_asset`'s snapshot copy is for.
class vdoc::file::part_range
{
public:
    struct iterator
    {
        asset_part const* const* at = nullptr;

        [[nodiscard]] asset_part const& operator*() const { return **at; }
        iterator& operator++()
        {
            ++at;
            return *this;
        }
        [[nodiscard]] friend bool operator==(iterator const&, iterator const&) = default;
    };

    part_range() = default;
    explicit part_range(cc::vector<asset_part const*> parts) : _parts(cc::move(parts)) {}

    [[nodiscard]] isize size() const { return _parts.size(); }
    [[nodiscard]] bool empty() const { return _parts.empty(); }

    /// `index` must be in [0, size).
    [[nodiscard]] asset_part const& operator[](isize index) const { return *_parts[index]; }

    [[nodiscard]] iterator begin() const { return {_parts.begin()}; }
    [[nodiscard]] iterator end() const { return {_parts.end()}; }

private:
    cc::vector<asset_part const*> _parts;
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
    /// Addressed by (name, index) — see asset_part.
    /// Stored order is preserved verbatim, because it is what disambiguates parts sharing a name.
    cc::vector<asset_part> parts;
    /// Informational; nothing keys behaviour on it.
    vdoc::value meta;

    /// Asset ids this asset needs, DECLARED by the application and never interpreted here.
    ///
    /// The store never parses a blob, so it cannot discover a dependency on its own — which is why this is declared
    /// rather than derived, and why an application that under-declares gets a sweep that collects too much.
    /// An id naming nothing in this file is legal and silent: a file is one asset source among many, so a dependency
    /// may name a built-in, procedural, remote or otherwise externally-stored asset.
    /// Order carries no meaning, duplicates are harmless, and cycles are ordinary.
    cc::vector<cc::string> dependencies;

    /// False once a load found a part whose blob is missing or incomplete.
    /// Set by the load and never by a caller, and publishing ignores it.
    bool is_resolvable = true;

    // addressing parts
public:
    /// The one part named `$main`.
    ///
    /// **Errors rather than picking one** where there are several: an application that expected a single-part asset
    /// and silently got the first of three has a bug it cannot see, and plausible-looking wrong bytes are the worst
    /// outcome this API can produce.
    [[nodiscard]] cc::result<asset_part const*, part_lookup_error> main_part() const;

    /// The one part named `name`, erroring where there is none or more than one.
    [[nodiscard]] cc::result<asset_part const*, part_lookup_error> try_find_part(cc::string_view name) const;

    /// The `index`-th part named `name`, or empty where there is no such part.
    ///
    /// Optional rather than a result, because naming an index already says "I know there may be several" — so running
    /// off the end is an ordinary absent answer rather than a mistake.
    [[nodiscard]] cc::optional<asset_part const*> part_at(cc::string_view name, isize index) const;

    /// Every part named `name`, in declaration order.
    [[nodiscard]] part_range parts_named(cc::string_view name) const;

    /// Every part named `$main` — what a caller reaches for after main_part() reported `ambiguous`.
    [[nodiscard]] part_range main_parts() const { return parts_named(main_part_name); }
};

/// A blob offered for storage, in whatever encoding it is stored as.
struct vdoc::file::blob_upload
{
    /// Over the DECODED bytes, whatever `encoding` says the stored ones are.
    blob_hash hash;
    cc::string format;
    /// `raw` is the only encoding v1 writes; the field is the seam compression attaches to.
    cc::string encoding = "raw";

    /// What a reader allocates for.
    ///
    /// REQUIRED when `encoding` is not `raw`: the file records the stored size implicitly, and nothing else can
    /// recover the decoded one.
    /// Zero under `raw` reads as `data.size()`.
    i64 decoded_size = 0;

    /// False means "you already have this", so nothing is read back just to be rewritten.
    /// False plus a hash naming no stored blob is a publish error, never a silently missing blob.
    bool has_data = true;

    /// The bytes AS STORED, i.e. after `encoding`.
    cc::vector<byte> data;

    /// Hashes the decoded bytes and encodes them for storage.
    ///
    /// The one route that makes the hash, the encoding and the decoded size agree by CONSTRUCTION rather than by a
    /// caller's promise — a caller with bytes already encoded still fills the fields itself.
    /// Fails where this build has no codec for `encoding`.
    [[nodiscard]] static cc::result<blob_upload> of(cc::span<byte const> decoded,
                                                    cc::string_view format,
                                                    cc::string_view encoding = "raw");
};
