#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/map.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-graphics/fwd.hh>

/// MTLSamplerStates for bound sampler values, keyed by the sampler's own identity and shared context-wide.
///
/// **Per context rather than per group**, and the reason is lifetime.
/// A dx12 sampler descriptor leaves no object behind; Metal needs an MTLSamplerState that outlives every argument
/// buffer naming it, and giving each group its own would mean deferring its destruction behind that group's epoch.
/// Caching them here makes the lifetime trivial and a re-minted group free.
///
/// Keyed on `sg::impl::sampler_hash` — sg's own identity for the value — rather than on one the backend invents, so
/// the cache answers the same question the layout identity does.
class sg::backend::metal::metal_sampler_cache
{
public:
    /// The state for `s`, minted on first use.
    /// Null only if the device refuses one, which is a device failure rather than a bad sampler.
    [[nodiscard]] MTL::SamplerState* acquire(MTL::Device* device, sg::sampler const& s);

    /// Releases every cached state; the cache is empty afterwards.
    void shutdown();

private:
    cc::mutex<cc::map<cc::hash128, MTL::SamplerState*>> _states;
};
