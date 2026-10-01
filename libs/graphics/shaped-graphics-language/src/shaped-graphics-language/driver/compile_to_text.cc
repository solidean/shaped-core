#include "compile_to_text.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/resources.hh>
#include <shaped-graphics-language/driver/impl/describe_binding.hh>
#include <shaped-graphics-language/driver/impl/front_end.hh>
#include <shaped-graphics-language/emit/impl/dialect.hh>
#include <shaped-graphics-language/legalize/legalize.hh>
#include <shaped-graphics-language/test/run_tests.hh>

using namespace sgl;

namespace
{
/// Whether the code of `legal` reaches slot `member` of `binding`: a resource by its position, a block by -1.
/// Every plain member reaches its binding's block, and every member of an `@inline` binding reaches that one block.
bool is_reached(check::checked_module const& m, check::flat_entry_point const& legal, check::symbol_id binding, i32 member)
{
    auto const& info = m.bindings[m.at(binding).info];
    auto const members = m.at(info.members);
    for (auto const& x : legal.exprs)
    {
        auto const* const b = x.node.try_as<check::flat_binding_member>();
        if (b == nullptr || b->binding != binding)
            continue;
        auto const is_resource = !info.is_inline && m.takes_slots(members[b->member].type);
        if ((is_resource ? b->member : -1) == member)
            return true;
    }
    return false;
}

/// Every slot the entry point's text declares, as `sgl describe` numbers and names them.
cc::vector<interface_binding> interface_of(check::checked_module const& m,
                                           check::flat_entry_point const& legal,
                                           cc::span<emit::bound_name const> bound_names)
{
    auto const emitted_of = [&](cc::string_view host) -> cc::string
    {
        for (auto const& b : bound_names)
            if (b.host == host)
                return b.emitted;
        return cc::string(host);
    };

    auto result = cc::vector<interface_binding>();
    auto group = 0;
    for (auto const id : legal.bindings)
    {
        auto const& s = m.at(id);
        // workgroup memory takes no group, and no host binds it
        if (m.bindings[s.info].is_workgroup)
            continue;
        auto const is_inline = m.bindings[s.info].is_inline;
        auto const described = driver::impl::describe_binding(m, s);
        if (is_inline || described.block_slot == 0)
            result.push_back({.name = s.name,
                              .emitted = emitted_of(s.name),
                              .kind = described_member_kind::constant,
                              .is_inline = is_inline,
                              .group = is_inline ? -1 : group,
                              .slot = 0,
                              .is_used = is_reached(m, legal, id, -1),
                              .block_size = described.block_size});
        if (is_inline)
            continue;
        for (auto i = isize(0); i < described.members.size(); ++i)
        {
            auto const& member = described.members[i];
            if (member.kind == described_member_kind::constant)
                continue;
            result.push_back({.name = member.host_name,
                              .emitted = emitted_of(member.host_name),
                              .kind = member.kind,
                              .group = group,
                              .slot = member.slot,
                              .count = member.count,
                              .is_used = is_reached(m, legal, id, i32(i)),
                              .access = member.access,
                              .texture_dimension = member.texture_dimension,
                              .sample_type = member.sample_type,
                              .image_format = member.image_format,
                              .sampler_type = member.sampler_type});
        }
        ++group;
    }
    for (auto const id : driver::impl::file_samplers_of(legal))
    {
        auto const described = driver::impl::describe_file_sampler(m, id);
        result.push_back({.name = described.name,
                          .emitted = emitted_of(described.name),
                          .kind = described_member_kind::sampler,
                          .is_file_sampler = true,
                          .group = -1,
                          .slot = described.index,
                          .is_used = true,
                          .sampler_type = described.sampler_type});
    }
    return result;
}

/// The back half of `compile_to_text`: one entry point of a checked module, written for one target.
cc::result<emitted_source, cc::string> emit_text(check::checked_module const& m,
                                                 check::flat_entry_point const& e,
                                                 emit::target target,
                                                 cc::string_view source_name)
{
    // The check pass writes the structured form, and a target prints the core form.
    // WebGPU traces through the emulated form of a trace, and every other target through its native query.
    auto const legal = check::legalize(m, e, {.is_emulated = target == emit::target::wgsl});
    auto emitted = emit::emit_entry_point(m, legal, target);
    if (!emitted.has_text())
    {
        auto text = cc::string();
        for (auto const& error : emitted.errors)
            text.appendf("{}: error: {}: {}\n", source_name, emit::to_string(error.kind), error.detail);
        return cc::error(cc::move(text));
    }
    auto result = sgl::emitted_source{.text = cc::move(emitted.text),
                                      .entry_point = cc::move(emitted.entry_point),
                                      .bindings = interface_of(m, legal, emitted.bound_names),
                                      .color_targets = emitted.color_targets,
                                      .target_struct = cc::move(emitted.target_struct),
                                      .features = e.features,
                                      .options = driver::impl::option_names_of(m, e),
                                      .footprint = check::footprint_of(m, legal),
                                      .layouts = cc::move(emitted.layouts)};
    for (auto axis = 0; axis < 3; ++axis)
        result.workgroup[axis] = e.workgroup[axis];
    result.preferred_subgroup_size = e.preferred_subgroup_size;
    return result;
}
} // namespace

cc::result<sgl::emitted_source, cc::string> sgl::compile_to_text(text_request const& request)
{
    auto const front = driver::impl::run_front_end(request.source, request.source_name, request.options);
    if (!front.errors.empty())
        return cc::error(front.errors);
    auto const& m = front.module;

    if (request.run_tests)
    {
        // CHK-354: a test runs with every option at its default, so the compile's values need a check of their own
        auto const defaults
            = request.options.empty()
                ? cc::optional<driver::impl::front_end>()
                : cc::optional<driver::impl::front_end>(driver::impl::run_front_end(request.source, request.source_name));
        auto const& tested = defaults.has_value() ? defaults.value() : front;
        auto failed = cc::string();
        for (auto const& r :
             test::run_tests(tested.module, driver::impl::module_files_of(tested), {.file = tested.program_file()}))
            if (!r.is_passed())
                failed += driver::impl::format_located(tested, test::diagnostic_of(tested.module, r));
        if (!failed.empty())
            return cc::error(cc::move(failed));
    }

    auto index = isize(-1);
    for (auto i = isize(0); i < m.entry_points.size(); ++i)
        if (m.entry_points[i].name == request.entry_point)
            index = i;

    if (index < 0)
    {
        auto held = cc::string();
        for (auto const& e : m.entry_points)
            held.appendf("{}{} '{}'", held.empty() ? "" : ", ", check::stage_name(e.entry_stage), e.name);
        return cc::error(cc::format("{}: error: no entry point named '{}' (the source holds: {})\n", request.source_name,
                                    request.entry_point, held.empty() ? cc::string_view("none") : cc::string_view(held)));
    }

    auto const& e = m.entry_points[index];
    if (request.stage != check::stage::none && e.entry_stage != request.stage)
        return cc::error(cc::format("{}: error: entry point '{}' is a {} entry point, and a {} one was asked for\n",
                                    request.source_name, e.name, check::stage_name(e.entry_stage),
                                    check::stage_name(request.stage)));
    return emit_text(m, e, request.target, request.source_name);
}

cc::result<cc::vector<sgl::entry_text>, cc::string> sgl::compile_all_to_text(all_text_request const& request)
{
    auto const front = driver::impl::run_front_end(request.source, request.source_name);
    if (!front.errors.empty())
        return cc::error(front.errors);
    auto result = cc::vector<entry_text>();
    for (auto const& e : front.module.entry_points)
        for (auto const t : request.targets)
            result.push_back(
                {.entry_point = e.name, .target = t, .text = emit_text(front.module, e, t, request.source_name)});
    return result;
}
