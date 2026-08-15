#pragma once

#include <clean-core/common/hash256.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <versioned-document/ids.hh>
#include <versioned-document/value.hh>

#include <compare>

/// The op: an immutable, content-addressed set of property assignments plus the parent links that place it in the DAG.
///
/// **An op holds the producer's bytes and decodes on demand.**
/// metadata() and assignments() are views over those bytes; nothing decoded is stored beside them.
/// That is what makes "no load path ever re-serializes" structural: no encoder is reachable from a loaded op, so no
/// future change to one can turn a good stored op into a hash mismatch — and a mismatch is indistinguishable from
/// tampering.
///
/// try_decode_op is the only route from bytes to an op, and it verifies as it goes.
///
/// The design is [the concept](../../docs/concept.md#ops-and-content-addressing), and the settled choices are
/// [decisions.md](../../docs/decisions.md#the-op-retains-its-bytes-and-decodes-on-demand).

/// Content hash of an op: a 32-byte BLAKE3 digest that recursively commits to the whole history behind it.
///
/// **Ordering is by the canonical 32 bytes**, never by the underlying hash256's defaulted <=>, which orders four u64
/// limbs assembled little-endian and is a different order entirely.
/// The parent sort feeds the hash preimage, so an implementation that sorted by limb would compute a different op_id
/// for identical content — an interop break no later version could fix.
struct vdoc::op_id
{
    /// The 32 canonical bytes an op id is, as stored and as hashed.
    static constexpr isize byte_size = 32;

    cc::hash256 digest;

    /// The all-zero id, which no real op has: it is what an absent or not-yet-known parent reads as.
    constexpr op_id() = default;
    explicit constexpr op_id(cc::hash256 d) : digest(d) {}

    [[nodiscard]] friend constexpr bool operator==(op_id const&, op_id const&) = default;

    /// Writes the canonical form; `out` must be exactly byte_size bytes.
    void to_bytes(cc::span<byte> out) const { digest.to_bytes(out); }

    /// Reads the canonical form; `data` must be exactly byte_size bytes.
    [[nodiscard]] static op_id from_bytes(cc::span<byte const> data) { return op_id(cc::hash256::from_bytes(data)); }

    /// Orders by the canonical 32 bytes, which is the order the hex digests sort in.
    [[nodiscard]] std::strong_ordering compare_bytes(op_id const& rhs) const;

    /// Sort predicate over the canonical bytes — what the builder sorts parents with.
    struct by_bytes
    {
        [[nodiscard]] bool operator()(op_id const& lhs, op_id const& rhs) const { return lhs.compare_bytes(rhs) < 0; }
    };

    [[nodiscard]] friend u64 hash(op_id const& v) { return hash(v.digest); }
};

/// Why bytes read from a store would not become an op.
/// String-free by design, so a caller localizes it and a test asserts on it exactly.
enum class vdoc::op_decode_error : cc::u8
{
    /// A buffer ended in the middle of a field it had declared.
    truncated,
    /// Parents are not in ascending byte order.
    unsorted_parents,
    /// The same parent id appears twice.
    duplicate_parent,
    /// The assignment blob's leading encoding tag is not one this build knows.
    unknown_assignment_encoding,
    /// An assignment's value, or the metadata, is not a canonically encoded value.
    invalid_value,
    /// Assignments are not in ascending (entity, component, property) order.
    unsorted_assignments,
    /// The same property path is assigned twice within one op.
    duplicate_assignment,
    /// Bytes remained after the last assignment.
    trailing_bytes,
    /// The recomputed hash is not the id the op was stored under.
    hash_mismatch,
};

/// The result of checking an op against its own stored bytes.
enum class vdoc::op_verification : cc::u8
{
    /// The recomputed hash matches the id.
    verified,
    /// A skeleton op left behind by pruning: there are no bytes to hash.
    /// **Never a mismatch.** Reporting a routinely-pruned op as tampering would be a false alarm about the one thing
    /// this whole scheme exists to detect.
    unverifiable,
    /// The recomputed hash is not the id, which means corruption or tampering.
    mismatch,
};

/// The exact bytes a store persists for an op, and the only thing op_id commits to.
///
/// Held verbatim as the producer canonicalized them.
/// The two blobs are present together or absent together — absent is a skeleton, and there is no op with metadata but
/// no assignments.
struct vdoc::op_payload
{
    /// Any canonically encoded value — op_builder writes an object, and a decoder accepts whatever kind it is handed.
    /// Informational, and hashed, but nothing interprets it.
    cc::vector<byte> metadata_bytes;

    /// A one-byte encoding tag, then the assignments in that encoding.
    cc::vector<byte> assignment_bytes;
};

/// One property assignment, as a cursor's current position.
/// The value is a view into the op's own bytes, so it outlives nothing the op does not.
struct vdoc::assignment
{
    property_path path;
    value_view value;
};

/// The assignment encodings this build knows.
/// A FORMAT CONSTANT: the tag lets the encoding change without touching the hashing rule, so these numbers are pinned.
enum class vdoc::assignment_encoding : vdoc::u8
{
    /// u32 count, then per assignment the three ids as u32 length plus bytes, then the encoded value.
    sorted_v1 = 1,
};

/// Walks an op's assignment blob, decoding each entry in place.
///
/// The blob has already passed try_decode_op, so nothing here re-validates and every accessor is total.
/// This is what materialization iterates, so an op never builds a per-op assignment vector.
class vdoc::assignment_cursor
{
public:
    constexpr assignment_cursor() = default;

    /// The blob must already have been validated by try_decode_op.
    [[nodiscard]] static assignment_cursor from_validated_bytes(cc::span<byte const> assignment_bytes);

    [[nodiscard]] bool at_end() const { return _remaining == 0; }

    /// The assignment at the cursor.
    /// Only valid while !at_end().
    [[nodiscard]] assignment get() const;

    /// Advances past the current assignment.
    /// Only valid while !at_end().
    void advance();

    /// How many assignments are left, including the current one.
    [[nodiscard]] isize remaining() const { return _remaining; }

    // range-for support: the cursor is its own iterator, so `for (auto const a : op.assignments())` reads naturally
public:
    struct sentinel
    {
    };

    [[nodiscard]] assignment operator*() const { return get(); }
    assignment_cursor& operator++()
    {
        advance();
        return *this;
    }
    [[nodiscard]] friend bool operator==(assignment_cursor const& c, sentinel) { return c.at_end(); }
    [[nodiscard]] friend bool operator!=(assignment_cursor const& c, sentinel) { return !c.at_end(); }

    [[nodiscard]] assignment_cursor begin() const { return *this; }
    [[nodiscard]] sentinel end() const { return {}; }

private:
    cc::span<byte const> _bytes;
    isize _cursor = 0;
    isize _remaining = 0;
};

/// An immutable, content-addressed op.
///
/// Constructed only by try_decode_op or by op_builder, so an op in hand has always been verified against its own bytes.
struct vdoc::op
{
    /// The content hash of everything below.
    op_id id;

    /// Verbatim, in the op's own order — which the builder canonicalizes to ascending byte order, deduplicated.
    /// Zero parents starts a document, one extends it, several merge.
    cc::vector<op_id> parents;

    /// Absent only on a skeleton op left behind by pruning.
    cc::optional<op_payload> payload;

    [[nodiscard]] bool is_skeleton() const { return !payload.has_value(); }

    /// The op's metadata, or the null value on a skeleton.
    [[nodiscard]] value_view metadata() const;

    /// Walks the op's assignments without materializing them.
    /// A skeleton yields an empty cursor, since its content is gone rather than empty — callers that care ask
    /// is_skeleton() rather than inferring it from a zero count.
    [[nodiscard]] assignment_cursor assignments() const;

    /// The whole assignment list at once, for tests and callers that want to hold it.
    /// Fallible because the blob may use an encoding this build does not know, which decoding lazily cannot report.
    [[nodiscard]] cc::result<cc::vector<assignment>, op_decode_error> try_decode_assignments() const;
};

namespace vdoc
{
/// Recomputes the op's hash from its retained bytes and compares it against its id.
///
/// **Re-hashes; never re-serializes.** A skeleton reports `unverifiable`, never `mismatch`.
[[nodiscard]] op_verification verify_op(op const& o);

/// The hash an op with these parts would have.
/// The one definition of the preimage, used by the builder to stamp an id and by verification to check one.
[[nodiscard]] op_id compute_op_id(cc::span<op_id const> parents,
                                  cc::span<byte const> metadata_bytes,
                                  cc::span<byte const> assignment_bytes);

/// The only route from stored bytes to an op, and it verifies as it goes.
///
/// `expected_id` is the id the store had it under; the recomputed hash must equal it.
/// Making this the only route is what stops a future loader forgetting to check.
[[nodiscard]] cc::result<op, op_decode_error> try_decode_op(op_id const& expected_id,
                                                            cc::span<op_id const> parents,
                                                            cc::span<byte const> metadata_bytes,
                                                            cc::span<byte const> assignment_bytes);

/// The skeleton form: an id and its parents, with no bytes to hash.
/// Verification reports `unverifiable` on the result, which is the whole point of the type.
[[nodiscard]] cc::result<op, op_decode_error> try_decode_skeleton_op(op_id const& id, cc::span<op_id const> parents);

/// Encodes assignments into the `sorted_v1` blob.
///
/// **Producer-side only.** Nothing on a load path may call this — see the type comment above for why.
/// The assignments must already be sorted by path and free of duplicates; this asserts rather than checks, because
/// op_builder is its only caller and a violation there is a bug rather than bad input.
[[nodiscard]] cc::vector<byte> encode_assignments(cc::span<assignment const> sorted_assignments);
} // namespace vdoc
