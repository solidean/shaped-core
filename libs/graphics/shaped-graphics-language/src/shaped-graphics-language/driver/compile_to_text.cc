#include "compile_to_text.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/reserved_bindings.hh>
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
                              .emitted = emitted_of(m.qualified_name_of(id)),
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
                              .emitted = emitted_of(cc::format("{}.{}", m.qualified_name_of(id), member.name)),
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

/// `level`'s name as `@float` spells it.
cc::string_view float_level_name(check::float_level level)
{
    switch (level)
    {
    case check::float_level::fast:
        return "fast";
    case check::float_level::specials:
        return "specials";
    case check::float_level::precise:
        return "precise";
    case check::float_level::ieee:
        return "ieee";
    }
    return "fast";
}

/// EMIT-185: the strictest float guarantee `target`'s compile keeps today.
check::float_level kept_float_level(emit::target target)
{
    switch (target)
    {
    case emit::target::hlsl_dx12:
        return check::float_level::ieee;
    // safe math keeps the specials and the order; whether it keeps denormals is not known
    case emit::target::msl:
        return check::float_level::precise;
    // DXC's -Gis writes SignedZeroInfNanPreserve and NoContraction on every float operation, and -denorm preserve
    // writes DenormPreserve, each behind the device property it needs
    case emit::target::hlsl_vulkan:
        return check::float_level::ieee;
    // nothing a shader says reaches Tint's or Naga's float handling
    case emit::target::wgsl:
        return check::float_level::fast;
    }
    return check::float_level::fast;
}

/// The back half of `compile_to_text`: one entry point of a checked module, written for one target.
cc::result<emitted_source, cc::string> emit_text(driver::impl::front_end const& front,
                                                 check::flat_entry_point const& e,
                                                 emit::target target,
                                                 bool line_directives,
                                                 bool diagnostics,
                                                 check::undefined_guard guards_undefined,
                                                 bool asks_software_trace,
                                                 bool weakens_float,
                                                 check::feature_set device_features,
                                                 emit::native_route native = emit::native_route::text,
                                                 bool native_line_table = false)
{
    auto const& m = front.module;
    auto const source_name = front.source_name;
    // EMIT-185: a guarantee the target cannot keep is refused, unless the host weakened it on WebGPU
    auto const raised_by = check::is_valid(e.float_raised_by) ? m.at(e.float_raised_by).name : cc::string();
    auto float_guarantee = e.float_guarantee;
    if (u8(float_guarantee) > u8(kept_float_level(target)))
    {
        if (target != emit::target::wgsl || !weakens_float)
            return cc::error(cc::format("{}: error: float-guarantee-not-kept: entry point '{}' needs @float(.{}), "
                                        "which "
                                        "'{}' asks for, and {} keeps no more than .{}\n",
                                        source_name, e.name, float_level_name(float_guarantee), raised_by,
                                        emit::to_string(target), float_level_name(kept_float_level(target))));
        float_guarantee = check::float_level::fast;
    }
    // EMIT-185: vulkan keeps a level through execution modes the device must have
    auto entry = e;
    if (target == emit::target::hlsl_vulkan && float_guarantee != check::float_level::fast)
        entry.features.set(check::feature::float_specials);
    if (target == emit::target::hlsl_vulkan && float_guarantee == check::float_level::ieee)
        entry.features.set(check::feature::float_denormals);
    // The check pass writes the structured form, and a target prints the core form.
    // WebGPU traces through the emulated form of a trace, and every other target through its native query, unless the
    // compile asks for software on HLSL (EMIT-135).
    auto const software_trace = asks_software_trace;
    // LEGAL-63, LEGAL-64: without diagnostics the text holds no print and no assert, and with them both write a record
    auto const files = driver::impl::module_files_of(front);
    auto names = cc::vector<cc::string_view>();
    for (auto i = isize(0); i < files.size(); ++i)
        names.push_back(front.name_of(i));
    // EMIT-183: a stage that cannot write the window ends a failed assert all the same, and drops what it would log
    auto const writes_records = diagnostics
                             && check::reaches_reserved(*check::reserved_binding_named("sg_debug"), target,
                                                        entry.entry_stage, device_features);
    auto const legal = check::legalize(m, entry,
                                       {.is_emulated = target == emit::target::wgsl || software_trace,
                                        .erases_prints = !writes_records,
                                        .lowers_asserts = diagnostics,
                                        .expands_prints = writes_records,
                                        .expands_workgroup = true,
                                        .workgroup_starts_zeroed = target == emit::target::wgsl,
                                        .files = files,
                                        .names = names,
                                        .guards_undefined = guards_undefined});
    auto emitted = emit::emit_entry_point(m, legal, target,
                                          {.files = files,
                                           .names = names,
                                           .write_directives = line_directives,
                                           .diagnostics = diagnostics,
                                           .device_features = device_features,
                                           .software_trace = software_trace,
                                           .native = native,
                                           .native_line_table = native_line_table});
    if (!emitted.has_text())
    {
        auto text = cc::string();
        for (auto const& error : emitted.errors)
            text.appendf("{}: error: {}: {}\n", source_name, emit::to_string(error.kind), error.detail);
        return cc::error(cc::move(text));
    }
    auto result = sgl::emitted_source{.text = cc::move(emitted.text),
                                      .spirv = cc::move(emitted.spirv),
                                      .dxil = cc::move(emitted.dxil),
                                      .native_gap = cc::move(emitted.native_gap),
                                      .signature = cc::move(emitted.signature),
                                      .stage = entry.entry_stage,
                                      .entry_point = cc::move(emitted.entry_point),
                                      .bindings = interface_of(m, legal, emitted.bound_names),
                                      .color_targets = emitted.color_targets,
                                      .target_struct = cc::move(emitted.target_struct),
                                      .features = entry.features,
                                      .options = driver::impl::option_names_of(m, e),
                                      .footprint = check::footprint_of(m, legal),
                                      .layouts = cc::move(emitted.layouts),
                                      .lines = cc::move(emitted.lines),
                                      .debug_sites = cc::move(emitted.debug_sites)};
    for (auto const name : names)
        result.line_files.push_back(cc::string(name));
    // A module's `@pixel struct` is named as the program names it, `m.target`, which tells the host it is that module's.
    if (!result.target_struct.empty() && check::is_valid(e.result) && check::is_valid(m.at(e.result).symbol))
        if (auto const module = m.foreign_module_of(m.at(e.result).symbol); !module.empty())
            result.target_struct = cc::format("{}.{}", module, result.target_struct);
    for (auto axis = 0; axis < 3; ++axis)
        result.workgroup[axis] = e.workgroup[axis];
    result.is_dispatched_by_threads = e.is_auto_workgroup;
    result.preferred_subgroup_size = e.preferred_subgroup_size;
    result.unshadowed_bytes = legal.unshadowed_bytes;
    result.float_guarantee = float_guarantee;
    if (float_guarantee != check::float_level::fast)
        result.float_raised_by = raised_by;
    return result;
}

/// What `front` says of the text of entry point `e`: the library files the source reached, and the warnings of the
/// program and of `e` (CHK-402).
void add_source_facts(cc::result<emitted_source, cc::string>& text,
                      driver::impl::front_end const& front,
                      check::flat_entry_point const& e)
{
    if (!text.has_value())
        return;
    for (auto const name : front.reached_library_names)
        text.value().library_files.push_back(cc::string(name));
    text.value().warnings = front.warnings_of(e.function);
}
} // namespace

cc::result<sgl::emitted_source, cc::string> sgl::compile_to_text(text_request const& request)
{
    // CHK-426: an inlined body reads the target and the device as this compile's
    auto const front = driver::impl::run_front_end(request.source, request.source_name, request.library, request.options,
                                                   emit::compile_target_of(request.target, request.device_features));
    if (!front.errors.empty())
        return cc::error(front.errors);
    auto const& m = front.module;

    if (request.run_tests)
    {
        // CHK-354: a test runs with every option at its default, so the compile's values need a check of their own
        auto const defaults = request.options.empty()
                                ? cc::optional<driver::impl::front_end>()
                                : cc::optional<driver::impl::front_end>(driver::impl::run_front_end(
                                      request.source, request.source_name, request.library));
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
        // CHK-402: an entry point whose own body did not check has no flat tree, and its errors say why
        for (auto i = isize(0); i < m.symbols.size(); ++i)
            if (auto const& s = m.symbols[i]; s.kind == check::symbol_kind::function && s.name == request.entry_point
                                              && s.file == front.program_file())
                if (auto own = front.errors_of(check::symbol_id(i)); !own.empty())
                    return cc::error(cc::move(own));
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
    // CHK-402: what was judged of another entry point's body is no error of this one
    if (auto own = front.errors_of(e.function); !own.empty())
        return cc::error(cc::move(own));
    auto result = emit_text(front, e, request.target, request.line_directives, request.diagnostics,
                            request.guards_undefined, request.software_trace, request.weakens_float,
                            request.device_features, request.native, request.native_line_table);
    add_source_facts(result, front, e);
    if (result.has_value() && result.value().float_guarantee != e.float_guarantee)
        result.value().warnings.push_back(cc::format(
            "{}: warning: float-guarantee-weakened: entry point '{}' needs @float(.{}), which '{}' asks for, and runs "
            "at .fast on {}, as the host allowed",
            request.source_name, e.name, float_level_name(e.float_guarantee),
            check::is_valid(e.float_raised_by) ? m.at(e.float_raised_by).name : cc::string(),
            emit::to_string(request.target)));
    if (result.has_value() && result.value().unshadowed_bytes > 0)
        result.value().warnings.push_back(cc::format(
            "{}: warning: workgroup-unshadowed: entry point '{}' holds {} bytes of workgroup memory, past what its "
            "shadow fits beside in 16384, so no read of it is tested for a store before it (LEGAL-69)",
            request.source_name, e.name, result.value().unshadowed_bytes));
    return result;
}

cc::result<cc::vector<sgl::entry_text>, cc::string> sgl::compile_all_to_text(all_text_request const& request)
{
    // CHK-426: each target is a check of its own, since an inlined body reads the target it is written for
    auto fronts = cc::vector<driver::impl::front_end>();
    for (auto const t : request.targets)
    {
        fronts.push_back(driver::impl::run_front_end(request.source, request.source_name, request.library, {},
                                                     emit::compile_target_of(t)));
        if (!fronts.back().errors.empty())
            return cc::error(fronts.back().errors);
    }
    auto result = cc::vector<entry_text>();
    if (fronts.empty())
        return result;
    for (auto i = isize(0); i < fronts.front().module.entry_points.size(); ++i)
        for (auto k = isize(0); k < request.targets.size(); ++k)
        {
            auto const& front = fronts[k];
            auto const& e = front.module.entry_points[i];
            // CHK-402: an entry point's own errors are that element's, and leave the others built
            auto own = front.errors_of(e.function);
            auto text = own.empty() ? emit_text(front, e, request.targets[k], false, false,
                                                check::undefined_guard::none, false, false, {})
                                    : cc::result<emitted_source, cc::string>(cc::error(own));
            add_source_facts(text, front, e);
            result.push_back({.entry_point = e.name, .target = request.targets[k], .text = cc::move(text)});
        }
    return result;
}
