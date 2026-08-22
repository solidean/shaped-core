#include <clean-core/common/profiling.hh>
#include <versioned-document-file/impl/store_io.hh>
#include <versioned-document-file/store.hh>

/// The one reclaimer, over a store_writer.
///
/// Assets go first and blobs second, in ONE transaction, so a file is never left holding an asset whose blob was
/// already collected.

namespace vdoc::file::impl
{
cc::result<reclaim_result> apply_reclaim(store_writer& writer, reclaim_job const& job)
{
    CC_RECORD_SCOPE("vdoc.file.reclaim");

    if (job.removed_assets.empty() && job.removed_blobs.empty())
        return reclaim_result{}; // nothing to collect performs no I/O at all

    CC_RETURN_IF_ERROR(writer.begin());

    for (auto const& asset_id : job.removed_assets)
        CC_RETURN_IF_ERROR(writer.delete_asset(asset_id));

    // The chunks follow by cascade rather than by a second loop here, which is why foreign_keys must be on.
    for (auto const& hash : job.removed_blobs)
        CC_RETURN_IF_ERROR(writer.delete_blob(hash));

    CC_RETURN_IF_ERROR(writer.commit());
    return reclaim_result{.assets_removed = job.removed_assets.size(), .blobs_removed = job.removed_blobs.size()};
}
} // namespace vdoc::file::impl
