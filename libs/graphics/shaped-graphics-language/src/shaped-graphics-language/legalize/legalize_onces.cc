#include <shaped-graphics-language/legalize/core.hh>
#include <shaped-graphics-language/legalize/impl/legalizer.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

// Rule X7 (LEGAL-62), over a core body: a `once` whose every `break` can become a branch is its body, in place.

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
struct once_dissolver
{
    flat_builder& out;

    [[nodiscard]] flat_entry_point const& e() const { return out.e; }

    /// Whether `id` holds a `break` of the `once` around it: one that no loop and no other `once` stands between.
    [[nodiscard]] bool breaks_out(flat_stmt_id id, int depth) const
    {
        if (!is_known(e(), id) || depth > k_max_depth)
            return true;
        auto const& s = e().at(id);
        if (s.node.is<flat_break>())
            return true;
        if (s.node.is<flat_loop>() || s.node.is<flat_while>() || s.node.is<flat_for>() || s.node.is<flat_once>())
            return false;
        auto breaks = false;
        for_each_body_of(e(), s,
                         [&](ast::range_of<flat_stmt_id> inner)
                         {
                             if (is_known(e(), inner))
                                 for (auto const n : e().at(inner))
                                     breaks = breaks || breaks_out(n, depth + 1);
                         });
        return breaks;
    }

    /// `list` with every `break` of its `once` written as a branch, and `tail` behind each way it runs to its end.
    /// `placed` counts how often `tail` was written; empty where a `break` stands where no branch can take its place.
    [[nodiscard]] cc::optional<stmt_list> without_breaks(cc::span<flat_stmt_id const> list,
                                                         stmt_list const& tail,
                                                         isize& placed,
                                                         int depth)
    {
        if (depth > k_max_depth)
            return cc::nullopt;
        auto result = stmt_list();
        for (auto i = isize(0); i < list.size(); ++i)
        {
            auto const id = list[i];
            if (!is_known(e(), id))
                return cc::nullopt;
            auto const& s = e().at(id);
            // what follows a `break` is never reached, and the `break` itself leaves the `once`
            if (s.node.is<flat_break>())
                return result;
            // nor is what follows any other exit, which stays
            if (s.node.is<flat_return>() || s.node.is<flat_continue>() || s.node.is<flat_discard>())
            {
                result.push_back(id);
                return result;
            }
            if (!breaks_out(id, 0))
            {
                result.push_back(id);
                continue;
            }
            // a `break` inside anything but an `if`, such as a `switch` arm, keeps its `once`
            auto const* const branch = s.node.try_as<flat_if>();
            if (branch == nullptr)
                return cc::nullopt;

            // What follows runs where neither side left, so it is written into each side that runs to its end.
            auto rest = stmt_list();
            for (auto k = i + 1; k < list.size(); ++k)
                rest.push_back(list[k]);
            auto rest_placed = isize(0);
            auto const continuation = without_breaks(rest, tail, rest_placed, depth + 1);
            if (!continuation.has_value())
                return cc::nullopt;

            auto then_body = stmt_list();
            auto else_body = stmt_list();
            if (is_known(e(), branch->then_body))
                then_body.push_back_range(e().at(branch->then_body));
            if (is_known(e(), branch->else_body))
                else_body.push_back_range(e().at(branch->else_body));
            auto then_placed = isize(0);
            auto else_placed = isize(0);
            auto const then_side = without_breaks(then_body, continuation.value(), then_placed, depth + 1);
            auto const else_side = without_breaks(else_body, continuation.value(), else_placed, depth + 1);
            // Both sides reaching what follows would write it twice, which this rule never does.
            if (!then_side.has_value() || !else_side.has_value()
                || (!continuation.value().empty() && then_placed + else_placed > 1))
                return cc::nullopt;
            placed += (then_placed + else_placed) * rest_placed;

            auto copy = s;
            auto& rewritten = copy.node.as<flat_if>();
            if (then_side.value().empty() && !else_side.value().empty())
            {
                rewritten.condition = negated(out, rewritten.condition);
                rewritten.then_body = out.stmt_list(else_side.value());
                rewritten.else_body = out.stmt_list(stmt_list());
            }
            else
            {
                rewritten.then_body = out.stmt_list(then_side.value());
                rewritten.else_body = out.stmt_list(else_side.value());
            }
            out.e.stmts.push_back(cc::move(copy));
            result.push_back(flat_stmt_id(out.e.stmts.size() - 1));
            return result;
        }
        result.push_back_range(tail);
        ++placed;
        return result;
    }

    ast::range_of<flat_stmt_id> dissolved_body(ast::range_of<flat_stmt_id> range, int depth)
    {
        auto list = stmt_list();
        if (is_known(e(), range))
            list.push_back_range(e().at(range));
        return out.stmt_list(dissolved(list, depth + 1));
    }

    /// `list` with every `once` the rule takes written as its body, at any depth.
    stmt_list dissolved(stmt_list const& list, int depth)
    {
        auto result = stmt_list();
        for (auto const id : list)
        {
            if (!is_known(e(), id) || depth > k_max_depth)
            {
                result.push_back(id);
                continue;
            }
            auto copy = e().at(id);
            if (auto* const once = copy.node.try_as<flat_once>())
            {
                // inner ones first, so an outer `once` meets only the breaks that are its own
                once->body = dissolved_body(once->body, depth);
                auto body = stmt_list();
                body.push_back_range(e().at(once->body));
                auto placed = isize(0);
                if (auto const flat = without_breaks(body, stmt_list(), placed, 0); flat.has_value())
                {
                    result.push_back_range(flat.value());
                    continue;
                }
            }
            else if (auto* const i = copy.node.try_as<flat_if>())
            {
                i->then_body = dissolved_body(i->then_body, depth);
                i->else_body = dissolved_body(i->else_body, depth);
            }
            else if (auto* const l = copy.node.try_as<flat_loop>())
                l->body = dissolved_body(l->body, depth);
            else if (auto* const w = copy.node.try_as<flat_while>())
                w->body = dissolved_body(w->body, depth);
            else if (auto* const f = copy.node.try_as<flat_for>())
                f->body = dissolved_body(f->body, depth);
            else if (auto* const sw = copy.node.try_as<flat_switch>())
            {
                auto arms = cc::vector<flat_arm>();
                if (is_known(e(), sw->arms))
                    arms.push_back_range(e().at(sw->arms));
                for (auto& arm : arms)
                    arm.body = dissolved_body(arm.body, depth);
                sw->arms = out.arm_list(arms);
                sw->default_body = dissolved_body(sw->default_body, depth);
            }
            else
            {
                result.push_back(id);
                continue;
            }
            out.e.stmts.push_back(cc::move(copy));
            result.push_back(flat_stmt_id(out.e.stmts.size() - 1));
        }
        return result;
    }
};
} // namespace

stmt_list sgl::check::impl::dissolve_onces(flat_builder& out, stmt_list const& body)
{
    auto d = once_dissolver{.out = out};
    return d.dissolved(body, 0);
}
