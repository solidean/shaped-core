#include <clean-core/common/profiling.hh>
#include <versioned-document-file/impl/store_io.hh>
#include <versioned-document-file/store.hh>

/// The one snapshot writer, over a store_writer — used by persisting a snapshot and by pruning alike.
///
/// The snapshots land BEFORE any op is emptied, in ONE transaction.
/// The transaction already makes a half-done prune unobservable, so the order is not what protects a reader — it is
/// what keeps the code impossible to get wrong when someone later adds a step between the two.

namespace vdoc::file::impl
{
cc::result<snapshot_write_result> apply_snapshot_write(store_writer& writer, snapshot_write_job const& job)
{
    CC_RECORD_SCOPE("vdoc.file.snapshot_write");

    if (job.snapshots.empty() && job.skeletonized.empty())
        return snapshot_write_result{}; // nothing to write performs no I/O at all

    CC_RETURN_IF_ERROR(writer.begin());

    for (auto const& snapshot : job.snapshots)
        CC_RETURN_IF_ERROR(writer.upsert_snapshot(snapshot.row, snapshot.chunks));

    for (auto const& op_hash : job.skeletonized)
        CC_RETURN_IF_ERROR(writer.skeletonize_op(op_hash));

    CC_RETURN_IF_ERROR(writer.commit());
    return snapshot_write_result{.snapshots_written = job.snapshots.size(), .ops_skeletonized = job.skeletonized.size()};
}
} // namespace vdoc::file::impl
