#include "describe.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/driver/impl/front_end.hh>
#include <shaped-graphics-language/emit/impl/plan.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

using namespace sgl;

namespace
{
namespace emit_impl = sgl::emit::impl;

described_binding describe_binding(check::checked_module const& m, check::symbol const& s)
{
    auto const& b = m.bindings[s.info];
    auto const members = m.at(b.members);
    auto result = described_binding{.name = s.name, .is_inline = b.is_inline};

    if (b.is_inline)
    {
        auto const placed = emit_impl::place_block(m, members);
        for (auto i = isize(0); i < members.size(); ++i)
            result.members.push_back({.name = members[i].name,
                                      .kind = described_member_kind::constant,
                                      .type = cc::string(m.name_of(members[i].type)),
                                      .offset = placed.offsets[i],
                                      .size = placed.sizes[i]});
        result.block_size = placed.size;
        return result;
    }

    // Numbered as the emitter numbers them: the constant block first when there is one, then the buffers in
    // declaration order, each the next slot of its group.
    auto const plain = emit_impl::plain_members_of(m, b);
    auto const placed = emit_impl::place_block(m, plain);
    if (!plain.empty())
    {
        result.block_size = placed.size;
        result.block_slot = 0;
        result.block_host_name = s.name;
    }
    auto slot = emit_impl::first_buffer_slot(m, b);
    auto next_constant = isize(0);
    for (auto const& member : members)
    {
        auto const& t = m.at(member.type);
        if (t.kind != check::type_kind::buffer)
        {
            result.members.push_back({.name = member.name,
                                      .kind = described_member_kind::constant,
                                      .type = cc::string(m.name_of(member.type)),
                                      .offset = placed.offsets[next_constant],
                                      .size = placed.sizes[next_constant]});
            ++next_constant;
            continue;
        }
        result.members.push_back({.name = member.name,
                                  .kind = described_member_kind::buffer,
                                  .type = cc::string(m.name_of(t.element)),
                                  .is_mut = t.is_mut,
                                  .slot = slot++,
                                  .host_name = cc::format("{}.{}", s.name, member.name)});
    }
    return result;
}

described_struct describe_struct(check::checked_module const& m, check::type_info const& t)
{
    auto result = described_struct{.name = m.at(t.symbol).name, .edge = t.edge};
    auto location = 0;
    for (auto const& member : m.at(t.members))
        result.members.push_back({.name = member.name,
                                  .type = cc::string(m.name_of(member.type)),
                                  .location = member.is_position ? -1 : location++,
                                  .stream = t.edge == check::stage::vertex ? emit_impl::stream_of(member) : cc::string(),
                                  .is_per_instance = member.is_per_instance});
    return result;
}

described_entry_point describe_entry_point(check::checked_module const& m, check::flat_entry_point const& e)
{
    auto result = described_entry_point{.name = e.name, .stage = e.entry_stage};
    for (auto axis = 0; axis < 3; ++axis)
        result.workgroup[axis] = e.workgroup[axis];
    for (auto const id : e.bindings)
        result.bindings.push_back(m.at(id).name);
    return result;
}
described_pipeline describe_pipeline(check::checked_module const& m, check::pipeline_info const& p)
{
    auto result = described_pipeline{.name = m.at(p.symbol).name, .vertex = m.at(p.vertex).name};
    if (check::is_valid(p.pixel))
        result.pixel = m.at(p.pixel).name;
    for (auto const b : m.at(p.layout))
        result.layout.push_back(m.at(b).name);
    if (check::is_valid(p.inline_constants))
        result.inline_constants = m.at(p.inline_constants).name;
    result.vertex_input = m.name_of(p.vertex_input);
    if (check::is_valid(p.target_set))
    {
        result.target_set = m.name_of(p.target_set);
        for (auto const& member : m.at(m.at(p.target_set).members))
            result.targets.push_back(member.name);
    }

    auto const settings = m.at(p.settings);
    for (auto const& s : settings)
        result.settings.push_back(
            {.path = s.path, .kind = s.kind, .integer = s.integer, .real = s.real, .enum_case = s.enum_case});

    // Open is where the last word is `.host`: a later setting of that field takes it back.
    for (auto i = isize(0); i < settings.size(); ++i)
    {
        auto is_last = true;
        for (auto j = i + 1; j < settings.size(); ++j)
            is_last = is_last && settings[j].path != settings[i].path;
        if (is_last && settings[i].kind == check::setting_kind::host)
            result.open.push_back(settings[i].path);
    }
    return result;
}
} // namespace

cc::result<sgl::module_description, cc::string> sgl::describe(describe_request const& request)
{
    auto const front = driver::impl::run_front_end(request.source, request.source_name);
    if (!front.errors.empty())
        return cc::error(front.errors);

    auto const& m = front.module;
    auto errors = cc::vector<emit::error>();
    auto result = module_description();

    // Only the program's own declarations: the prelude describes nothing, and an imported module describes itself.
    for (auto i = isize(0); i < m.symbols.size(); ++i)
    {
        auto const id = check::symbol_id(i);
        auto const& s = m.at(id);
        if (s.file != front.program_file() || s.state != check::symbol_state::checked)
            continue;

        if (s.kind == check::symbol_kind::binding)
        {
            auto const before = errors.size();
            emit_impl::validate_binding(m, id, errors);
            if (errors.size() == before)
                result.bindings.push_back(describe_binding(m, s));
        }
        else if (s.kind == check::symbol_kind::structure && check::is_valid(s.type))
        {
            auto const& t = m.at(s.type);
            if (t.edge == check::stage::none)
                continue;
            auto const role = t.edge == check::stage::vertex ? emit_impl::struct_role::vertex_input
                                                             : emit_impl::struct_role::render_targets;
            auto const before = errors.size();
            emit_impl::validate_edge_struct(m, s.type, role, errors);
            if (errors.size() == before)
                result.structs.push_back(describe_struct(m, t));
        }
    }

    // What only an entry point can get wrong: its list, its signature and its body.
    for (auto const& e : m.entry_points)
    {
        auto const before = errors.size();
        emit_impl::validate(m, check::legalize(m, e), errors);
        if (errors.size() == before)
            result.entry_points.push_back(describe_entry_point(m, e));
    }

    for (auto const& p : m.pipelines)
        if (m.at(p.symbol).file == front.program_file())
            result.pipelines.push_back(describe_pipeline(m, p));

    if (!errors.empty())
    {
        // A binding is judged on its own and again under every entry point that lists it, so its errors are said once.
        auto text = cc::string();
        for (auto i = isize(0); i < errors.size(); ++i)
        {
            auto const& error = errors[i];
            auto is_repeat = false;
            for (auto j = isize(0); j < i; ++j)
                is_repeat = is_repeat || errors[j] == error;
            if (!is_repeat)
                text.appendf("{}: error: {}: {}\n", request.source_name, emit::to_string(error.kind), error.detail);
        }
        return cc::error(cc::move(text));
    }
    return result;
}
