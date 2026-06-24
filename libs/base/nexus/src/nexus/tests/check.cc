#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <nexus/tests/check.hh>
#include <nexus/tests/execute.hh>

#include <format> // std::format: no cc::format yet

struct nx::impl::check_handle::impl_context
{
    check_kind kind;
    cmp_op op;
    cc::string expr_text;
    bool passed;
    cc::source_location location;
    cc::vector<cc::string> extra_lines;
};

nx::impl::check_handle nx::impl::check_handle::make(check_kind kind,
                                                    cmp_op op,
                                                    char const* expr_text,
                                                    bool passed,
                                                    cc::source_location loc)
{
    check_handle handle;
    handle.ctx = cc::make_unique<impl_context>(impl_context{
        .kind = kind,
        .op = op,
        .expr_text = expr_text,
        .passed = passed,
        .location = loc,
    });
    handle.passed = passed;
    return handle;
}

nx::impl::check_handle::~check_handle() noexcept(false)
{
    if (ctx)
    {
        nx::impl::report_check_result(ctx->kind, ctx->op, cc::move(ctx->expr_text), ctx->passed,
                                      cc::move(ctx->extra_lines), ctx->location);
    }
}

nx::impl::check_handle nx::impl::check_handle::add_extra_line(cc::string line) &&
{
    if (!passed)
        ctx->extra_lines.push_back(cc::move(line));
    return cc::move(*this);
}

nx::impl::check_handle nx::impl::check_handle::context(cc::string msg) &&
{
    return cc::move(*this).add_extra_line(cc::move(msg));
}

nx::impl::check_handle nx::impl::check_handle::note(cc::string msg) &&
{
    return cc::move(*this).add_extra_line(std::format("note: {}", msg.c_str_materialize()));
}

nx::impl::check_handle nx::impl::check_handle::fail_note() &&
{
    return cc::move(*this).add_extra_line("note: test failed");
}

nx::impl::check_handle nx::impl::check_handle::fail_note(cc::string msg) &&
{
    return cc::move(*this).add_extra_line(std::format("note: {}", msg.c_str_materialize()));
}

nx::impl::check_handle nx::impl::check_handle::succeed_note() &&
{
    return cc::move(*this).add_extra_line("note: test succeeded");
}

nx::impl::check_handle nx::impl::check_handle::succeed_note(cc::string msg) &&
{
    return cc::move(*this).add_extra_line(std::format("note: {}", msg.c_str_materialize()));
}
