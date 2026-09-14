#include "metal_sampler_cache.hh"

#include <clean-core/common/assert.hh>
#include <shaped-graphics/binding/impl/layout_hash.hh>

namespace sg::backend::metal
{
namespace
{
[[nodiscard]] MTL::SamplerMinMagFilter min_mag_filter_of(sg::sampler_filter f)
{
    return f == sg::sampler_filter::nearest ? MTL::SamplerMinMagFilterNearest : MTL::SamplerMinMagFilterLinear;
}

[[nodiscard]] MTL::SamplerMipFilter mip_filter_of(sg::sampler_filter f)
{
    return f == sg::sampler_filter::nearest ? MTL::SamplerMipFilterNearest : MTL::SamplerMipFilterLinear;
}

[[nodiscard]] MTL::SamplerAddressMode address_mode_of(sg::sampler_address_mode m)
{
    switch (m)
    {
    case sg::sampler_address_mode::repeat:
        return MTL::SamplerAddressModeRepeat;
    case sg::sampler_address_mode::mirror_repeat:
        return MTL::SamplerAddressModeMirrorRepeat;
    case sg::sampler_address_mode::clamp_edge:
        return MTL::SamplerAddressModeClampToEdge;
    case sg::sampler_address_mode::clamp_border:
        return MTL::SamplerAddressModeClampToBorderColor;
    }
    return MTL::SamplerAddressModeRepeat;
}

[[nodiscard]] MTL::CompareFunction compare_function_of(sg::compare_op op)
{
    switch (op)
    {
    case sg::compare_op::never:
        return MTL::CompareFunctionNever;
    case sg::compare_op::less:
        return MTL::CompareFunctionLess;
    case sg::compare_op::equal:
        return MTL::CompareFunctionEqual;
    case sg::compare_op::less_equal:
        return MTL::CompareFunctionLessEqual;
    case sg::compare_op::greater:
        return MTL::CompareFunctionGreater;
    case sg::compare_op::not_equal:
        return MTL::CompareFunctionNotEqual;
    case sg::compare_op::greater_equal:
        return MTL::CompareFunctionGreaterEqual;
    case sg::compare_op::always:
        return MTL::CompareFunctionAlways;
    }
    return MTL::CompareFunctionNever;
}

[[nodiscard]] MTL::SamplerBorderColor border_color_of(sg::sampler_border_color c)
{
    switch (c)
    {
    case sg::sampler_border_color::transparent_black:
        return MTL::SamplerBorderColorTransparentBlack;
    case sg::sampler_border_color::opaque_black:
        return MTL::SamplerBorderColorOpaqueBlack;
    case sg::sampler_border_color::opaque_white:
        return MTL::SamplerBorderColorOpaqueWhite;
    }
    return MTL::SamplerBorderColorTransparentBlack;
}
} // namespace

MTL::SamplerState* metal_sampler_cache::acquire(MTL::Device* device, sg::sampler const& s)
{
    auto const key = sg::impl::sampler_hash(s);

    return _states.lock(
        [&](cc::map<cc::hash128, MTL::SamplerState*>& states) -> MTL::SamplerState*
        {
            if (auto* const found = states.get_ptr(key); found != nullptr)
                return *found;

            auto const scope = autorelease_scope();

            auto* const descriptor = MTL::SamplerDescriptor::alloc()->init();
            descriptor->setMinFilter(min_mag_filter_of(s.min_filter));
            descriptor->setMagFilter(min_mag_filter_of(s.mag_filter));
            descriptor->setMipFilter(mip_filter_of(s.mip_filter));
            descriptor->setSAddressMode(address_mode_of(s.address_u));
            descriptor->setTAddressMode(address_mode_of(s.address_v));
            descriptor->setRAddressMode(address_mode_of(s.address_w));
            descriptor->setLodMinClamp(s.min_lod);
            descriptor->setLodMaxClamp(s.max_lod);
            descriptor->setMaxAnisotropy(NS::UInteger(s.max_anisotropy < 1 ? 1 : s.max_anisotropy));
            descriptor->setBorderColor(border_color_of(s.border_color));
            if (s.compare.has_value())
                descriptor->setCompareFunction(compare_function_of(s.compare.value()));

            // Required for a sampler an argument buffer names: without it the state has no GPU resource ID and cannot
            // be encoded into one at all.
            descriptor->setSupportArgumentBuffers(true);

            auto* const state = device->newSamplerState(descriptor);
            descriptor->release();
            CC_ASSERT(state != nullptr, "the metal device refused a sampler state");

            states[key] = state;
            return state;
        });
}

void metal_sampler_cache::shutdown()
{
    _states.lock(
        [](cc::map<cc::hash128, MTL::SamplerState*>& states)
        {
            for (auto&& [key, state] : states)
                state->release();
            states.clear();
        });
}
} // namespace sg::backend::metal
