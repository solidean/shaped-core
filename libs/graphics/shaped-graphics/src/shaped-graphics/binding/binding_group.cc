#include <clean-core/common/assert.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/binding/binding_group_layout.hh>

namespace sg
{
binding_group::~binding_group() = default;

namespace
{
/// The use `bound` makes of a buffer, or none for a binding array or a binding that is not a buffer.
void add_use(cc::vector<impl::buffer_use>& out, cc::string_view binding, bound_view const& bound)
{
    // A binding array is exempt: WebGPU, the backend the check stands in for, has none.
    if (bound.size() != 1)
        return;
    if (auto const* buffer = try_as_buffer_view(bound.span()[0]); buffer != nullptr && buffer->buffer != nullptr)
        out.push_back({.buffer = buffer->buffer.get(),
                       .binding = cc::string(binding),
                       .writes = buffer->bound_as == view_class::readwrite});
}
} // namespace

void impl::record_buffer_uses(binding_group const& group, cc::span<named_view const> views)
{
    auto uses = cc::vector<buffer_use>();
    for (auto const& v : views)
        add_use(uses, v.name, v.view);
    set_buffer_uses(group, cc::move(uses));
}

void impl::record_buffer_uses(binding_group const& group,
                              binding_group_layout const& layout,
                              cc::span<slotted_view const> views)
{
    auto const bindings = layout.bindings();
    auto uses = cc::vector<buffer_use>();
    for (auto const& v : views)
        if (auto const slot = isize(u32(v.slot)); slot < bindings.size())
            add_use(uses, bindings[slot].name, v.view);
    set_buffer_uses(group, cc::move(uses));
}

void impl::set_buffer_uses(binding_group const& group, cc::vector<buffer_use> uses)
{
    group._buffer_uses = cc::move(uses);
}

cc::span<impl::buffer_use const> impl::buffer_uses_of(binding_group const& group)
{
    return group._buffer_uses;
}

void impl::drop_static_samplers(binding_group_layout const& layout, cc::vector<named_sampler>& samplers)
{
    auto const declared_static = layout.static_samplers();
    if (declared_static.empty())
        return;

    auto kept = cc::vector<named_sampler>();
    kept.reserve(samplers.size());
    for (auto& s : samplers)
    {
        auto is_static = false;
        for (auto const& d : declared_static)
            if (d.name == s.name)
                is_static = true;
        if (!is_static)
            kept.push_back(cc::move(s));
    }
    samplers = cc::move(kept);
}

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
