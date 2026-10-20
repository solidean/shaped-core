#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// Uniform control flow (the spec's checking file, "Uniformity").
//
// A barrier waits for every thread of the workgroup, and a derivative compares a pixel with its quad's neighbours,
// so both need every invocation of the group to reach them together.
// The pass reads the core tree an emitter prints, whose shape is what every target runs.
// It must be sound for every target: whatever it accepts reaches each barrier and derivative in uniform flow.
// Refusing all that Tint refuses is no goal; where Tint's coarser analysis refuses a sound program, the WGSL text
// silences Tint's check.
// A barrier is the exception: Tint's verdict on one cannot be silenced, so barriers are judged by WGSL's rules,
// and derivatives and subgroup operations by SGL's own, in a second walk.

namespace
{
/// Why a value, or the control flow, may differ between invocations; none where it is the same in all of them.
struct divergence
{
    bool is = false;
    /// The node that made it so: a branch, a loop's exit, or the read of a value that differs.
    origin where = {};
    /// Of a value, what it is said of: "comes from the stage input id".
    /// Of the control flow, a sentence of its own: "this branch tests a value that comes from the stage input id".
    cc::string why = {};
    /// Of the control flow, the value its branch tested, which a note names of a local set under a branch of the prelude.
    origin tested_where = {};
    cc::string tested_why = {};
    /// Of the control flow, the calls its branch was inlined through, outermost first.
    ast::range_of<call_site> through = {};

    [[nodiscard]] static divergence none() { return {}; }
};

divergence first_of(divergence const& a, divergence const& b)
{
    return a.is ? a : b;
}

/// A construct a `break` or a `continue` of the core tree may leave.
struct exit_scope
{
    bool is_switch = false;
    /// A break or a continue some invocations took and others did not, which WGSL then holds against the whole loop.
    divergence divergent_exit;
};

struct uniformity_pass
{
    checked_module const& m;
    flat_entry_point const& e;
    /// The files below it are the prelude's, which a note names only until the program reads what came from there.
    i32 prelude_count = 0;
    /// Parallel to `e.locals`; a local that is assigned anywhere under divergence is divergent everywhere.
    cc::vector<divergence> locals;
    cc::vector<exit_scope> scopes;
    /// A `return` some invocations took and others did not: every statement after it is divergent.
    divergence returned;
    /// How many `return`s and `continue`s the walk met, which is how a construct knows one left through it.
    isize returns = 0;
    isize continues = 0;
    bool is_changed = false;
    /// Set for the last walk, once the locals are settled.
    bool is_reporting = false;
    /// CHK-284: the flow after a loop is the flow before it, where WGSL's rules hold a divergent `break` against it.
    /// Off, the walk judges barriers as WGSL does; on, derivatives, subgroup operations and indices as SGL does.
    bool reconverges = false;

    struct violation
    {
        flat_expr_id call;
        divergence flow;
    };
    cc::vector<violation> violations;

    /// CHK-300: an index into a binding array, divergent and unmarked, or marked and uniform.
    struct index_finding
    {
        flat_expr_id index;
        bool is_marked = false;
        divergence value;
    };
    cc::vector<index_finding> index_findings;

    /// True for `nonuniform i`, the one call that marks an index.
    [[nodiscard]] bool is_marked(flat_expr_id index) const
    {
        auto const* const c = e.at(index).node.try_as<flat_call>();
        auto const* const record = c != nullptr ? m.builtin_function(c->intrinsic) : nullptr;
        return record != nullptr && record->is_nonuniform_mark;
    }

    /// What a walk of statements hands on: how the flow leaves them, and whether any of them jumps out.
    struct flow_out
    {
        divergence flow;
        bool jumps = false;
    };

    /// A local takes `stored`, in control flow `flow`.
    void settle_local(local_id id, divergence const& stored, divergence const& flow)
    {
        auto const d = stored.is ? stored : set_under(flow);
        auto& slot = locals[index_of(id)];
        if (d.is && !slot.is)
        {
            slot = d;
            is_changed = true;
        }
    }

    /// What a value set in control flow `flow` is said to come from.
    /// A branch of the prelude is no line of the program, so a note names what that branch tested instead.
    [[nodiscard]] divergence set_under(divergence const& flow) const
    {
        if (!flow.is)
            return {};
        if (flow.where.file < prelude_count && !flow.tested_why.empty())
            return {.is = true, .where = flow.tested_where, .why = flow.tested_why};
        return {.is = true, .where = flow.where, .why = cc::string("is set where the control flow differs")};
    }

    [[nodiscard]] builtins::function_record const* record_of(flat_call const& c) const
    {
        return m.builtin_function(c.intrinsic);
    }

    /// Whether a note names the read `read` of a value that differs since `where`.
    /// A stage input differs from the start, and is first seen where it is read.
    /// What the prelude made differ, such as a ray-tracing stage's hit, is first seen where the program reads it.
    [[nodiscard]] bool is_first_seen_at(origin const& where, origin const& read) const
    {
        auto const names_node = [](origin const& o) { return ast::is_valid(o.expr) || ast::is_valid(o.stmt); };
        if (!names_node(where))
            return true;
        return where.file < prelude_count && names_node(read) && read.file >= prelude_count;
    }

    /// The divergence of a value, judging each call it makes against `flow`.
    divergence value(flat_expr_id id, divergence const& flow)
    {
        if (!is_valid(id))
            return {};
        auto const& x = e.at(id);
        auto result = divergence::none();
        x.node.visit(
            [&](flat_local_ref const& l)
            {
                result = locals[index_of(l.local)];
                if (result.is && is_first_seen_at(result.where, x.from))
                    result.where = x.from;
            },
            [&](flat_binding_member const& b)
            {
                // WGSL takes workgroup memory as different in every thread, whatever was stored to it
                if (b.is_workgroup)
                    result = {.is = true,
                              .where = x.from,
                              .why = cc::format("is read from {}.{}, workgroup memory the threads write",
                                                m.at(b.binding).name,
                                                m.at(m.bindings[m.at(b.binding).info].members)[b.member].name)};
            },
            [&](flat_buffer_element const& b)
            {
                // the buffer, or the binding array it is an element of, whose index picks a buffer
                auto const index = first_of(value(b.index, flow), value(b.buffer, flow));
                auto named = b.buffer;
                if (auto const* const element = e.at(named).node.try_as<flat_element>())
                    named = element->object;
                auto const* const member = e.at(named).node.try_as<flat_binding_member>();
                // a buffer the shader also writes may hold what another invocation just stored
                if (m.at(e.at(b.buffer).type).is_mut && member != nullptr)
                    result = {
                        .is = true,
                        .where = x.from,
                        .why = cc::format("is read from {}.{}, which the shader also writes", m.at(member->binding).name,
                                          m.at(m.bindings[m.at(member->binding).info].members)[member->member].name)};
                else
                    result = index;
            },
            [&](flat_member const& member) { result = value(member.object, flow); },
            [&](flat_element const& element)
            {
                auto const index = value(element.index, flow);
                result = first_of(value(element.object, flow), index);
                // an element of a binding array is the resource the index picks, read once per wave
                if (is_reporting && reconverges && e.at(element.object).node.is<flat_binding_member>()
                    && is_resource(m.at(x.type).kind))
                {
                    auto const marked = is_marked(element.index);
                    if (index.is != marked)
                        index_findings.push_back({.index = element.index, .is_marked = marked, .value = index});
                }
                // a resource is the same in every invocation that names it, whatever index named it
                if (is_resource(m.at(x.type).kind))
                    result = {};
            },
            [&](flat_construct const& c)
            {
                for (auto const a : e.at(c.arguments))
                    result = first_of(result, value(a, flow));
            },
            [&](flat_call const& c) { result = call(id, c, flow); },
            [&](flat_not const& n) { result = value(n.operand, flow); },
            [&](flat_and const& a)
            {
                auto const lhs = value(a.lhs, flow);
                // `rhs` runs only where `lhs` is true, which is flow of its own
                result = first_of(lhs, value(a.rhs, first_of(flow, as_branch(lhs, x.from, x.inlined_through))));
            },
            [&](flat_or const& o)
            {
                auto const lhs = value(o.lhs, flow);
                result = first_of(lhs, value(o.rhs, first_of(flow, as_branch(lhs, x.from, x.inlined_through))));
            },
            [&](auto const&) {});
        return result;
    }

    divergence call(flat_expr_id id, flat_call const& c, divergence const& flow)
    {
        auto const* const record = record_of(c);
        // CHK-374: the load reads workgroup memory once every thread has arrived, and hands each the same value
        if (record != nullptr && record->is_uniform_load)
        {
            if (flow.is && is_reporting && !reconverges)
                violations.push_back({.call = id, .flow = flow});
            return {};
        }
        auto result = divergence::none();
        auto const arguments = e.at(c.arguments);
        for (auto const a : arguments)
            result = first_of(result, value(a, flow));
        if (record == nullptr)
            return result;
        auto const is_judged
            = reconverges ? record->uses_derivatives || record->is_subgroup_operation : record->is_barrier;
        if (is_judged && flow.is && is_reporting)
            violations.push_back({.call = id, .flow = flow});
        // CHK-377: what the subgroup agrees on may still differ between the subgroups of a workgroup
        if (record->is_subgroup_operation && !result.is)
            result = {.is = true, .where = e.at(id).from, .why = cc::format("is what {} gave", record->name)};
        // what another thread did to it first is what an atomic gives
        if (record->is_atomic && !result.is)
            result = {.is = true, .where = e.at(id).from, .why = cc::format("is what {} gave", record->name)};
        // CHK-283: a ray-tracing stage's launch index, its ray and its hit are each invocation's own
        if (record->differs_between_invocations && !result.is)
            result = {.is = true, .where = e.at(id).from, .why = cc::string("comes from what the stage is handed")};
        if (!arguments.empty() && !result.is)
            result = stored_to(id, arguments[0]);
        write_through(id, *record, arguments, result, flow);
        return result;
    }

    /// What a load from `object` gives where the shader also stores to it: what another invocation may just have
    /// stored, whether it is named directly or as an element of a binding array.
    [[nodiscard]] divergence stored_to(flat_expr_id id, flat_expr_id object) const
    {
        auto const& t = m.at(e.at(object).type);
        auto const is_stored_to = (t.kind == type_kind::image && t.access == access_mode::read_write)
                               || (t.kind == type_kind::bytes && t.is_mut);
        if (!is_stored_to)
            return {};
        auto named = object;
        if (auto const* const element = e.at(named).node.try_as<flat_element>())
            named = element->object;
        auto const* const member = e.at(named).node.try_as<flat_binding_member>();
        if (member == nullptr)
            return {};
        return {.is = true,
                .where = e.at(id).from,
                .why = cc::format("is loaded from {}.{}, which the shader also stores to", m.at(member->binding).name,
                                  m.at(m.bindings[m.at(member->binding).info].members)[member->member].name)};
    }

    /// The local a call writes through takes what the call gives, in the control flow it stands in (CHK-283).
    void write_through(flat_expr_id id,
                       builtins::function_record const& record,
                       tree_view<flat_expr_id> const& arguments,
                       divergence const& given,
                       divergence const& flow)
    {
        if (record.writes_through == builtins::written_argument::none || arguments.empty())
            return;
        auto const is_first = record.writes_through == builtins::written_argument::first;
        auto const* const l = e.at(is_first ? arguments[0] : arguments.back()).node.try_as<flat_local_ref>();
        if (l == nullptr)
            return;
        // a payload or a parameter comes back from the shaders the call ran, which read what their invocation is handed
        auto const written = is_first ? given
                                      : divergence{.is = true,
                                                   .where = e.at(id).from,
                                                   .why = cc::string("is what the shaders the call ran wrote back")};
        settle_local(l->local, written, flow);
    }

    /// The flow inside a branch on a value of divergence `d`.
    [[nodiscard]] static divergence as_branch(divergence const& d, origin const& where, ast::range_of<call_site> through)
    {
        if (!d.is)
            return {};
        return {.is = true,
                .where = where,
                .why = cc::format("this branch tests a value that {}", d.why),
                .tested_where = d.where,
                .tested_why = d.why,
                .through = through};
    }

    flow_out statements(ast::range_of<flat_stmt_id> range, divergence flow)
    {
        auto jumps = false;
        for (auto const id : e.at(range))
        {
            if (returned.is)
                flow = first_of(flow, returned);
            auto const out = statement(e.at(id), flow);
            flow = out.flow;
            jumps = jumps || out.jumps;
        }
        return {.flow = flow, .jumps = jumps};
    }

    /// A `break` or a `continue` of some invocations and not others, which the loop it leaves takes for all of it.
    void exit(divergence const& flow, bool is_continue)
    {
        if (!flow.is)
            return;
        for (auto i = scopes.size(); i-- > 0;)
        {
            if (scopes[i].is_switch && is_continue)
                continue;
            // a switch's arms meet again after it, as a loop's iterations do not
            if (!scopes[i].is_switch && !scopes[i].divergent_exit.is)
                scopes[i].divergent_exit = flow;
            return;
        }
    }

    /// A loop, a `once` among them: a divergent exit anywhere in it makes all of it divergent.
    /// What follows it is divergent too by WGSL's rules, and by SGL's only where some invocations returned (CHK-284).
    /// A `while` passes its `condition`, which runs at the top of every iteration, so under every flow the body meets.
    /// A `once` runs no later iteration, so by SGL's rules what stands before its divergent exit is not divergent.
    flow_out loop(ast::range_of<flat_stmt_id> body,
                  divergence const& flow,
                  divergence const& bounds,
                  bool repeats,
                  flat_expr_id condition = flat_expr_id::none,
                  flat_stmt const* from = nullptr)
    {
        auto const returns_before = returns;
        auto const branch = from != nullptr ? as_branch(value(condition, flow), from->from, from->inlined_through)
                                            : divergence::none();
        auto const tested = first_of(bounds, branch);
        scopes.push_back({.divergent_exit = tested});
        auto const entered = first_of(flow, tested);
        (void)statements(body, entered);
        auto const exit = scopes.back().divergent_exit;
        // the later iterations run for the invocations that stayed, and test the condition again first
        if (exit.is && !flow.is)
            (void)value(condition, exit);
        if (exit.is && !entered.is && (repeats || !reconverges))
            (void)statements(body, exit);
        scopes.remove_back();
        // a `break` or a `continue` stays inside it, and a `return` goes on out
        auto const after = reconverges ? first_of(flow, returned) : first_of(flow, exit);
        return {.flow = after, .jumps = returns > returns_before};
    }

    flow_out statement(flat_stmt const& s, divergence const& flow)
    {
        auto result = flow_out{.flow = flow};
        s.node.visit(
            [&](flat_let const& l) { settle_local(l.local, value(l.value, flow), flow); },
            [&](flat_var const& v) { settle_local(v.local, value(v.value, flow), flow); },
            [&](flat_assign const& a)
            {
                auto stored = value(a.value, flow);
                // the place's root local takes the value; a buffer element's index is only read
                auto place = a.place;
                while (is_valid(place))
                {
                    auto const& p = e.at(place).node;
                    if (auto const* const member = p.try_as<flat_member>())
                        place = member->object;
                    // an element at a divergent index is a divergent part of the array
                    else if (auto const* const element = p.try_as<flat_element>())
                    {
                        stored = first_of(stored, value(element->index, flow));
                        place = element->object;
                    }
                    else if (auto const* const l = p.try_as<flat_local_ref>())
                    {
                        settle_local(l->local, stored, flow);
                        break;
                    }
                    else
                    {
                        (void)value(place, flow);
                        break;
                    }
                }
            },
            [&](flat_eval const& v) { (void)value(v.value, flow); },
            [&](flat_print const& p) { (void)value(p.value, flow); },
            [&](flat_if const& i)
            {
                auto const inside = first_of(flow, as_branch(value(i.condition, flow), s.from, s.inlined_through));
                auto const then_out = statements(i.then_body, inside);
                auto const else_out = statements(i.else_body, inside);
                // the two sides meet again after it, unless one of them left: then only some invocations go on
                result.jumps = then_out.jumps || else_out.jumps;
                if (result.jumps)
                    result.flow = first_of(inside, first_of(then_out.flow, else_out.flow));
            },
            [&](flat_loop const& l) { result = loop(l.body, flow, {}, true); },
            [&](flat_once const& o) { result = loop(o.body, flow, {}, false); },
            [&](flat_while const& w) { result = loop(w.body, flow, {}, true, w.condition, &s); },
            [&](flat_for const& f)
            {
                auto const bounds = first_of(value(f.first, flow), value(f.end, flow));
                settle_local(f.index, bounds, flow);
                result = loop(f.body, flow, as_branch(bounds, s.from, s.inlined_through), true);
            },
            [&](flat_switch const& sw)
            {
                auto const inside = first_of(flow, as_branch(value(sw.scrutinee, flow), s.from, s.inlined_through));
                auto const jumps_before = returns + continues;
                scopes.push_back({.is_switch = true});
                auto left = divergence::none();
                for (auto const& a : e.at(sw.arms))
                    left = first_of(left, statements(a.body, inside).flow);
                left = first_of(left, statements(sw.default_body, inside).flow);
                scopes.remove_back();
                // the arms meet again after it, unless one left the loop or the function around it
                result.jumps = returns + continues > jumps_before;
                if (result.jumps)
                    result.flow = first_of(inside, left);
            },
            [&](flat_break const&)
            {
                exit(flow, false);
                result.jumps = true;
            },
            [&](flat_continue const&)
            {
                exit(flow, true);
                ++continues;
                result.jumps = true;
            },
            [&](flat_return const& r)
            {
                (void)value(r.value, flow);
                ++returns;
                if (flow.is && !returned.is)
                {
                    returned = {.is = true,
                                .where = s.from,
                                .why = cc::string("some invocations return here, and the others go on"),
                                .tested_where = flow.tested_where,
                                .tested_why = flow.tested_why};
                    // every loop around it runs on for the invocations that stayed
                    for (auto& scope : scopes)
                        if (!scope.is_switch && !scope.divergent_exit.is)
                            scope.divergent_exit = returned;
                }
                result.jumps = true;
            },
            // a discard demotes the invocation to a helper, which still takes part in its quad's derivatives
            [&](flat_discard const&) {}, [&](auto const&) {});
        return result;
    }

    void walk()
    {
        returned = {};
        scopes.clear();
        (void)statements(e.body, {});
    }
};
} // namespace

void checker::judge_uniformity(flat_entry_point const& structured)
{
    // most entry points call nothing that asks, and index no binding array, and legalizing is not free
    auto asks_barrier = false;
    auto asks_reconverged = false;
    for (auto const& x : structured.exprs)
    {
        if (auto const* const c = x.node.try_as<flat_call>())
            if (auto const* const record = out.builtin_function(c->intrinsic))
            {
                asks_barrier = asks_barrier || record->is_barrier || record->is_uniform_load;
                asks_reconverged = asks_reconverged || record->uses_derivatives || record->is_subgroup_operation;
            }
        if (x.node.is<flat_element>())
            asks_reconverged = asks_reconverged || is_resource(out.at(x.type).kind);
    }
    if (!asks_barrier && !asks_reconverged)
        return;

    auto const e = legalize(out, structured);
    auto const judged = [&](bool reconverges)
    {
        auto pass = uniformity_pass{.m = out,
                                    .e = e,
                                    .prelude_count = prelude_count,
                                    .locals = cc::vector<divergence>::create_defaulted(e.locals.size()),
                                    .reconverges = reconverges};

        // CHK-283: what differs between invocations from the start, which is every parameter but the workgroup's id
        for (auto i = isize(0); i < e.locals.size(); ++i)
            if (e.locals[i].kind == local_kind::parameter)
                pass.locals[i] = {.is = true, .why = cc::format("comes from the stage's parameter {}", e.locals[i].name)};
        if (is_valid(e.input))
            pass.locals[0] = {.is = true, .why = cc::string("comes from the stage's input struct")};
        for (auto const& input : e.stage_inputs)
            pass.locals[index_of(input.local)]
                = input.input == stage_input::workgroup_id
                    ? divergence::none()
                    : divergence{.is = true, .why = cc::format("comes from the stage input {}", e.at(input.local).name)};

        // a local only ever becomes divergent, so the walks settle within one per local
        for (auto rounds = isize(0); rounds <= e.locals.size(); ++rounds)
        {
            pass.is_changed = false;
            pass.walk();
            if (!pass.is_changed)
                break;
        }
        pass.is_reporting = true;
        pass.walk();
        return pass;
    };
    auto pass = judged(asks_reconverged);
    if (asks_reconverged && asks_barrier)
        pass.violations.push_back_range(judged(false).violations);

    // an index a parameter names several times is one index where it was written
    auto reported_indices = cc::vector<origin>();
    for (auto const& f : pass.index_findings)
    {
        auto const& index = e.at(f.index);
        auto is_seen = false;
        for (auto const& r : reported_indices)
            is_seen = is_seen || r == index.from;
        if (is_seen)
            continue;
        reported_indices.push_back(index.from);
        auto const where = span_of(index.from.file, index.from.expr);
        if (f.is_marked)
        {
            report(diagnostic_kind::needless_nonuniform, index.from.file, where,
                   "this index is the same in every invocation, so marking it `nonuniform` pays for nothing");
            continue;
        }
        auto const text = text_of(index.from.file, where);
        auto is_operand = true;
        for (auto const ch : text)
            is_operand = is_operand && (cc::is_alphanumeric(ch) || ch == '_' || ch == '.');
        auto& d = report(diagnostic_kind::non_uniform_index, index.from.file, where,
                         cc::format("an index into a binding array that may differ between invocations: mark it "
                                    "`nonuniform {}`, or make it the same in all of them",
                                    is_operand ? cc::string(text) : cc::format("({})", text)));
        auto const& from = f.value.where;
        auto const span = ast::is_valid(from.expr) ? span_of(from.file, from.expr) : span_of(from.file, from.stmt);
        d.notes.push_back({.file = from.file, .where = span, .message = cc::format("this value {}", f.value.why)});
    }
    auto reported = cc::vector<flat_expr_id>();
    for (auto const& v : pass.violations)
    {
        auto is_seen = false;
        for (auto const r : reported)
            is_seen = is_seen || r == v.call;
        if (is_seen)
            continue;
        reported.push_back(v.call);

        auto const& call = e.at(v.call);
        auto const& c = call.node.as<flat_call>();
        auto const* const record = out.builtin_function(c.intrinsic);
        auto const where = span_of(call.from.file, call.from.expr);
        auto& d = report(
            diagnostic_kind::non_uniform_control_flow, call.from.file, where,
            record->is_barrier
                ? cc::format("{} waits for every thread of the workgroup, and not every one reaches it here", record->name)
            : record->is_subgroup_operation
                ? cc::format("{} exchanges values within the subgroup, and not every invocation of it reaches it here",
                             record->name)
                : cc::format("{} takes derivatives across a quad of pixels, and not every pixel of the "
                             "quad reaches it here{}",
                             record->name,
                             record->name.starts_with("sample") ? ": sample before the branch, or name the level or "
                                                                  "the gradients"
                                                                : ": take them before the branch"));
        // a branch of the prelude is no line of the program, so the note names the program's call that reached it
        auto const& from = v.flow.where;
        auto site = call_site();
        if (from.file < prelude_count)
            for (auto const& s : e.at(v.flow.through))
                if (s.file >= prelude_count)
                    site = s;
        if (ast::is_valid(site.call))
        {
            d.notes.push_back(
                {.file = site.file,
                 .where = span_of(site.file, site.call),
                 .message = v.flow.tested_why.empty()
                              ? cc::string("the control flow differs inside this call")
                              : cc::format("a branch inside this call tests a value that {}", v.flow.tested_why)});
            continue;
        }
        auto const span = ast::is_valid(from.expr) ? span_of(from.file, from.expr) : span_of(from.file, from.stmt);
        d.notes.push_back({.file = from.file, .where = span, .message = v.flow.why});
    }
}
