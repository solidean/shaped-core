#include "payload_access.hh"

using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit::impl;

u32 payload_access::dxil_mask() const
{
    auto any_read = false;
    auto any_write = false;
    for (auto i = 0; i < 4; ++i)
    {
        any_read = any_read || reads[i];
        any_write = any_write || writes[i];
    }
    auto mask = u32(0);
    for (auto i = 0; i < 4; ++i)
    {
        auto const read = reads[i] || (i == 0 && !any_read);
        auto const written = writes[i] || (i == 0 && !any_write);
        mask |= (u32(read ? 1 : 0) | u32(written ? 2 : 0)) << (4 * i);
    }
    return mask;
}

// A stage is its payload parameter's reader and writer, and a caller whatever local of the type it traces with.
// A partial write reads the field too, since what it leaves must arrive intact; and a whole-struct access is one
// of every field.
payload_access sgl::emit::impl::payload_access_of(plan const& p, check::type_id type, cc::string_view member)
{
    auto result = payload_access();
    // Every shader of a pipeline must state the same qualifiers, and a host's hit group is compiled apart from the
    // pipeline it joins: so a type only closed pipelines of this module trace is inferred, and any other is widest.
    auto is_traced = false;
    auto is_open = false;
    for (auto const& pipeline : p.m.pipelines)
    {
        if (pipeline.kind != check::pipeline_kind::raytracing)
            continue;
        auto is_its = false;
        for (auto const& ray : p.m.at(p.m.at(p.m.at(pipeline.ray_set).type).members))
            is_its = is_its || ray.type == type;
        is_traced = is_traced || is_its;
        is_open = is_open || (is_its && pipeline.has_host_hit_groups);
    }
    if (!is_traced || is_open)
    {
        for (auto i = 0; i < 4; ++i)
            result.reads[i] = result.writes[i] = true;
        return result;
    }
    auto field = isize(-1);
    auto const members = p.m.at(p.m.at(type).members);
    for (auto i = isize(0); i < members.size(); ++i)
        if (members[i].name == member)
            field = i;
    for (auto const& e : p.m.entry_points)
    {
        auto const stage_slot = e.entry_stage == stage::closest_hit ? 1
                              : e.entry_stage == stage::miss        ? 2
                              : e.entry_stage == stage::any_hit     ? 3
                                                                    : -1;
        auto is_caller = false;
        for (auto const& r : e.traced_rays)
            is_caller = is_caller || p.m.at(p.m.at(p.m.at(r.set).type).members)[r.ray].type == type;
        auto const slot_of = [&](check::local_id local) -> int
        {
            if (stage_slot >= 0 && e.input == type && local == check::local_id(0))
                return stage_slot;
            if (is_caller && e.at(local).type == type)
                return 0;
            return -1;
        };
        // what stands as the object of a member, a place, or a trace's payload is no whole-struct read
        auto is_object = cc::vector<bool>::create_filled(e.exprs.size(), false);
        auto is_place = cc::vector<bool>::create_filled(e.exprs.size(), false);
        for (auto const& x : e.exprs)
        {
            if (auto const* const m = x.node.try_as<check::flat_member>(); m != nullptr && check::is_valid(m->object))
                is_object[index_of(m->object)] = true;
            auto const* const c = x.node.try_as<check::flat_call>();
            auto const* const record = c == nullptr ? nullptr : p.m.builtin_function(c->intrinsic);
            if (record == nullptr || !record->takes_element || e.at(c->arguments).empty())
                continue;
            auto const handed = e.at(c->arguments).back();
            is_place[index_of(handed)] = true;
            // a payload handed on to a nested trace or a callable comes back written, whatever the stage itself
            // does with it: what the nested shaders wrote must survive this stage's exit
            if (auto const* const ref = e.at(handed).node.try_as<check::flat_local_ref>();
                ref != nullptr && slot_of(ref->local) >= 0)
                result.reads[slot_of(ref->local)] = result.writes[slot_of(ref->local)] = true;
        }
        for (auto const& st : e.stmts)
        {
            // a local's value where it is declared is a write of every field
            if (auto const* const l = st.node.try_as<check::flat_let>(); l != nullptr && slot_of(l->local) >= 0)
                result.writes[slot_of(l->local)] = true;
            if (auto const* const v = st.node.try_as<check::flat_var>();
                v != nullptr && check::is_valid(v->value) && slot_of(v->local) >= 0)
                result.writes[slot_of(v->local)] = true;
            auto const* const a = st.node.try_as<check::flat_assign>();
            if (a == nullptr || !check::is_valid(a->place))
                continue;
            // down the member chain to the field right below the local
            auto at = a->place;
            auto top = flat_expr_id::none;
            auto depth = 0;
            while (auto const* const m = e.at(at).node.try_as<check::flat_member>())
            {
                is_place[index_of(at)] = true;
                top = at;
                at = m->object;
                ++depth;
            }
            auto const* const ref = e.at(at).node.try_as<check::flat_local_ref>();
            if (ref == nullptr)
                continue;
            is_place[index_of(at)] = true;
            auto const slot = slot_of(ref->local);
            if (slot < 0)
                continue;
            if (!check::is_valid(top))
                result.writes[slot] = true;
            else if (e.at(top).node.as<check::flat_member>().member == field)
            {
                result.writes[slot] = true;
                result.reads[slot] = result.reads[slot] || depth > 1;
            }
        }
        for (auto i = isize(0); i < e.exprs.size(); ++i)
        {
            auto const& x = e.exprs[i];
            if (auto const* const m = x.node.try_as<check::flat_member>(); m != nullptr && !is_place[i])
                if (auto const* const ref = e.at(m->object).node.try_as<check::flat_local_ref>();
                    ref != nullptr && m->member == field && slot_of(ref->local) >= 0)
                    result.reads[slot_of(ref->local)] = true;
            if (auto const* const ref = x.node.try_as<check::flat_local_ref>();
                ref != nullptr && !is_object[i] && !is_place[i] && slot_of(ref->local) >= 0)
                result.reads[slot_of(ref->local)] = true;
        }
    }
    // a stage that writes a field reads it too: a write it makes only on some paths keeps what came in on the others
    for (auto i = 1; i < 4; ++i)
    {
        result.reads[i] = result.reads[i] || result.writes[i];
        // what a stage writes the caller reads, since it alone consumes the trace; and what a stage reads the caller
        // writes, so it arrives defined
        result.reads[0] = result.reads[0] || result.writes[i];
        result.writes[0] = result.writes[0] || result.reads[i];
    }
    return result;
}

bool sgl::emit::impl::is_payload(plan const& p, check::type_id type)
{
    if (p.e.entry_stage >= stage::raygen && p.e.entry_stage != stage::callable && type == p.e.input)
        return true;
    for (auto const& r : p.e.traced_rays)
        if (p.m.at(p.m.at(p.m.at(r.set).type).members)[r.ray].type == type)
            return true;
    return false;
}
