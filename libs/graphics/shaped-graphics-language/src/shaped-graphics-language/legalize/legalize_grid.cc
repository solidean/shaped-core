#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/reserved_bindings.hh>
#include <shaped-graphics-language/legalize/impl/legalizer.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

// CHK-520: a bare @compute's size is the compile's, so the threads its last workgroups hold past the grid are the
// compile's too, and they return before the body; sg writes the grid at each dispatch_threads into the reserved
// `sg_dispatch_grid`, and metal dispatches exactly the grid's threads.

using namespace sgl;
using namespace sgl::check;

bool sgl::check::guards_dispatch_grid(checked_module const& m, flat_entry_point const& e)
{
    auto const binding = impl::reserved_symbol_of(m, "sg_dispatch_grid");
    for (auto const id : e.reserved)
        if (is_valid(binding) && id == binding)
            return true;
    return false;
}

flat_entry_point sgl::check::with_dispatch_grid_guard(checked_module const& m, flat_entry_point const& e)
{
    auto const binding = impl::reserved_symbol_of(m, "sg_dispatch_grid");
    // a test's own prelude declares no grid, and its tree is left as it is
    if (!is_valid(binding))
        return e;
    auto out = flat_builder::extend(m, e);
    auto id = local_id::none;
    for (auto const& input : out.e.stage_inputs)
        if (input.input == stage_input::thread_id)
            id = input.local;
    if (!is_valid(id))
    {
        id = out.add_local(local_kind::parameter, "thread_id", out.type_named("int3"));
        out.e.stage_inputs.push_back({.input = stage_input::thread_id, .local = id});
    }

    // `!(uint(id.x) < sg_dispatch_grid.threads.x) || …`, a thread id never being negative
    auto past = flat_expr_id::none;
    for (auto axis = 0; axis < 3; ++axis)
    {
        flat_expr_id const lane[] = {out.member(out.local(id), axis)};
        flat_expr_id const compared[]
            = {out.call("convert_int_to_uint", lane), out.member(out.binding_member(binding, 0), axis)};
        auto const outside = out.not_(out.call("less_uint", compared));
        past = is_valid(past) ? out.or_(past, outside) : outside;
    }
    auto ends = impl::stmt_list();
    impl::end_invocation(out, ends);
    auto body = impl::stmt_list();
    body.push_back(out.if_(past, ends));
    for (auto const s : out.e.at(out.e.body))
        body.push_back(s);
    out.set_body(body);
    out.e.reserved.push_back(binding);
    return cc::move(out.e);
}
