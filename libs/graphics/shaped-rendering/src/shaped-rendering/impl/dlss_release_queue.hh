#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-rendering/fwd.hh>

namespace sr::impl
{
/// NGX features parked until the GPU is done with them.
///
/// **Why a queue rather than a plain release.**
/// `sr::denoise_history::_prepare` drops a member's state whenever the method or either extent changed, and it runs
/// inside `sr::dlss_rr_routine::execute` while the caller is still recording a command list.
/// Releasing an NGX feature there frees device memory a frame in flight may still be reading, which is a use-after-free
/// with no diagnostic — see `dlss_release_feature`.
/// So the history's deleter parks the feature here against the epoch that was open, and a later call releases it once
/// the context reports that epoch complete.
///
/// Shared by `shared_ptr` with every deleter the routine installs, so a history that outlives the routine still has
/// somewhere to park rather than a dangling pointer to sweep into.
///
/// Thread-safe: `retire` runs wherever a history is dropped, `sweep` on whoever is recording.
class dlss_release_queue
{
public:
    dlss_release_queue() = default;
    dlss_release_queue(dlss_release_queue const&) = delete;
    dlss_release_queue& operator=(dlss_release_queue const&) = delete;

    /// Releases whatever is still parked, whether or not its epoch completed.
    ///
    /// The last resort, and it runs only once the routine and every history sharing this queue are gone.
    /// The device must still be alive, which is the same condition `sr::denoise_history`'s destructor already states.
    ~dlss_release_queue();

    /// Parks `feature` until `not_before` has completed; a null feature is ignored.
    void retire(void* feature, sg::epoch not_before);

    /// Releases everything whose epoch `ctx` now reports complete.
    /// Cheap on an empty queue, which is the normal case, so a caller sweeps every frame rather than deciding when to.
    void sweep(sg::context const& ctx);

private:
    struct entry
    {
        void* feature = nullptr;
        sg::epoch not_before = sg::epoch(0);
    };

    cc::mutex<cc::vector<entry>> _parked;
};
} // namespace sr::impl
