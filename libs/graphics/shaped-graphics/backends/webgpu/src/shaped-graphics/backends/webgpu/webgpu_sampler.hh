#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/map.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/binding/sampler.hh>

/// WGPUSamplers for sg sampler states, one per distinct state for the context's life.
/// Keyed by sg::impl::sampler_hash, so two states a layout hash calls equal share one object.
class sg::backend::webgpu::webgpu_sampler_cache
{
public:
    void initialize(WGPUDevice device) { _device = device; }
    void shutdown() { _samplers.clear(); }

    /// The sampler for `s`, created on first use.
    [[nodiscard]] WGPUSampler acquire(sg::sampler const& s);

private:
    WGPUDevice _device = nullptr;
    cc::map<cc::hash128, wgpu_sampler> _samplers;
};
