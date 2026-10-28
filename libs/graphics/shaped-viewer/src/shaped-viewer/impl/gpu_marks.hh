#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/query/gpu_timestamp.hh>

// TEMPORARY(debt-sv-gpu-stage-marks): GPU scopes recorded into cc::rec by sg are what replaces these.

namespace sv::impl
{
/// Named GPU timestamps at the stage boundaries of a frame, in the order the stages ended.
struct gpu_marks
{
    struct mark
    {
        cc::string_view stage;
        sg::gpu_timestamp at;
    };
    cc::vector<mark> marks;
};

/// The collector `gpu_mark` records into on this thread, or null for none, which is every frame but a benchmark's.
inline thread_local gpu_marks* active_gpu_marks = nullptr;

/// Records that `stage` ended here in `cmd`, where a collector is set; `stage` must outlive the collector.
inline void gpu_mark(sg::command_list& cmd, cc::string_view stage)
{
    if (active_gpu_marks != nullptr)
        active_gpu_marks->marks.push_back({.stage = stage, .at = cmd.query.record_gpu_timestamp()});
}
} // namespace sv::impl
