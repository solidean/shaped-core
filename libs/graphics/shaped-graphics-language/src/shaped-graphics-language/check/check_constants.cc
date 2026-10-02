#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/interpret/interpret.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// Constants (the spec's checking file, CHK-270 and CHK-310 to CHK-312).
//
// WGSL folds an expression of constants when it creates the shader, and refuses one it cannot fold.
// So the check pass folds every constant of the tree an entry point or a test inlines to, by the abstract machine,
// and refuses the same, on every target: an entry point is written for every target or for none (EMIT-13).

namespace
{
enum class judged : u8
{
    unknown,
    /// Not a constant, or one whose value is sound.
    sound,
    /// A constant that was reported, or holds one that was: nothing around it is reported again.
    reported,
};

struct constant_pass
{
    checker& c;
    checked_module const& m;
    flat_entry_point const& e;
    cc::vector<judged> states;

    void report(flat_expr_id id, diagnostic_kind kind, cc::string_view detail)
    {
        auto const& x = e.at(id);
        auto const where
            = ast::is_valid(x.from.expr) ? c.span_of(x.from.file, x.from.expr) : c.span_of(x.from.file, x.from.stmt);
        // once, however many trees inline the function the expression stands in
        for (auto const& d : c.out.diagnostics)
            if (d.what.kind == kind && d.file == x.from.file && d.what.where == where)
                return;
        auto& d = c.report(kind, x.from.file, where, cc::format("`{}` {}", c.text_of(x.from.file, where), detail));
        // a parameter of an inlined function stands for its argument, so the call may be what makes it constant
        if (x.inlined_through.count > 0)
        {
            auto const& site = e.call_sites[isize(x.inlined_through.first) + isize(x.inlined_through.count) - 1];
            d.notes.push_back({.file = site.file,
                               .where = c.span_of(site.file, site.call),
                               .message = "reached through this call, whose arguments stand in for its parameters"});
        }
    }

    /// The value of constant `id`, which judging its operands has found sound.
    [[nodiscard]] outcome value_of(flat_expr_id id) const { return evaluate_constant(m, e, id); }

    judged judge(flat_expr_id id)
    {
        auto& state = states[index_of(id)];
        if (state != judged::unknown)
            return state;
        state = judged::sound;
        state = judge_node(id);
        return state;
    }

    judged judge_all(ast::range_of<flat_expr_id> range)
    {
        auto result = judged::sound;
        for (auto const argument : e.at(range))
            if (judge(argument) == judged::reported)
                result = judged::reported;
        return result;
    }

    judged judge_node(flat_expr_id id)
    {
        auto const& x = e.at(id);
        if (auto const* const construct = x.node.try_as<flat_construct>())
            return judge_all(construct->arguments);
        if (auto const* const member = x.node.try_as<flat_member>())
            return judge(member->object);
        if (auto const* const n = x.node.try_as<flat_not>())
            return judge(n->operand);
        if (auto const* const a = x.node.try_as<flat_and>())
            return judge(a->lhs) == judged::reported || judge(a->rhs) == judged::reported ? judged::reported
                                                                                          : judged::sound;
        if (auto const* const o = x.node.try_as<flat_or>())
            return judge(o->lhs) == judged::reported || judge(o->rhs) == judged::reported ? judged::reported
                                                                                          : judged::sound;
        if (auto const* const call = x.node.try_as<flat_call>())
            return judge_call(id, *call);
        return judged::sound;
    }

    judged judge_call(flat_expr_id id, flat_call const& call)
    {
        if (judge_all(call.arguments) == judged::reported)
            return judged::reported;
        auto const* const record = m.builtin_function(call.intrinsic);
        auto const arguments = e.at(call.arguments);
        if (record == nullptr || arguments.empty())
            return judged::sound;

        // CHK-270 and CHK-311: WGSL judges a shift's count and an integer divisor alone, whatever their left side is
        auto const last = arguments[arguments.size() - 1];
        if (record->judged_last != builtins::judged_operand::none && is_constant(m, e, last))
        {
            auto const operand = value_of(last);
            if (operand.status != run_status::ok)
                return judged::sound;
            for (auto const& leaf : operand.result.leaves)
            {
                if (record->judged_last == builtins::judged_operand::shift_count && leaf.bits > 31u)
                {
                    report(id, diagnostic_kind::shift_out_of_range,
                           cc::format("shifts by {}, which moves every bit out of 32; a constant count is 0 to 31",
                                      leaf.kind == value_kind::scalar_int ? i64(leaf.as_int()) : i64(leaf.bits)));
                    return judged::reported;
                }
                if (record->judged_last == builtins::judged_operand::divisor && leaf.bits == 0)
                {
                    report(id, diagnostic_kind::constant_without_value,
                           "divides an integer by a constant zero, which no target gives a value");
                    return judged::reported;
                }
            }
        }

        if (!is_constant(m, e, id))
            return judged::sound;

        // CHK-311: a call of constants that EVAL leaves without a value is wrong on every run
        auto const folded = value_of(id);
        if (folded.status == run_status::program_error)
        {
            report(id, diagnostic_kind::constant_without_value, cc::format("has no value: {}", folded.detail));
            return judged::reported;
        }
        if (folded.status != run_status::ok)
            return judged::sound;

        // CHK-312: what WGSL folds exactly and cannot hold
        if (record->unrepresentable_when_constant != nullptr)
        {
            auto in = cc::vector<scalar>();
            for (auto const argument : arguments)
                for (auto const& leaf : value_of(argument).result.leaves)
                    in.push_back(leaf.widened());
            if (auto const why = record->unrepresentable_when_constant(in); !why.empty())
            {
                report(id, diagnostic_kind::constant_not_representable,
                       cc::format("is {}, which WGSL refuses in a constant", why));
                return judged::reported;
            }
        }
        // a half past its range is infinite as well, which WGSL refuses alike
        for (auto const& folded_leaf : folded.result.leaves)
            if (auto const leaf = folded_leaf.widened();
                leaf.kind == value_kind::scalar_float && !(leaf.as_float() - leaf.as_float() == 0.0f))
            {
                report(id, diagnostic_kind::constant_not_representable,
                       leaf.as_float() == leaf.as_float() ? "is an infinite float, which WGSL refuses in a constant"
                                                          : "is a NaN, which WGSL refuses in a constant");
                return judged::reported;
            }
        return judged::sound;
    }
};
} // namespace

void checker::judge_constants(flat_entry_point const& structured)
{
    auto pass = constant_pass{.c = *this,
                              .m = out,
                              .e = structured,
                              .states = cc::vector<judged>::create_filled(structured.exprs.size(), judged::unknown)};
    for (auto i = isize(0); i < structured.exprs.size(); ++i)
        (void)pass.judge(flat_expr_id(i32(i)));
}
