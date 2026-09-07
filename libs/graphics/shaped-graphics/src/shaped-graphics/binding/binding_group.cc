#include <clean-core/common/assert.hh>
#include <shaped-graphics/binding/binding_group.hh>

namespace sg
{
binding_group::~binding_group() = default;

cc::vector<named_sampler> impl::merge_declared_samplers(cc::span<named_sampler const> declared,
                                                        cc::span<named_sampler const> supplied)
{
    cc::vector<named_sampler> merged;
    merged.reserve(declared.size() + supplied.size());
    for (auto const& d : declared)
        merged.push_back(d);

    // Declared first, then only what the shader did not declare, so the shader wins in every build and the
    // assertion is what names the mistake in a checked one.
    for (auto const& s : supplied)
    {
        bool already_declared = false;
        for (auto const& d : declared)
            already_declared = already_declared || d.name == s.name;

        CC_ASSERT(!already_declared, "the shader already declared this sampler static");
        if (!already_declared)
            merged.push_back(s);
    }

    return merged;
}
} // namespace sg
