#pragma once

#include <clean-core/error/result.hh>
#include <shaped-graphics/fwd.hh>

/// How loaded the GPU is, which is the one resource cc cannot answer for.
///
/// CPU, memory, disk and network all come from documented OS calls, so they live in clean-core's platform layer.
/// A GPU does not: there is no portable "how busy is it" syscall anywhere, and even the memory figures come from the
/// graphics API rather than from the OS.
/// So sg owns these outright, and cc never mentions GPUs at all.
///
/// Both queries follow cc's conventions.
/// A load of 1 is the whole device busy, never one engine.
/// An unanswerable query returns an error rather than a zero that reads like an idle GPU.

/// GPU memory, as this process sees it.
struct sg::gpu_memory_usage
{
    /// What the OS says THIS process may use right now, which shrinks as other processes take memory.
    /// Not the card's size: see adapter_info::dedicated_video_memory_bytes for that.
    i64 budget_bytes = 0;

    /// What this process is currently using against that budget.
    i64 current_usage_bytes = 0;
};

/// How busy the GPU was, over whatever window the platform measures.
struct sg::gpu_load
{
    /// Busy fraction of the whole device, in [0, 1], following cc's load convention.
    f32 total = 0;
};
