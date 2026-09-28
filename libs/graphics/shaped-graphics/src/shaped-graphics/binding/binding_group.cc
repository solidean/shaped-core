#include <clean-core/common/assert.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/binding/binding_group_layout.hh>

namespace sg
{
binding_group::~binding_group() = default;

namespace
{
void add_uses(cc::vector<impl::buffer_use>& out, bound_view const& bound)
{
    for (auto const& view : bound.span())
        if (auto const* buffer = try_as_buffer_view(view); buffer != nullptr && buffer->buffer != nullptr)
            out.push_back({.buffer = buffer->buffer.get(), .writes = buffer->bound_as == view_class::readwrite});
}
} // namespace

cc::vector<impl::buffer_use> impl::buffer_uses_of(cc::span<named_view const> views)
{
    auto uses = cc::vector<buffer_use>();
    for (auto const& v : views)
        add_uses(uses, v.view);
    return uses;
}

cc::vector<impl::buffer_use> impl::buffer_uses_of(cc::span<slotted_view const> views)
{
    auto uses = cc::vector<buffer_use>();
    for (auto const& v : views)
        add_uses(uses, v.view);
    return uses;
}

void impl::set_buffer_uses(binding_group const& group, cc::vector<buffer_use> uses)
{
    group._buffer_uses = cc::move(uses);
}

cc::optional<cc::string> impl::find_write_aliasing(cc::span<binding_group const* const> groups,
                                                   cc::span<raw_buffer const* const> reads)
{
    // Runs at every dispatch and draw, so it walks the groups where they lie rather than gathering them.
    auto const read_elsewhere = [&](raw_buffer const* written)
    {
        for (auto const* group : groups)
            if (group != nullptr)
                for (auto const& use : group->buffer_uses())
                    if (use.buffer == written && !use.writes)
                        return true;
        for (auto const* read : reads)
            if (read == written)
                return true;
        return false;
    };
    for (auto const* group : groups)
        if (group != nullptr)
            for (auto const& use : group->buffer_uses())
                if (use.writes && read_elsewhere(use.buffer))
                    return cc::string("one buffer is bound for writing and for reading in one dispatch or draw, "
                                      "which WebGPU refuses whatever the ranges; copy it, or split the work");
    return {};
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
