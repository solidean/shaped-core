#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/container/set.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/barrier/footprint.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group_layout.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>

sg::slot_footprint const* sg::shader_footprint::find(cc::string_view name) const
{
    for (auto const& s : slots)
        if (s.name == name)
            return &s;
    return nullptr;
}

sg::shader_footprint sg::reflected_footprint(cc::span<binding const> bindings)
{
    auto fp = shader_footprint{.source = footprint_source::reflected, .slots = {}};
    for (auto const& b : bindings)
    {
        auto access = access_flags();
        switch (b.type)
        {
        case binding_type::constants_buffer:
            access = access_flag::constants_read;
            break;
        case binding_type::texture:
            access = access_flag::shader_read;
            break;
        case binding_type::acceleration_structure:
            access = access_flag::accel_read;
            break;
        case binding_type::sampler:
            continue; // nothing orders against a sampler
        case binding_type::buffer:
        case binding_type::bytes:
        case binding_type::image:
            if (view_class_of(b) == view_class::readonly)
                access = access_flag::shader_read;
            else if (b.access == access_mode::write)
                access = access_flag::shader_write;
            else if (b.access == access_mode::read)
                access = access_flag::storage_read;
            else
                access = access_flag::storage_read | access_flag::shader_write;
            break;
        }
        fp.slots.push_back({.name = b.name, .access = access, .dynamic_index = b.is_array()});
    }
    return fp;
}

bool sg::shader_footprint::operator==(shader_footprint const& rhs) const
{
    if (source != rhs.source || slots.size() != rhs.slots.size())
        return false;
    for (isize i = 0; i < slots.size(); ++i)
        if (!(slots[i] == rhs.slots[i]))
            return false;
    return true;
}

sg::pipeline_stage_flags sg::impl::stages_of(shader_stage stage)
{
    switch (stage)
    {
    case shader_stage::vertex:
    case shader_stage::tessellation_control:
    case shader_stage::tessellation_evaluation:
    case shader_stage::geometry:
        return pipeline_stage_flag::vertex;
    case shader_stage::fragment:
        return pipeline_stage_flag::fragment;
    case shader_stage::compute:
        return pipeline_stage_flag::compute;
    case shader_stage::raygen:
    case shader_stage::closest_hit:
    case shader_stage::any_hit:
    case shader_stage::miss:
    case shader_stage::intersection:
    case shader_stage::callable:
        return pipeline_stage_flag::raytracing;
    }
    return pipeline_stage_flag::compute; // unreachable for the closed set above
}

sg::impl::pipeline_footprint sg::impl::pipeline_footprint::resolve(pipeline_layout const& layout,
                                                                   cc::span<stage_input const> stages)
{
    auto fp = pipeline_footprint();
    for (auto const& s : stages)
        if (s.footprint == nullptr || !s.footprint->is_known())
            return fp;

    // A footprint naming a binding the layout does not hold is keyed differently from it, not narrower than it.
    // Reading its absences as "untouched" would then drop real barriers, so it is not used at all.
    for (auto const& s : stages)
        for (auto const& slot : s.footprint->slots)
        {
            auto found = false;
            for (auto const& group : layout.groups())
                if (group != nullptr)
                    for (auto const& b : group->bindings())
                        found |= b.name == slot.name;
            if (!found)
                return fp;
        }

    fp._known = true;
    for (auto const& group : layout.groups())
    {
        auto& uses = fp._groups.emplace_back();
        if (group == nullptr)
            continue;
        for (auto const& b : group->bindings())
        {
            auto use = slot_use();
            for (auto const& s : stages)
                if (auto const* slot = s.footprint->find(b.name); slot != nullptr && !slot->access.is_empty())
                {
                    use.access |= slot->access;
                    use.stages |= s.stages;
                    use.dynamic_index |= slot->dynamic_index;
                }
            uses.slots.push_back(use);
            uses.names.push_back(b.name);
        }
    }
    return fp;
}

sg::impl::slot_use sg::impl::pipeline_footprint::use_of(int group, isize binding) const
{
    CC_ASSERT(_known, "an unknown footprint has no uses to look up");
    if (group < 0 || group >= int(_groups.size()) || binding < 0 || binding >= _groups[group].slots.size())
        return {};
    return _groups[group].slots[binding];
}

sg::impl::slot_use sg::impl::pipeline_footprint::use_of(int group, cc::string_view name) const
{
    CC_ASSERT(_known, "an unknown footprint has no uses to look up");
    if (group < 0 || group >= int(_groups.size()))
        return {};
    auto const& g = _groups[group];
    for (isize i = 0; i < g.names.size(); ++i)
        if (g.names[i] == name)
            return g.slots[i];
    return {};
}

void sg::impl::log_footprint_mismatch_once(void const* pipeline, cc::string_view binding, cc::string_view message)
{
    // Keyed by address: a pipeline freed and another built at its address inherits its silence, which costs one log line.
    static auto logged = cc::mutex<cc::set<cc::string>>();
    auto const key = cc::format("{}:{}", reinterpret_cast<u64>(pipeline), binding);
    if (logged.lock([&](cc::set<cc::string>& s) { return s.insert(key); }))
        CC_LOG_ERROR("'{}': {}", binding, message);
}
