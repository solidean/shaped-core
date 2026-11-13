#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/impl/flattener.hh>
#include <shaped-graphics-language/check/reserved_bindings.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

/// True where evaluating `id` writes what outlives it: a builtin call that is not pure.
bool sgl::check::impl::writes_outside(flat_entry_point const& e, flat_expr_id id, int depth)
{
    if (!is_known(e, id) || depth > k_max_depth)
        return false;
    auto const& x = e.at(id);
    if (auto const* const call = x.node.try_as<flat_call>(); call != nullptr && !call->is_pure)
        return true;
    if (auto const* const block = x.node.try_as<flat_block>())
        return writes_outside(e, block->body, depth + 1);
    auto result = false;
    for_each_operand(e, x, [&](flat_expr_id operand) { result = result || writes_outside(e, operand, depth + 1); });
    return result;
}

/// True where running `body` writes what outlives it: a print, a store to a buffer element, or an impure builtin.
/// A local is no such thing, so an inlined function that only computes a value writes nothing.
bool sgl::check::impl::writes_outside(flat_entry_point const& e, ast::range_of<flat_stmt_id> body, int depth)
{
    if (!is_known(e, body) || depth > k_max_depth)
        return false;
    for (auto const id : e.at(body))
    {
        if (!is_known(e, id))
            continue;
        auto const& s = e.at(id);
        if (s.node.is<flat_print>())
            return true;
        if (auto const* const assign = s.node.try_as<flat_assign>();
            assign != nullptr && is_known(e, assign->place) && e.at(assign->place).node.is<flat_buffer_element>())
            return true;
        auto result = false;
        for_each_expr_of(e, s, [&](flat_expr_id x) { result = result || writes_outside(e, x, depth + 1); });
        for_each_body_of(
            e, s, [&](ast::range_of<flat_stmt_id> inner) { result = result || writes_outside(e, inner, depth + 1); });
        if (result)
            return true;
    }
    return false;
}

void sgl::check::impl::flattener::note_value_use(type_id type, ast::expr_id from)
{
    auto const needs = c.features_of_type(type);
    if (needs.is_empty() || frames.empty() || !ast::is_valid(from))
        return;
    for (auto const& u : feature_uses)
        if (u.is_value && u.features == needs && !u.within.empty() && u.within.back() == frames.back().function)
            return;
    auto within = cc::vector<symbol_id>();
    for (auto const& fr : frames)
        within.push_back(fr.function);
    feature_uses.push_back(
        {.file = file(), .call = from, .features = needs, .within = cc::move(within), .is_value = true, .granted = granted});
}

void sgl::check::impl::flattener::judge_stage(ast::expr_id call, symbol_id callee)
{
    if (auto const* const record = c.out.builtin_function(c.out.at(callee).intrinsic);
        record != nullptr && !record->features.is_empty())
    {
        auto within = cc::vector<symbol_id>();
        for (auto const& fr : frames)
            within.push_back(fr.function);
        // EMIT-184: an HLSL ray query sees the micromaps of a device that has them, which the compile states for it
        auto const target = compiled_for();
        auto const may_see_micromaps = record->declares_only && record->features.has(feature::ray_query)
                                    && (target.api == graphics_api::dx12 || target.api == graphics_api::vulkan);
        if (may_see_micromaps)
            entry.consulted.set(feature::opacity_micromap); // EMIT-264
        if (may_see_micromaps && target.device_features.has(feature::opacity_micromap))
        {
            auto const sees = feature_set(feature::opacity_micromap);
            feature_uses.push_back({.file = file(), .call = call, .features = sees, .within = within, .granted = sees});
        }
        feature_uses.push_back(
            {.file = file(), .call = call, .features = record->features, .within = cc::move(within), .granted = granted});
    }
    // CHK-298, CHK-379: a test's run is as many lanes as its subgroup operations and derivatives need (EVAL-140)
    if (is_test)
    {
        auto const chain = entry.at(current()->chain);
        auto const written = chain.empty() ? call_site{.file = file(), .call = call} : chain[0];
        auto const ray_stages = stage_bit(stage::raygen) | stage_bit(stage::miss) | stage_bit(stage::closest_hit)
                              | stage_bit(stage::any_hit) | stage_bit(stage::intersection) | stage_bit(stage::callable);
        if (auto const& s = c.out.at(callee); s.info >= 0 && (c.out.functions[s.info].stages & ~ray_stages) == 0)
            ray_stage_calls.push_back({.file = file(), .call = call, .callee = callee, .written = written});
        return;
    }
    if (entry.entry_stage == stage::none)
        return;
    auto const* const record = c.out.builtin_function(c.out.at(callee).intrinsic);
    // CHK-512: what only a test states, no target writes
    if (record != nullptr && record->is_test_only)
    {
        stage_violations.push_back({.file = file(), .call = call, .callee = callee, .is_test_only = true});
        return;
    }
    if (record != nullptr && record->is_quad_operation && entry.entry_stage == stage::compute)
        quad_calls.push_back({.file = file(), .call = call, .callee = callee});
    if (record != nullptr && entry.entry_stage == stage::compute
        && (record->is_barrier || record->is_uniform_load || record->is_subgroup_operation || record->is_quad_operation))
        group_calls.push_back({.file = file(), .call = call, .callee = callee});
    // CHK-404: a decision runs as often as the traversal asks it, whichever stage's loop asks
    if (auto const decision = decision_stage();
        decision != stage::none && record != nullptr
        && (record->is_atomic || record->is_image_store || record->is_bytes_store))
    {
        stage_violations.push_back({.file = file(),
                                    .call = call,
                                    .callee = callee,
                                    .decision = decision,
                                    .is_inlined_decision = inlined_decision != stage::none});
        return;
    }
    // EMIT-163: what only a pixel stage needs of the device, which every target then judges alike
    if (record != nullptr && entry.entry_stage == stage::pixel && !record->pixel_features.is_empty())
    {
        auto within = cc::vector<symbol_id>();
        for (auto const& fr : frames)
            within.push_back(fr.function);
        feature_uses.push_back({.file = file(),
                                .call = call,
                                .features = record->pixel_features,
                                .within = cc::move(within),
                                .granted = granted});
    }
    auto const& s = c.out.at(callee);
    if (s.info >= 0 && (c.out.functions[s.info].stages & stage_bit(entry.entry_stage)) == 0)
        stage_violations.push_back({.file = file(), .call = call, .callee = callee});
}

auto sgl::check::impl::flattener::decision_stage() const -> stage
{
    if (inlined_decision != stage::none)
        return inlined_decision;
    auto const is_decision = entry.entry_stage == stage::any_hit || entry.entry_stage == stage::intersection;
    return is_decision ? entry.entry_stage : stage::none;
}

void sgl::check::impl::flattener::judge_decision_store(flat_expr_id place, ast::expr_id target)
{
    auto const decision = decision_stage();
    for (auto depth = 0; decision != stage::none && is_valid(place) && depth < k_max_inline_depth; ++depth)
    {
        auto const& node = entry.at(place).node;
        if (node.is<flat_buffer_element>())
        {
            stage_violations.push_back({.file = file(),
                                        .call = target,
                                        .decision = decision,
                                        .is_inlined_decision = inlined_decision != stage::none,
                                        .is_buffer_store = true});
            return;
        }
        if (auto const* const member = node.try_as<flat_member>())
            place = member->object;
        else if (auto const* const element = node.try_as<flat_element>())
            place = element->object;
        else
            return;
    }
}

auto sgl::check::impl::flattener::holds_error(type_id type) const -> bool
{
    if (type == checked_module::error_type)
        return true;
    if (!is_valid(type))
        return false;
    for (auto const& m : c.out.at(c.out.at(type).members))
        if (holds_error(m.type))
            return true;
    return false;
}

auto sgl::check::impl::flattener::current() -> frame_ref
{
    auto ref = frame_ref{.owner = &frames, .at = frames.size() - 1};
#if CC_ASSERT_ENABLED
    ref.base = frames.data();
#endif
    return ref;
}

auto sgl::check::impl::flattener::fail() -> flat_expr_id
{
    mark_failed();
    return flat_expr_id::none;
}

void sgl::check::impl::flattener::mark_failed()
{
    is_failed = true;
    if (!first_failure.has_value() && (ast::is_valid(cursor.expr) || ast::is_valid(cursor.stmt)))
        first_failure = cursor;
}

auto sgl::check::impl::flattener::concrete(type_id type) -> type_id
{
    if (frames.empty() || frames.back().bindings.empty())
        return type;
    auto const result = c.substitute_existing(type, frames.back().bindings);
    if (!is_valid(result) || c.is_open(result))
    {
        mark_failed();
        return type;
    }
    return result;
}

auto sgl::check::impl::flattener::add_local(local_kind kind, cc::string_view desired, type_id type) -> local_id
{
    type = concrete(type);
    is_failed = is_failed || !c.is_sound(type);
    meets_error = meets_error || holds_error(type);
    // `_` is a name nobody reads, and no name at all in WGSL
    entry.locals.push_back({.kind = kind,
                            .name = entry.names.mint(desired == "_" ? cc::string_view("unused") : desired),
                            .type = type,
                            .is_mut = kind == local_kind::var});
    return local_id(entry.locals.size() - 1);
}

auto sgl::check::impl::flattener::add_label(cc::string_view desired) -> label_id
{
    entry.labels.push_back({.name = label_names.mint(desired)});
    return label_id(entry.labels.size() - 1);
}

auto sgl::check::impl::flattener::add_list(cc::span<flat_expr_id const> list) -> ast::range_of<flat_expr_id>
{
    auto const range = ast::range_of<flat_expr_id>{.first = u32(entry.expr_lists.size()), .count = u32(list.size())};
    entry.expr_lists.push_back_range(list);
    return range;
}

auto sgl::check::impl::flattener::add_list(cc::span<flat_stmt_id const> list) -> ast::range_of<flat_stmt_id>
{
    auto const range = ast::range_of<flat_stmt_id>{.first = u32(entry.stmt_lists.size()), .count = u32(list.size())};
    entry.stmt_lists.push_back_range(list);
    return range;
}

auto sgl::check::impl::flattener::root_body() -> ast::range_of<flat_stmt_id>
{
    auto all = hoisted_lets;
    for (auto const stmt : block)
        all.push_back(stmt);
    return add_list(all);
}

auto sgl::check::impl::flattener::add_arms(cc::span<flat_arm const> list) -> ast::range_of<flat_arm>
{
    auto const range = ast::range_of<flat_arm>{.first = u32(entry.arms.size()), .count = u32(list.size())};
    entry.arms.push_back_range(list);
    return range;
}

auto sgl::check::impl::flattener::local_ref(local_id local, ast::expr_id from) -> flat_expr_id
{
    return add_expr(entry.at(local).type, from, flat_local_ref{.local = local});
}

auto sgl::check::impl::flattener::is_substitutable(flat_expr_id id) const -> bool
{
    if (!is_valid(id))
        return false;
    auto const& x = entry.at(id);
    if (x.node.is<flat_literal>() || x.node.is<flat_int_literal>() || x.node.is<flat_wide_literal>()
        || x.node.is<flat_bool_literal>() || x.node.is<flat_enum_value>())
        return true;
    auto const* const ref = x.node.try_as<flat_local_ref>();
    return ref != nullptr && !entry.at(ref->local).is_mut;
}

auto sgl::check::impl::flattener::marked_index(flat_expr_id id) const -> flat_expr_id
{
    auto const* const call = is_valid(id) ? entry.at(id).node.try_as<flat_call>() : nullptr;
    auto const* const record = call != nullptr ? c.out.builtin_function(call->intrinsic) : nullptr;
    return record != nullptr && record->mark != builtins::index_mark::none ? entry.at(call->arguments)[0]
                                                                           : flat_expr_id::none;
}

auto sgl::check::impl::flattener::is_substitutable_index(flat_expr_id id) const -> bool
{
    return is_substitutable(id) || is_substitutable(marked_index(id));
}

auto sgl::check::impl::flattener::is_literal_construction(flat_expr_id id) const -> bool
{
    auto const* const made = is_valid(id) ? entry.at(id).node.try_as<flat_construct>() : nullptr;
    if (made == nullptr)
        return false;
    for (auto const a : entry.at(made->arguments))
    {
        auto const& x = entry.at(a).node;
        auto const is_literal = x.is<flat_literal>() || x.is<flat_int_literal>() || x.is<flat_wide_literal>()
                             || x.is<flat_bool_literal>() || x.is<flat_enum_value>();
        if (!is_literal && !is_literal_construction(a))
            return false;
    }
    return true;
}

auto sgl::check::impl::flattener::again(flat_expr_id id, ast::expr_id from) -> flat_expr_id
{
    auto const x = entry.at(id);
    if (auto const* const ref = x.node.try_as<flat_local_ref>())
        return local_ref(ref->local, from);
    if (auto const* const l = x.node.try_as<flat_literal>())
        return add_expr(x.type, from, *l);
    if (auto const* const l = x.node.try_as<flat_int_literal>())
        return add_expr(x.type, from, *l);
    if (auto const* const l = x.node.try_as<flat_wide_literal>())
        return add_expr(x.type, from, *l);
    if (auto const* const l = x.node.try_as<flat_bool_literal>())
        return add_expr(x.type, from, *l);
    if (auto const* const v = x.node.try_as<flat_enum_value>())
        return add_expr(x.type, from, *v);
    if (auto const* const m = x.node.try_as<flat_binding_member>())
        return add_expr(x.type, from, *m);
    if (auto const* const smp = x.node.try_as<flat_file_sampler>())
        return add_expr(x.type, from, *smp);
    if (auto const* const element = x.node.try_as<flat_element>())
    {
        auto const object = again(element->object, from);
        auto const index = again_as_written(element->index, from);
        return add_expr(x.type, from, flat_element{.object = object, .index = index});
    }
    // a place a `mut` parameter stands for, whose indices were pinned when it was bound (`pin_place`)
    if (auto const* const member = x.node.try_as<flat_member>())
    {
        auto const index = member->member;
        auto const letters = member->letters;
        auto const object = again(member->object, from);
        return add_expr(x.type, from, flat_member{.object = object, .member = index, .letters = letters});
    }
    if (auto const* const length = x.node.try_as<flat_buffer_length>())
        return add_expr(x.type, from, flat_buffer_length{.buffer = again(length->buffer, from)});
    if (auto const* const element = x.node.try_as<flat_buffer_element>())
    {
        auto const buffer = again(element->buffer, from);
        auto const index = again_as_written(element->index, from);
        return add_expr(x.type, from, flat_buffer_element{.buffer = buffer, .index = index});
    }
    // the mark stays where it was written, which is what a diagnostic about it points at
    if (auto const marked = marked_index(id); is_valid(marked))
    {
        flat_expr_id const arguments[] = {again(marked, from)};
        auto copy = x;
        copy.node.as<flat_call>().arguments = add_list(arguments);
        entry.exprs.push_back(cc::move(copy));
        return flat_expr_id(entry.exprs.size() - 1);
    }
    if (auto const* const made = x.node.try_as<flat_construct>())
    {
        // copied first: `again` appends to the lists the view reads
        auto arguments = cc::vector<flat_expr_id>();
        for (auto const a : entry.at(made->arguments))
            arguments.push_back(a);
        for (auto& a : arguments)
            a = again(a, from);
        return add_expr(x.type, from, flat_construct{.arguments = add_list(arguments)});
    }
    // the int arithmetic a walk's variable is (CHK-415), whose operands are substitutable in turn
    if (auto const* const call = x.node.try_as<flat_call>())
    {
        auto arguments = cc::vector<flat_expr_id>();
        for (auto const a : entry.at(call->arguments))
            arguments.push_back(a);
        for (auto& a : arguments)
            a = again(a, from);
        auto copy = x;
        copy.node.as<flat_call>().arguments = add_list(arguments);
        entry.exprs.push_back(cc::move(copy));
        return flat_expr_id(entry.exprs.size() - 1);
    }
    return fail();
}

auto sgl::check::impl::flattener::bound_index(flat_expr_id index, local_kind kind, cc::string_view name, origin from)
    -> flat_expr_id
{
    if (is_substitutable_index(index))
        return index;
    auto const marked = marked_index(index);
    auto const value = is_valid(marked) ? marked : index;
    auto const local = add_local(kind, name, entry.at(value).type);
    add_stmt(from, flat_let{.local = local, .value = value});
    if (!is_valid(marked))
        return local_ref(local, from.expr);
    flat_expr_id const arguments[] = {local_ref(local, from.expr)};
    auto mark = entry.at(index);
    mark.node.as<flat_call>().arguments = add_list(arguments);
    entry.exprs.push_back(cc::move(mark));
    return flat_expr_id(entry.exprs.size() - 1);
}

auto sgl::check::impl::flattener::again_as_written(flat_expr_id index, ast::expr_id from) -> flat_expr_id
{
    if (!is_substitutable_index(index))
        return again(index, from);
    // by value: the copy of a mark's argument appends to the arrays
    auto copy = entry.at(index);
    if (auto const marked = marked_index(index); is_valid(marked))
    {
        flat_expr_id const arguments[] = {again_as_written(marked, from)};
        copy.node.as<flat_call>().arguments = add_list(arguments);
    }
    entry.exprs.push_back(cc::move(copy));
    return flat_expr_id(entry.exprs.size() - 1);
}

auto sgl::check::impl::flattener::pin_place(flat_expr_id place) -> flat_expr_id
{
    auto const x = entry.at(place);
    auto const pin_index
        = [&](flat_expr_id index) { return bound_index(index, local_kind::let, "at", entry.at(index).from); };
    if (auto const* const member = x.node.try_as<flat_member>())
    {
        auto const index = member->member;
        auto const letters = member->letters;
        auto const object = pin_place(member->object);
        return add_expr(x.type, x.from.expr, flat_member{.object = object, .member = index, .letters = letters});
    }
    if (auto const* const element = x.node.try_as<flat_element>())
    {
        auto const index = element->index;
        auto const object = pin_place(element->object);
        return add_expr(x.type, x.from.expr, flat_element{.object = object, .index = pin_index(index)});
    }
    if (auto const* const element = x.node.try_as<flat_buffer_element>())
    {
        auto const index = element->index;
        auto const buffer = element->buffer;
        return add_expr(x.type, x.from.expr, flat_buffer_element{.buffer = buffer, .index = pin_index(index)});
    }
    return place;
}

auto sgl::check::impl::flattener::is_resource_member(flat_expr_id id) const -> bool
{
    if (!is_valid(id) || !is_resource(c.out.at(entry.at(id).type).kind))
        return false;
    // an element of a binding array, at an index that reads the same wherever it stands
    if (auto const* const element = entry.at(id).node.try_as<flat_element>())
        return entry.at(element->object).node.is<flat_binding_member>() && is_substitutable_index(element->index);
    return entry.at(id).node.is<flat_binding_member>() || entry.at(id).node.is<flat_file_sampler>();
}

auto sgl::check::impl::flattener::evaluate_once(flat_expr_id value, cc::string_view name, ast::expr_id from)
    -> evaluated_once
{
    if (!is_valid(value) || is_substitutable(value))
        return {.first = value, .later = value};

    auto const type = entry.at(value).type;
    auto const local = add_local(local_kind::temporary, name, type);
    auto const label = add_label(name);
    auto const where = origin{.file = file(), .expr = from};
    flat_stmt_id const body[] = {
        make_stmt(where, flat_let{.local = local, .value = value}),
        make_stmt(where, flat_leave{.target = label, .value = local_ref(local, from)}),
    };
    return {.first = add_expr(type, from, flat_block{.label = label, .body = add_list(body)}),
            .later = local_ref(local, from)};
}

auto sgl::check::impl::flattener::is_swizzled(ast::expr_id id) const -> bool
{
    auto const* const m = ast::is_valid(id) ? ast().at(id).node.try_as<ast::member>() : nullptr;
    if (m == nullptr)
        return false;
    auto const kind = tables().target_at(id).kind;
    return kind == target_kind::swizzle
        || (kind == target_kind::field && ast::is_valid(m->object)
            && tables().target_at(m->object).kind == target_kind::swizzle);
}

auto sgl::check::impl::flattener::swizzled_at(ast::expr_id id) const -> swizzled
{
    auto const& where = tables().target_at(id);
    auto result = swizzled{
        .operand = ast().at(id).node.as<ast::member>().object,
        .letters = where.kind == target_kind::swizzle ? swizzle::unpacked(where.index)
                                                      : swizzle{.fields = {i8(where.index)}, .count = 1},
    };
    while (tables().target_at(result.operand).kind == target_kind::swizzle)
    {
        result.letters = result.letters.over(swizzle::unpacked(tables().target_at(result.operand).index));
        result.operand = ast().at(result.operand).node.as<ast::member>().object;
    }
    return result;
}

auto sgl::check::impl::flattener::field_type(type_id type, i32 field) const -> type_id
{
    return c.out.at(c.out.at(type).members)[field].type;
}

auto sgl::check::impl::flattener::swizzle_of(flat_expr_id object, swizzle letters, type_id type, ast::expr_id id)
    -> flat_expr_id
{
    if (!is_valid(object))
        return fail();
    auto const object_type = entry.at(object).type;
    if (letters.count == 1)
        return add_expr(type, id, flat_member{.object = object, .member = letters.fields[0]});
    if (c.out.builtin_type_of(object_type) != nullptr)
        return add_expr(type, id, flat_member{.object = object, .letters = letters});
    auto const once = entry.at(object).node.is<flat_local_ref>() ? evaluated_once{.first = object, .later = object}
                                                                 : evaluate_once(object, "swizzled", id);
    auto arguments = cc::vector<flat_expr_id>();
    for (auto i = 0; i < letters.count; ++i)
    {
        auto const operand = i == 0 ? once.first : again(once.later, id);
        arguments.push_back(add_expr(field_type(object_type, letters.fields[i]), id,
                                     flat_member{.object = operand, .member = letters.fields[i]}));
    }
    return add_expr(type, id, flat_construct{.arguments = add_list(arguments)});
}

auto sgl::check::impl::flattener::texel_conversion(flat_expr_id value, type_id type, ast::expr_id id) -> flat_expr_id
{
    if (!is_valid(value))
        return fail();
    auto const value_type = entry.at(value).type;
    auto const* const source = c.out.builtin_type_of(value_type);
    auto const* const texel = c.out.builtin_type_of(type);
    if (source == nullptr || texel == nullptr)
        return fail();
    auto const from_width = source->leaf_count;
    auto const to_width = texel->leaf_count;
    if (to_width < from_width)
    {
        auto letters = swizzle{.count = i8(to_width)};
        for (auto i = 0; i < to_width; ++i)
            letters.fields[i] = i8(i);
        return swizzle_of(value, letters, type, id);
    }

    // a widened texel is a vector, whose fields are its component
    auto const component = field_type(type, 0);
    auto arguments = cc::vector<flat_expr_id>();
    if (from_width == 1)
        arguments.push_back(value);
    else
    {
        auto const once = evaluate_once(value, "texel", id);
        for (auto i = 0; i < from_width; ++i)
        {
            auto const operand = i == 0 ? once.first : again(once.later, id);
            arguments.push_back(add_expr(component, id, flat_member{.object = operand, .member = i}));
        }
    }
    for (auto i = from_width; i < to_width; ++i)
    {
        auto const fill = i == 3 ? 1 : 0;
        if (texel->leaf_kind == value_kind::scalar_float)
            arguments.push_back(add_expr(component, id, flat_literal{.value = f64(fill)}));
        else
            arguments.push_back(
                add_expr(component, id,
                         flat_int_literal{.value = fill, .is_unsigned = texel->leaf_kind == value_kind::scalar_uint}));
    }
    return add_expr(type, id, flat_construct{.arguments = add_list(arguments)});
}

auto sgl::check::impl::flattener::read_swizzle(swizzled const& s, type_id type, ast::expr_id id) -> flat_expr_id
{
    return swizzle_of(flatten_expr(s.operand), s.letters, type, id);
}

void sgl::check::impl::flattener::flatten_swizzle_assign(origin from, ast::assign_stmt const& assign, cc::string_view op)
{
    auto const s = swizzled_at(assign.target);
    auto const type = tables().type_at(assign.target);
    auto const operand = pin_place(flatten_expr(s.operand));
    auto value = flatten_expr(assign.value);
    if (!is_valid(operand) || !is_valid(value))
    {
        mark_failed();
        return;
    }
    judge_decision_store(operand, assign.target);
    auto const operand_type = entry.at(operand).type;
    if (op != "=")
    {
        type_id const types[] = {type, entry.at(value).type};
        auto const callee = c.find_operator(file(), op.subview({.offset = 0, .size = op.size() - 1}), types);
        if (!is_valid(callee))
        {
            mark_failed();
            return;
        }
        auto const read = swizzle_of(again(operand, assign.target), s.letters, type, assign.target);
        flat_expr_id const arguments[] = {read, value};
        value = builtin_call(assign.value, callee, arguments);
    }
    if (s.letters.count == 1 || c.out.builtin_type_of(operand_type) != nullptr)
    {
        add_stmt(from, flat_assign{.place = swizzle_of(operand, s.letters, type, assign.target), .value = value});
        return;
    }
    // EMIT-143: a struct of the program takes one component at a time, from the value held once
    auto const held = add_local(local_kind::temporary, "swizzled", type);
    add_stmt(from, flat_let{.local = held, .value = value});
    for (auto i = 0; i < s.letters.count; ++i)
    {
        auto const element = field_type(operand_type, s.letters.fields[i]);
        auto const object = i == 0 ? operand : again(operand, assign.target);
        auto const place = add_expr(element, assign.target, flat_member{.object = object, .member = s.letters.fields[i]});
        auto const component
            = add_expr(element, assign.target, flat_member{.object = local_ref(held, assign.target), .member = i});
        add_stmt(from, flat_assign{.place = place, .value = component});
    }
}

namespace
{
/// CHK-520: the axes of its thread id a tree reads, counted to the highest.
/// 0 for none, 1 for `id.x` alone, 2 where it reads `id.y`, and 3 where it reads `id.z` or hands the vector on whole.
i32 thread_id_axes(flat_entry_point const& e)
{
    auto id = local_id::none;
    for (auto const& input : e.stage_inputs)
        if (input.input == stage_input::thread_id)
            id = input.local;
    if (!is_valid(id))
        return 0;
    auto const names_id = [&](flat_expr_id x)
    {
        auto const* const ref = is_valid(x) ? e.at(x).node.try_as<flat_local_ref>() : nullptr;
        return ref != nullptr && ref->local == id;
    };
    auto is_read_by_axis = cc::vector<u8>::create_filled(e.exprs.size(), 0);
    auto axes = 0;
    for (auto const& x : e.exprs)
    {
        auto const* const member = x.node.try_as<flat_member>();
        if (member == nullptr || !names_id(member->object))
            continue;
        is_read_by_axis[index_of(member->object)] = 1;
        auto const highest = [&]
        {
            if (!member->is_swizzle())
                return i32(member->member);
            auto result = 0;
            for (auto k = 0; k < member->letters.count; ++k)
                result = cc::max(result, i32(member->letters.fields[k]));
            return result;
        }();
        axes = cc::max(axes, highest + 1);
    }
    for (auto i = isize(0); i < e.exprs.size(); ++i)
        if (names_id(flat_expr_id(i)) && is_read_by_axis[i] == 0)
            return 3;
    return axes;
}

/// CHK-521: why a stated workgroup size wastes what a bare @compute would not, which would choose `chosen`; none where
/// it wastes nothing, and never for one thread, a serial kernel's size.
cc::optional<cc::string> stated_workgroup_waste(cc::fixed_array<i32, 3> const& size,
                                                cc::fixed_array<i32, 3> const& chosen,
                                                i32 axes)
{
    auto const total = size[0] * size[1] * size[2];
    auto const stated
        = cc::format("@compute({}, {}, {}) runs {} threads to a workgroup", size[0], size[1], size[2], total);
    auto const instead = chosen[1] == 1 ? cc::format("{}", chosen[0])
                       : chosen[2] == 1 ? cc::format("{}, {}", chosen[0], chosen[1])
                                        : cc::format("{}, {}, {}", chosen[0], chosen[1], chosen[2]);
    auto const bare = cc::format("a bare @compute would choose ({}) here", instead);
    if (total == 1)
        return {};
    if (total < 32)
        return cc::format("{}, less than a wave of any GPU, which idles the rest of it; {}", stated, bare);
    if (total % 32 != 0)
        return cc::format("{}, which leaves a partial wave in every workgroup; {}", stated, bare);
    if (total > 256 || size[0] > 256 || size[1] > 256 || size[2] > 64)
        return cc::format("{}, which no webgpu device runs: it has 256 threads, at most (256, 256, 64) along the axes; "
                          "{}",
                          stated, bare);
    if (axes == 1 && (size[1] > 1 || size[2] > 1))
        return cc::format("{} along more than one axis, and the body reads the thread id's x alone, so on a grid of "
                          "one row every row of a workgroup but the first runs past it; {}",
                          stated, bare);
    return {};
}

/// CHK-436: a float compared with itself, `x != x` or `x == x`, which a fast-math compiler folds.
/// `is_nan` and `is_inf` test the bits on every target, which no such compiler can fold, so they are no such test.
bool is_special_value_test(flat_entry_point const& e, flat_call const& call)
{
    auto const& registry = builtins::default_registry();
    if (!is_valid(call.intrinsic) || index_of(call.intrinsic) >= registry.functions.size())
        return false;
    auto const& name = registry.at(call.intrinsic).name;
    if ((name != "equal" && name != "not_equal") || call.arguments.count != 2)
        return false;
    auto const arguments = e.at(call.arguments);
    auto const* const a = e.at(arguments[0]).node.try_as<flat_local_ref>();
    auto const* const b = e.at(arguments[1]).node.try_as<flat_local_ref>();
    return a != nullptr && b != nullptr && a->local == b->local;
}

/// CHK-389: once, however many trees inline the function the loop stands in.
/// A loop of an inlined function names the call it was reached through, whose arguments may be what its bounds read.
located_diagnostic& report_unroll(checker& c, flattener::unroll_report const& u)
{
    for (auto& d : c.out.diagnostics)
        if (d.what.kind == u.kind && d.file == u.file && d.what.where == u.where)
            return d;
    auto& d = c.report(u.kind, u.file, u.where, u.detail);
    if (ast::is_valid(u.site.call))
        d.notes.push_back({.file = u.site.file,
                           .where = c.span_of(u.site.file, u.site.call),
                           .message = "reached through this call, whose arguments stand in for its parameters"});
    return d;
}
} // namespace

cc::shared_ptr<cc::set<cc::string>> checker::names_up_to(isize count) const
{
    auto names = cc::make_shared<cc::set<cc::string>>();
    for (auto i = isize(0); i < count; ++i)
        if (!out.symbols[i].name.empty() && !takes_no_emitted_name(out.symbols[i]))
            (void)names->insert(out.symbols[i].name);
    return names;
}

checker::taken_names checker::module_names()
{
    // a link's names below are made where they are first needed (`seal_layer`)
    if (module_names_below == nullptr && module_names_below_count > 0)
        module_names_below = names_up_to(module_names_below_count);
    if (own_module_names == nullptr || module_names_for != out.symbols.size())
    {
        auto names = cc::make_shared<cc::set<cc::string>>();
        // a function of a type scope is reached through its type, `x.row(1)`, and never written under its own name
        for (auto i = module_names_below_count; i < out.symbols.size(); ++i)
            if (!out.symbols[i].name.empty() && !takes_no_emitted_name(out.symbols[i]))
                (void)names->insert(out.symbols[i].name);
        own_module_names = names.get();
        // the checked module keeps both alive, since its flat trees' mints point at them
        if (out.module_names.empty() && module_names_below != nullptr)
            out.module_names.push_back(module_names_below);
        out.module_names.push_back(cc::move(names));
        module_names_for = out.symbols.size();
    }
    return {.below = module_names_below.get(), .own = own_module_names};
}

void checker::flatten_test(i32 index)
{
    // by value: flattening appends to the module's vectors
    auto const test = out.tests[index];
    auto const info = out.at(test.symbol).info;
    // One whose text an earlier phase found an error in has been reported already: flattening it could only add a
    // second diagnostic to the first.
    if (has_syntax_error_in(test.file, test.extent))
        return;
    if (!notes[info].is_body_sound || !inlines_whole(test.symbol))
        return;

    auto f = flattener{.c = *this, .is_test = true};
    f.entry.name = "test";
    f.entry.function = test.symbol;
    f.entry.result = checked_module::void_type;
    // CHK-333: what the test lists, which its driver binds
    for (auto const binding : out.at(out.functions[out.at(test.symbol).info].bindings))
        f.entry.bindings.push_back(binding);
    auto const taken = module_names();
    f.entry.names.also_taken = taken.own;
    f.entry.names.also_taken_below = taken.below;
    f.frames.push_back({.function = test.symbol, .file = test.file, .result = checked_module::void_type});

    auto const& body = ast_of(test.file).at(test.declaration).node.as<ast::test_decl>().body;
    for (auto const stmt : ast_of(test.file).at(body.statements))
        f.flatten_stmt(stmt);
    f.judge_text_types();
    // CHK-265: the test's `require` and a helper's that the test reaches are used by the calls that need them
    for (auto const& u : f.feature_uses)
        mark_requires_used(u.within, u.features);

    // Reported for a test that expects it too, which is how a corpus file pins the rule.
    // There it stands at the test's own call, so a helper the test reaches it through carries no error of the test's.
    auto const report_reached = [&](stage_violation const& v, cc::string_view why)
    {
        auto const site = test.expects_diagnostics() ? v.written : call_site{.file = v.file, .call = v.call};
        unsupported(site.file, span_of(site.file, site.call),
                    cc::format("a test that reaches {}{}", out.at(v.callee).name, why));
    };
    // a pipeline's trace or callable runs against tables a test has none of
    for (auto const& v : f.ray_stage_calls)
        report_reached(v, ", which only the ray-tracing stages run");

    // what a test reaches is of every compile, even where an entry point's tree judged it first
    for (auto const& u : f.unroll_reports)
        report_unroll(*this, u).entry_points.clear();

    // A test that expects a diagnostic is never run (CHK-232), so its tree is judged for its constants alone.
    // Nothing else of the tree is reported: what it expects stands in its text, where the check pass found it already.
    if (test.expects_diagnostics())
    {
        if (!f.is_failed)
        {
            f.entry.body = f.root_body();
            judge_constants(f.entry);
            judge_assignments(f.entry);
            (void)judge_uniformity(f.entry);
        }
        return;
    }

    // CHK-227: once, however many trees inline the function the assert stands in
    for (auto const& a : f.effectful_asserts)
        report_once(diagnostic_kind::unsupported_yet, a.file, span_of(a.file, a.expr),
                    "an assert whose condition writes a buffer, prints, or calls a builtin with an effect")
            .entry_points.clear();
    if (f.is_failed && !f.meets_error)
        unsupported(test.file, test.where,
                    f.failure_detail.empty() ? cc::string("a test whose body reaches a construct the flat tree cannot "
                                                          "hold yet")
                                             : f.failure_detail);
    if (f.is_failed || !f.ray_stage_calls.empty())
        return;
    f.entry.body = f.root_body();
    judge_constants(f.entry);
    judge_assignments(f.entry);
    // CHK-377, CHK-282: its lanes reach each subgroup operation and derivative together, as an entry point's do
    (void)judge_uniformity(f.entry);
    out.tests.own(index).unit = i32(out.test_units.size());
    out.test_units.push_back(cc::move(f.entry));
}

checker::const_evaluation checker::evaluate_const(symbol_id scope, i32 file, ast::expr_id expr, type_id type)
{
    // Bodies are checked once every signature is known, and inlining needs them, so a const checks those it reaches
    // first; recursion among them is find_recursion's to report, and stops this walk at the first repeat.
    auto reached = cc::vector<symbol_id>{scope};
    for (auto i = isize(0); i < reached.size(); ++i)
        for (auto k = isize(0); k < calls.size(); ++k)
        {
            if (calls[k].caller != reached[i])
                continue;
            auto const callee = calls[k].callee;
            auto is_known = false;
            for (auto const r : reached)
                is_known = is_known || r == callee;
            if (is_known)
                continue;
            reached.push_back(callee);
            if (out.at(callee).state == symbol_state::checked)
                check_body(callee);
        }

    auto f = flattener{.c = *this};
    f.entry.name = "const";
    f.entry.function = scope;
    f.entry.result = type;
    auto const taken = module_names();
    f.entry.names.also_taken = taken.own;
    f.entry.names.also_taken_below = taken.below;
    f.frames.push_back({.function = scope, .file = file, .result = type});
    f.is_const_evaluation = true;
    auto const result = f.flatten_expr(expr);
    // CHK-426: a const is one value for every compile, so it holds nothing a compile stands for
    if (!f.target_in_const.empty())
        return {.kind = diagnostic_kind::invalid_constant_argument, .detail = cc::move(f.target_in_const)};
    if (f.is_failed || !is_valid(result))
        return {.detail = "a const whose value reaches a construct the flat tree cannot hold yet"};
    f.add_stmt(origin{.file = file, .expr = expr}, flat_return{.value = result});
    f.entry.body = f.root_body();

    // a const reads no binding, so the run has no inputs, and a value that takes this long is no constant
    constexpr auto fuel = 100'000;
    auto const o = interpret(out, f.entry, {}, {.fuel = fuel});
    auto const without_value = [](cc::string detail)
    { return const_evaluation{.kind = diagnostic_kind::constant_without_value, .detail = cc::move(detail)}; };
    switch (o.status)
    {
    case run_status::ok:
        return {.leaves = o.result.leaves, .is_ok = true};
    case run_status::out_of_fuel:
        return without_value(cc::format("takes more than {} steps to compute", fuel));
    case run_status::assertion_failed:
        return without_value("fails an assert while it is computed");
    case run_status::program_error:
        return without_value(cc::format("has no value: {}", o.detail));
    default:
        return {.detail = cc::format("a const whose value the abstract machine cannot compute: {} {}",
                                     to_string(o.status), o.detail)};
    }
}

bool checker::is_sound(type_id type) const
{
    if (!is_valid(type) || type == checked_module::error_type)
        return false;
    for (auto const& m : out.at(out.at(type).members))
        if (!is_sound(m.type))
            return false;
    return true;
}

void checker::flatten_entry_point(symbol_id id, traversal_request const* traversal)
{
    auto const& s = out.at(id);
    auto const& info = out.functions[s.info];
    auto const& note = notes[s.info];
    if (info.entry_stage == stage::none || !note.is_valid_entry || !inlines_whole(id))
        return;

    auto const parameters = out.at(info.parameters);
    // A compute entry point hands nothing back, so `nothing` is its result and not a hole.
    auto const wants_result = info.entry_stage != stage::compute && info.entry_stage != stage::task;
    auto is_parameter_sound = true;
    for (auto const& parameter : parameters)
        is_parameter_sound = is_parameter_sound && is_sound(parameter.type);
    if (!is_parameter_sound || (wants_result && !is_sound(info.result)))
        return;

    auto f = flattener{.c = *this};
    f.entry.entry_stage = info.entry_stage;
    f.entry.name = traversal != nullptr ? traversal->name : cc::string(s.name);
    f.entry.function = id;
    // CHK-271: the stage struct, when there is one, is the first parameter; every other is a stage input
    if (!parameters.empty() && parameters[0].input == stage_input::none)
        f.entry.input = parameters[0].type;
    f.entry.result = is_valid(info.pixel_targets) ? info.pixel_targets : info.result;
    f.pixel_targets = info.pixel_targets;
    f.entry.workgroup[0] = info.workgroup[0];
    f.entry.workgroup[1] = info.workgroup[1];
    f.entry.workgroup[2] = info.workgroup[2];
    f.entry.is_auto_workgroup = info.is_auto_workgroup;
    f.entry.preferred_subgroup_size = info.preferred_subgroup_size;
    f.entry.features = info.features;
    // CHK-527: a reserved binding an internal entry point of the prelude lists keeps its reserved place
    for (auto const binding : out.at(info.bindings))
        if (is_internal(id) && reserved_binding_named(out.at(binding).name) != nullptr)
            f.entry.reserved.push_back(binding);
        else
            f.entry.bindings.push_back(binding);

    // Every module-level name is taken, so no local can hide a type, a binding or a builtin an emitter writes.
    auto const taken = module_names();
    f.entry.names.also_taken = taken.own;
    f.entry.names.also_taken_below = taken.below;
    f.entry.names.reserve(s.name);

    f.raise_float_level(id);
    f.frames.push_back({.function = id, .file = s.file, .result = info.result});
    // CHK-326: a ray-tracing stage's one parameter of the target is its payload, `locals[0]`, which the stage writes
    // through; its launch, its ray and its hit it reads from the target where the entry point starts
    if (info.entry_stage >= stage::raygen)
    {
        f.entry.input = type_id::none;
        // CHK-345: a traversal function takes the payload its any hit writes, which no intersection is handed
        if (traversal != nullptr)
        {
            f.fused = flattener::fusion{.any_hit = traversal->any_hit};
            if (is_valid(traversal->payload))
            {
                f.fused.value().payload = f.add_local(local_kind::parameter, "payload", traversal->payload);
                f.entry.input = traversal->payload;
            }
            if (is_valid(traversal->any_hit))
            {
                // a report's `is_hit` is the one bool at hand, since `bool` is a builtin rather than a prelude struct
                auto boolean = type_id::none;
                for (auto const& m : out.at(out.at(info.result).members))
                    if (m.name == "is_hit")
                        boolean = m.type;
                auto const ends = f.add_local(local_kind::var, "ends_search", boolean);
                f.add_stmt({.file = s.file, .expr = ast::expr_id::none},
                           flat_var{.local = ends,
                                    .value = f.add_expr(boolean, ast::expr_id::none, flat_bool_literal{.value = false})});
                f.fused.value().ends_search = ends;
                f.entry.ends_search = ends;
            }
        }
        for (auto const& parameter : parameters)
            if (parameter.is_mut)
            {
                auto const local = f.add_local(local_kind::parameter, parameter.name, parameter.type);
                f.entry.input = parameter.type;
                f.current()->bound.push_back(
                    {.where = {.kind = target_kind::parameter, .index = i32(parameter.field)}, .local = local});
            }
        for (auto const& parameter : parameters)
        {
            if (parameter.is_mut)
                continue;
            // CHK-342: a procedural hit or candidate carries the attributes the target hands the stage
            auto const& t = out.at(parameter.type);
            if (is_valid(t.generic))
            {
                auto const is_hit = out.at(t.symbol).name == "procedural_hit";
                auto const* const found
                    = prelude_names.get_ptr(is_hit ? cc::string_view("current_procedural_hit")
                                                   : cc::string_view("current_procedural_candidate"));
                if (found == nullptr || found->empty())
                {
                    f.mark_failed();
                    continue;
                }
                auto const builder = found->front();
                auto const attributes = f.add_local(local_kind::parameter, "attributes", t.element);
                f.entry.attributes = attributes;
                type_id const bindings[] = {out.at(out.functions[out.at(builder).info].type_parameters)[0], t.element};
                flat_expr_id const arguments[] = {f.local_ref(attributes, ast::expr_id::none)};
                i32 const slots[] = {0};
                auto const inlined = f.inline_bound(ast::expr_id::none, builder, arguments, slots, {}, bindings);
                auto const value = f.add_expr(parameter.type, ast::expr_id::none,
                                              flat_block{.label = inlined.label, .body = inlined.body});
                auto const local = f.add_local(local_kind::let, parameter.name, parameter.type);
                f.add_stmt({.file = s.file, .expr = ast::expr_id::none}, flat_let{.local = local, .value = value});
                f.current()->bound.push_back(
                    {.where = {.kind = target_kind::parameter, .index = i32(parameter.field)}, .local = local});
                continue;
            }
            auto const source
                = parameter.input == stage_input::launch_id           ? cc::string_view("launch_index")
                : parameter.input == stage_input::launch_size         ? cc::string_view("launch_dimensions")
                : out.name_of(parameter.type) == "ray"                ? cc::string_view("current_ray")
                : out.name_of(parameter.type) == "triangle_hit"       ? cc::string_view("current_triangle_hit")
                : out.name_of(parameter.type) == "triangle_candidate" ? cc::string_view("current_triangle_candidate")
                : out.name_of(parameter.type) == "procedural_box"     ? cc::string_view("current_procedural_box")
                                                                      : cc::string_view();
            auto const* const found = prelude_names.get_ptr(source);
            if (source.empty() || found == nullptr || found->empty())
            {
                f.mark_failed();
                continue;
            }
            auto const read = found->front();
            auto value = flat_expr_id::none;
            if (is_valid(out.at(read).intrinsic))
                value = f.builtin_call(ast::expr_id::none, read, {});
            else
            {
                auto const inlined = f.inline_call(ast::expr_id::none, read, {});
                value = f.add_expr(parameter.type, ast::expr_id::none,
                                   flat_block{.label = inlined.label, .body = inlined.body});
            }
            auto const local = f.add_local(local_kind::let, parameter.name, parameter.type);
            f.add_stmt({.file = s.file, .expr = ast::expr_id::none}, flat_let{.local = local, .value = value});
            f.current()->bound.push_back(
                {.where = {.kind = target_kind::parameter, .index = i32(parameter.field)}, .local = local});
            if (f.fused.has_value() && source == "current_procedural_box")
                f.fused.value().box = local;
        }
    }
    // Every parameter is a local, in the order written: the stage struct at `locals[0]` when there is one.
    for (auto const& parameter : info.entry_stage >= stage::raygen ? cc::span<check::parameter const>() : parameters)
    {
        auto const local = f.add_local(local_kind::parameter, parameter.name, parameter.type);
        f.current()->bound.push_back(
            {.where = {.kind = target_kind::parameter, .index = i32(parameter.field)}, .local = local});
        if (parameter.input != stage_input::none)
            f.entry.stage_inputs.push_back({.input = parameter.input, .local = local});
    }

    auto const& body = ast_of(s.file).at(s.declaration).node.as<ast::fun_decl>().body;
    if (ast::is_valid(body.value))
        f.flatten_return({.file = s.file, .expr = body.value}, body.value);
    for (auto const stmt : ast_of(s.file).at(body.statements))
        f.flatten_stmt(stmt);
    f.judge_text_types();
    f.place_target_layouts();

    // CHK-322: what the body calls needs a device's feature, which the entry point declares like one of its signature
    // CHK-402: what is judged of this entry point's inlined body is its own, which a compile of a sibling leaves out.
    // One that another tree judged first without an entry point, a test's, stays of every compile.
    auto const of_entry = [&](auto&& make) -> located_diagnostic&
    {
        auto const before = out.diagnostics.size();
        auto& d = make();
        if ((out.diagnostics.size() > before || !d.entry_points.empty()) && !d.entry_points.values().contains(id))
            d.entry_points.push_back(id);
        return d;
    };
    // once, however many entry points inline the function it stands in, the same detail naming every one of them
    auto const report_of_entry
        = [&](diagnostic_kind kind, i32 file, source_span where, cc::string detail) -> located_diagnostic&
    {
        return of_entry(
            [&]() -> located_diagnostic&
            {
                for (auto& d : out.diagnostics)
                    if (d.what.kind == kind && d.file == file && d.what.where == where && d.detail == detail)
                        return d;
                return report(kind, file, where, cc::move(detail));
            });
    };

    auto used = feature_set();
    for (auto const& u : f.feature_uses)
        used |= u.features;
    auto const declared = notes[s.info].declared_features;
    for (auto i = isize(0); i < k_feature_count; ++i)
    {
        auto const needed = feature(i);
        if (!used.has(needed))
            continue;
        // CHK-426: a use inside a branch whose `has` granted the feature needs no `require`
        auto is_granted_everywhere = true;
        for (auto const& u : f.feature_uses)
            if (u.features.has(needed) && !u.granted.has(needed))
                is_granted_everywhere = false;
        if (!declared.has(needed) && !is_granted_everywhere)
        {
            auto& d = report_of_entry(diagnostic_kind::feature_not_declared, s.file,
                                      ast_of(s.file).at(s.declaration).node.as<ast::fun_decl>().name,
                                      cc::format("{} needs {}, which neither its file, a binding it lists nor its body "
                                                 "requires",
                                                 s.name, name_of(needed)));
            for (auto const& u : f.feature_uses)
                if (u.features.has(needed) && !u.granted.has(needed))
                {
                    d.notes.push_back({.file = u.file,
                                       .where = span_of(u.file, u.call),
                                       .message = u.is_value ? "a value that needs it" : "the call that needs it"});
                    break;
                }
        }
    }
    // a body's `require` that declares it is used, as one declaring a need of the signature is (CHK-265)
    for (auto const& u : f.feature_uses)
        mark_requires_used(u.within, u.features);
    // only where it adds one: a prelude function reached from a program has its features from the prelude's own
    // check, and its entry is shared
    if ((out.functions[s.info].features | used) != out.functions[s.info].features)
        out.functions.own(s.info).features |= used;
    f.entry.features |= used;
    for (auto const& u : f.unroll_reports)
        of_entry([&]() -> located_diagnostic& { return report_unroll(*this, u); });
    // CHK-436: at `fast` a target may assume no NaN or infinity arises, and decide a test for one ahead of time
    if (f.entry.float_guarantee == float_level::fast)
        for (auto const& x : f.entry.exprs)
            if (auto const* const call = x.node.try_as<flat_call>(); call != nullptr && ast::is_valid(x.from.expr)
                                                                     && !is_prelude_file(x.from.file)
                                                                     && is_special_value_test(f.entry, *call))
                of_entry(
                    [&]() -> located_diagnostic&
                    {
                        return report_once(diagnostic_kind::nan_test_at_fast, x.from.file,
                                           span_of(x.from.file, x.from.expr),
                                           "a float compared with itself tests for a NaN, which code at "
                                           "@float(.fast) may assume never arises, so a target may fold the test; "
                                           "is_nan tests the bits and holds at every level, or the function needs "
                                           "@float(.specials)");
                    });
    // CHK-227: once, however many trees inline the function the assert stands in
    for (auto const& a : f.effectful_asserts)
        of_entry(
            [&]() -> located_diagnostic&
            {
                return report_once(diagnostic_kind::unsupported_yet, a.file, span_of(a.file, a.expr),
                                   "an assert whose condition writes a buffer, prints, or calls a builtin with an "
                                   "effect");
            });
    for (auto const& v : f.stage_violations)
    {
        auto const where = span_of(v.file, v.call);
        if (v.is_test_only)
        {
            report_of_entry(diagnostic_kind::stage_not_allowed, v.file, where,
                            cc::format("a {} entry point reaches this, and only a test calls {}, which states what "
                                       "its run binds or which lane it runs in",
                                       check::stage_name(info.entry_stage), out.at(v.callee).name));
            continue;
        }
        if (v.decision == stage::none)
        {
            report_of_entry(diagnostic_kind::stage_not_allowed, v.file, where,
                            cc::format("a {} entry point reaches this, and {} is @stages without it",
                                       check::stage_name(info.entry_stage), out.at(v.callee).name));
            continue;
        }
        // CHK-345: a traversal fuses an intersection and an any hit whose own trees reported it already
        if (traversal != nullptr)
            continue;
        // CHK-404: a write runs as often as its function, which traversal decides for a decision
        auto const* const record = v.is_buffer_store ? nullptr : out.builtin_function(out.at(v.callee).intrinsic);
        auto const is_any_hit = v.decision == stage::any_hit;
        auto const what
            = v.is_inlined_decision
                ? cc::format("an inline trace calls this as its {} decision", is_any_hit ? "any-hit" : "intersection")
                : cc::format("this is reached from an {} entry point", check::stage_name(v.decision));
        auto const how_often
            = is_any_hit
                ? cc::string("which runs an implementation-defined number of times: for candidates the traversal "
                             "later rejects, and more than once per candidate unless the geometry sets "
                             "NO_DUPLICATE_ANYHIT_INVOCATION, which Metal has no equivalent of")
                : cc::string("which runs as often as the traversal reaches its box, an implementation-defined "
                             "number of times");
        auto written = cc::string();
        if (v.is_buffer_store)
            written = "this store to a buffer would write a number of times no spec defines, and race with itself";
        else if (record != nullptr && record->is_atomic)
        {
            // an image's texel atomic is the prelude's internal `texel_max`, which the program wrote as `.max`
            auto name = cc::string_view(out.at(v.callee).name);
            if (name.starts_with("texel_"))
                name = name.subview(6);
            written = cc::format("the atomic {} would run a number of times no spec defines", name);
        }
        else
            written = cc::format("the {} would write a number of times no spec defines", out.at(v.callee).name);
        report_of_entry(
            diagnostic_kind::stage_not_allowed, v.file, where,
            cc::format("{}, {}; a decision writes no memory through a binding, so {}", what, how_often, written));
    }
    // CHK-380: HLSL forms a compute stage's quads from its threads' ids, which only one axis keeps consecutive
    auto const is_linear = info.workgroup[0] % 4 == 0 && info.workgroup[1] == 1 && info.workgroup[2] == 1;
    for (auto const& q : is_linear ? cc::span<stage_violation const>() : cc::span<stage_violation const>(f.quad_calls))
        report_of_entry(diagnostic_kind::invalid_entry_point, q.file, span_of(q.file, q.call),
                        cc::format("{} forms quads along one axis, so a compute entry point that reaches it has a "
                                   "workgroup of (x, 1, 1) whose x is a multiple of 4, and one reaching it has ({}, "
                                   "{}, "
                                   "{})",
                                   out.at(q.callee).name, info.workgroup[0], info.workgroup[1], info.workgroup[2]));
    // CHK-520: what a bare @compute reaches reads nothing of the group whose size the compile chooses
    for (auto const& g :
         info.is_auto_workgroup ? cc::span<stage_violation const>(f.group_calls) : cc::span<stage_violation const>())
        report_of_entry(diagnostic_kind::invalid_entry_point, g.file, span_of(g.file, g.call),
                        cc::format("{} reads the workgroup it runs in, and {} is a bare @compute, which chooses its "
                                   "own workgroup size; it states one, `@compute(64)`",
                                   out.at(g.callee).name, s.name));
    auto const reaches_discard = info.entry_stage != stage::pixel && !f.discards.empty();
    for (auto const& d : info.entry_stage != stage::pixel ? cc::span<origin const>(f.discards) : cc::span<origin const>())
        report_of_entry(diagnostic_kind::stage_not_allowed, d.file, span_of(d.file, d.expr),
                        cc::format("a {} entry point reaches this, and only a pixel stage discards",
                                   check::stage_name(info.entry_stage)));
    if ((info.stages & stage_bit(info.entry_stage)) == 0)
        report_of_entry(diagnostic_kind::stage_not_allowed, s.file,
                        ast_of(s.file).at(s.declaration).node.as<ast::fun_decl>().name,
                        cc::format("{} is a {} entry point, and its own @stages leaves that out", s.name,
                                   check::stage_name(info.entry_stage)));
    // CHK-213: a gap of this pass is reported, so an entry point never vanishes without a word.
    if (f.is_failed && !f.meets_error)
    {
        auto const name = ast_of(s.file).at(s.declaration).node.as<ast::fun_decl>().name;
        if (!f.first_failure.has_value())
            unsupported(s.file, name,
                        cc::format("{}: its body reaches a construct the flat tree cannot hold yet", s.name));
        else
        {
            // the construct itself, and the entry point it was reached from, however many calls deep
            auto const at = f.first_failure.value();
            auto const where = ast::is_valid(at.expr) ? span_of(at.file, at.expr) : span_of(at.file, at.stmt);
            auto& d = report_of_entry(diagnostic_kind::unsupported_yet, at.file, where,
                                      f.failure_detail.empty()
                                          ? cc::string("a construct the flat tree cannot hold yet, here")
                                          : f.failure_detail);
            d.notes.push_back({.file = s.file,
                               .where = name,
                               .message = cc::format("reached from the {} entry point {}",
                                                     check::stage_name(info.entry_stage), s.name)});
        }
    }
    if (f.is_failed || !f.stage_violations.empty() || reaches_discard || (info.stages & stage_bit(info.entry_stage)) == 0
        || (!is_linear && !f.quad_calls.empty()) || (info.is_auto_workgroup && !f.group_calls.empty()))
        return;
    f.entry.body = f.root_body();

    // CHK-520: a bare @compute's size follows the axes of its thread id the body reads, each a multiple of 64
    if (info.entry_stage == stage::compute)
    {
        auto const axes = thread_id_axes(f.entry);
        auto const chosen = axes >= 3 ? cc::fixed_array<i32, 3>{4, 4, 4}
                          : axes == 2 ? cc::fixed_array<i32, 3>{8, 8, 1}
                                      : cc::fixed_array<i32, 3>{64, 1, 1};
        // CHK-521: a stated size a bare @compute could have chosen is habit rather than the algorithm's, and is judged
        auto is_group_read = !f.group_calls.empty();
        for (auto const binding : out.at(info.bindings))
            is_group_read = is_group_read || out.bindings[out.at(binding).info].is_workgroup;
        for (auto const& parameter : parameters)
            is_group_read = is_group_read || check::reads_group(parameter.input);
        if (info.is_auto_workgroup)
            f.entry.workgroup = chosen;
        else if (auto detail = stated_workgroup_waste(info.workgroup, chosen, axes); detail.has_value() && !is_group_read)
            report_of_entry(diagnostic_kind::workgroup_wastes_lanes, s.file,
                            find_attribute(s.file, ast_of(s.file).at(s.declaration).attributes, "compute")->name,
                            cc::move(detail.value()));
    }

    // CHK-485: a task stage launches its mesh workgroups once, as the last thing its body does
    if (info.entry_stage == stage::task)
    {
        auto const is_dispatch = [&](flat_expr_id id)
        {
            auto const* const call = is_valid(id) ? f.entry.at(id).node.try_as<flat_call>() : nullptr;
            auto const* const record
                = call != nullptr && is_valid(call->intrinsic) ? out.builtin_function(call->intrinsic) : nullptr;
            return record != nullptr && record->name == "dispatch_mesh";
        };
        auto dispatches = 0;
        for (auto i = isize(0); i < f.entry.exprs.size(); ++i)
            dispatches += is_dispatch(flat_expr_id(i)) ? 1 : 0;
        // the last statement's value, through the blocks a call is flattened into, down to what each leaves with
        auto ends_in_dispatch = false;
        auto top = f.entry.at(f.entry.body);
        for (auto depth = 0; depth < 8 && !top.empty() && !ends_in_dispatch; ++depth)
        {
            auto const& last = f.entry.at(top[top.size() - 1]).node;
            auto value = flat_expr_id::none;
            if (auto const* const eval = last.try_as<flat_eval>())
                value = eval->value;
            else if (auto const* const leave = last.try_as<flat_leave>())
                value = leave->value;
            if (!is_valid(value))
                break;
            ends_in_dispatch = is_dispatch(value);
            auto const* const block = f.entry.at(value).node.try_as<flat_block>();
            if (block == nullptr)
                break;
            top = f.entry.at(block->body);
        }
        if (dispatches != 1 || !ends_in_dispatch)
        {
            report(diagnostic_kind::invalid_entry_point, s.file,
                   ast_of(s.file).at(s.declaration).node.as<ast::fun_decl>().name,
                   cc::format("{} is a task stage, which calls dispatch_mesh once, as the last statement of its body",
                              s.name));
            return;
        }
    }

    // Every later walk stops descending at the limit and leaves a hole where it stopped, so a tree past it must never
    // reach them: the hole would surface as a missing id rather than as the limit (CHK-268).
    auto probe = depth_probe{.e = f.entry};
    probe.body(f.entry.body, 0);
    if (probe.found.has_value())
    {
        auto const& at = probe.found.value();
        auto const where = ast::is_valid(at.expr) ? span_of(at.file, at.expr) : span_of(at.file, at.stmt);
        report(diagnostic_kind::nesting_too_deep, at.file, where,
               cc::format("'{}' nests deeper than {} levels here, with every call inlined", s.name, k_max_program_depth));
        return;
    }
    // CHK-516: here, with every call inlined, so a copy reached through a generic or a return is judged too
    if (auto const misuse = find_hit_object_misuse(f.entry); misuse.has_value())
    {
        auto const& at = misuse.value().where;
        unsupported(at.file, ast::is_valid(at.expr) ? span_of(at.file, at.expr) : span_of(at.file, at.stmt),
                    misuse.value().detail);
        return;
    }
    judge_constants(f.entry);
    judge_assignments(f.entry);
    auto legalized = judge_uniformity(f.entry);
    f.entry.options = options_reached(id, traversal != nullptr ? traversal->any_hit : symbol_id::none);
    // legalizing copies the entry point's own fields, so the copy takes what was set after it
    if (legalized.has_value())
    {
        legalized.value().options = f.entry.options;
        f.entry.legalized = cc::make_shared<flat_entry_point const>(cc::move(legalized.value()));
    }
    // CHK-527: the prelude's own entry points are no program's, and only a polyfill compiles them, by name
    if (is_internal(id))
        out.internal_entry_points.push_back(cc::move(f.entry));
    else
        out.entry_points.push_back(cc::move(f.entry));
}

void checker::flatten_metal_traversals()
{
    auto is_any = false;
    for (auto const& p : out.pipelines)
    {
        if ((p.kind != pipeline_kind::hit_group && p.kind != pipeline_kind::raytracing)
            || !out.is_reached_file(out.at(p.symbol).file))
            continue;
        is_any = true;
        if (p.kind != pipeline_kind::hit_group || !p.is_procedural || out.at(p.symbol).file != i32(out.files.size()) - 1)
            continue;
        auto const rays = out.at(out.at(out.at(p.ray_set).type).members);
        auto const records = out.at(p.records);
        for (auto r = isize(0); r < rays.size(); ++r)
        {
            auto const request = traversal_request{.name = cc::format("sgl_{}_{}", out.at(p.symbol).name, rays[r].name),
                                                   .any_hit = records[r * 2 + 1],
                                                   .payload = rays[r].type};
            flatten_entry_point(p.intersection, &request);
        }
    }
    if (!is_any)
        return;
    // CHK-345: a record without a closest hit calls this one on metal, where a table slot holds a function or crashes
    // it stands for no function of the source, so it is owned by a ray-tracing function of the program
    auto owner = symbol_id::none;
    for (auto const& fn : out.functions)
        if (fn.entry_stage >= stage::raygen && out.at(fn.symbol).file == i32(out.files.size()) - 1)
            owner = fn.symbol;
    if (!is_valid(owner))
        return;
    auto f = flattener{.c = *this};
    f.entry.entry_stage = stage::closest_hit;
    f.entry.function = owner;
    f.entry.name = "sgl_empty_closest_hit";
    f.entry.result = checked_module::void_type;
    f.entry.features = feature_set(feature::raytracing_pipeline);
    f.entry.names.reserve(f.entry.name);
    f.frames.push_back({.file = i32(out.files.size()) - 1});
    f.entry.body = f.root_body();
    out.entry_points.push_back(cc::move(f.entry));
}
