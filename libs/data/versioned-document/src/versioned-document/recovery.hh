#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <versioned-document/op.hh>
#include <versioned-document/op_graph.hh>

/// Accepting history from a peer nobody trusts.
///
/// An op id recursively commits to everything behind it, so a replica missing history can take that history from
/// anyone at all: it recomputes the hashes and checks them against the ids it already expected.
/// If they match, the history is correct, and the sender was never trusted at any point.
///
/// The design is [the concept](../../docs/concept.md#recovery-from-an-untrusted-peer).

/// One op exactly as a peer sent it: the id it claims, its parents, and its two payload blobs.
///
/// The spans view the receive buffer and must outlive the call, which is what makes "verification is over the bytes as
/// received" literal rather than a promise — nothing is re-serialized on the way in.
///
/// **A skeleton cannot be offered.** An empty payload has nothing to verify, and it is refused as malformed rather
/// than quietly accepted as an op with no writes.
struct vdoc::received_op
{
    op_id id;
    cc::span<op_id const> parents;
    cc::span<byte const> metadata_bytes;
    cc::span<byte const> assignment_bytes;
};

/// Why a received batch was refused.
enum class vdoc::integration_error : vdoc::u8
{
    /// The bytes did not decode, or their recomputed hash is not the id the sender claimed.
    /// The accompanying op_decode_error says which, and `hash_mismatch` there means corruption or tampering.
    malformed,

    /// The op's parents are not the ones this replica already holds under that id.
    ///
    /// **Reachable only against a skeleton**, whose parents no hash has ever covered: a full op's parents are part of
    /// its preimage, so two ops that hash alike cannot disagree about them.
    /// That makes integration the one moment a corrupted skeleton row becomes detectable at all.
    parents_disagree,
};

/// Why a whole batch was refused, naming the op that caused it.
struct vdoc::integration_rejection
{
    /// The op the batch was refused at, always one the sender named.
    op_id op;

    integration_error reason;

    /// Meaningful only where `reason` is `malformed`.
    op_decode_error decode_error = op_decode_error::hash_mismatch;
};

/// What applying a verified batch did.
struct vdoc::integration_result
{
    /// Ops the graph did not have at all.
    isize ops_added = 0;

    /// Skeletons whose payload the batch put back.
    isize skeletons_filled = 0;

    /// Ops the graph already held in full, which the batch verified and then left alone.
    isize already_present = 0;
};

namespace vdoc
{
/// Decodes and checks a whole batch against `graph`, and changes nothing.
///
/// Verification is over the bytes as received: [try_decode_op](op.hh) re-hashes them, and a mismatch refuses the batch
/// naming the op it happened at.
/// Batch order does not matter, and a parent in neither the batch nor the graph is fine — the graph tolerates ops it
/// does not have, by design.
///
/// Split from applying so a caller with more to check — a store weighing a required snapshot — can refuse the batch
/// while "changed nothing" is still true for free, rather than having to undo anything.
[[nodiscard]] cc::result<cc::vector<op>, integration_rejection> try_verify_batch(op_graph const& graph,
                                                                                 cc::span<received_op const> batch);

/// Applies a verified batch.
/// **Infallible** — every fallible step already ran in try_verify_batch.
///
/// An op the graph holds as a skeleton is FILLED IN, which is the one thing `add` deliberately will not do.
/// Every op must have come from try_verify_batch against this same graph.
integration_result apply_verified_batch(op_graph& graph, cc::vector<op> verified);

/// Verify and apply, for a caller with nothing else to weigh.
///
/// **The batch is a set**: a partial or hostile one is refused naming the op, and leaves the graph exactly as it was.
[[nodiscard]] cc::result<integration_result, integration_rejection> integrate(op_graph& graph,
                                                                              cc::span<received_op const> batch);
} // namespace vdoc
