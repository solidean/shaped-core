#include <versioned-document-file/impl/store_io.hh>
#include <versioned-document-file/store.hh>

/// The one recovery writer, over a store_writer — shared by both store implementations.
///
/// The ops land BEFORE any snapshot is demoted, in ONE transaction.
/// The transaction already makes a half-done recovery unobservable, so the order is not what protects a reader — it is
/// what keeps the failure that survives a crash the harmless one: a snapshot still marked required over history that
/// is already back costs a pinned cache entry, while the reverse leaves a droppable snapshot over history that is gone.

namespace vdoc::file::impl
{
namespace
{
[[nodiscard]] recovery_result outcome_of(recovery_job const& job)
{
    return recovery_result{.ops_added = job.ops_added,
                           .skeletons_filled = job.skeletons_filled,
                           .snapshots_demoted = job.demoted_snapshots.size()};
}
} // namespace

cc::result<recovery_result> apply_recovery(store_writer& writer, recovery_job const& job)
{
    if (job.ops.empty() && job.demoted_snapshots.empty())
        return outcome_of(job); // nothing to write performs no I/O at all

    CC_RETURN_IF_ERROR(writer.begin());

    // Insert AND fill, for every op.
    // The insert conflicts away where the row is already there, and the fill only touches NULL payload columns, so
    // neither has to know which case it is in.
    for (auto const& row : job.ops)
    {
        CC_RETURN_IF_ERROR(writer.insert_op(row));
        CC_RETURN_IF_ERROR(writer.fill_op_payload(row));
    }

    for (auto const& op_hash : job.demoted_snapshots)
        CC_RETURN_IF_ERROR(writer.set_snapshot_required(op_hash, /*required =*/false));

    CC_RETURN_IF_ERROR(writer.commit());
    return outcome_of(job);
}
} // namespace vdoc::file::impl
