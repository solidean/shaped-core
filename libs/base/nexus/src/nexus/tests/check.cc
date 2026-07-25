#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/format.hh>
#include <nexus/tests/check.hh>
#include <nexus/tests/execute.hh>

struct nx::impl::check_handle::impl_context
{
    check_kind kind;
    cmp_op op;
    cc::string expr_text;
    bool passed;
    cc::source_location location;

    // mirrors nx::impl::check_result — see execute.hh for why these are three separate fields
    bool operands_captured = false;
    cc::string lhs;
    cc::string rhs;
    cc::string diagnostic;
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
        nx::impl::report_check_result({
            .kind = ctx->kind,
            .op = ctx->op,
            .expr = cc::move(ctx->expr_text),
            .passed = ctx->passed,
            .operands_captured = ctx->operands_captured,
            .lhs = cc::move(ctx->lhs),
            .rhs = cc::move(ctx->rhs),
            .diagnostic = cc::move(ctx->diagnostic),
            .extra_lines = cc::move(ctx->extra_lines),
            .location = ctx->location,
        });
    }
}

nx::impl::check_handle nx::impl::check_handle::add_extra_line(cc::string line) &&
{
    if (!passed)
        ctx->extra_lines.push_back(cc::move(line));
    return cc::move(*this);
}

nx::impl::check_handle nx::impl::check_handle::set_operands(cc::string lhs, cc::string rhs) &&
{
    if (!passed)
    {
        ctx->operands_captured = true;
        ctx->lhs = cc::move(lhs);
        ctx->rhs = cc::move(rhs);
    }
    return cc::move(*this);
}

nx::impl::check_handle nx::impl::check_handle::set_diagnostic(cc::string text) &&
{
    if (!passed)
        ctx->diagnostic = cc::move(text);
    return cc::move(*this);
}

nx::impl::check_handle nx::impl::check_handle::context(cc::string msg) &&
{
    return cc::move(*this).add_extra_line(cc::move(msg));
}

nx::impl::check_handle nx::impl::check_handle::note(cc::string msg) &&
{
    return cc::move(*this).add_extra_line(cc::format("note: {}", msg));
}

nx::impl::check_handle nx::impl::check_handle::fail_note() &&
{
    return cc::move(*this).add_extra_line("note: test failed");
}

nx::impl::check_handle nx::impl::check_handle::fail_note(cc::string msg) &&
{
    return cc::move(*this).add_extra_line(cc::format("note: {}", msg));
}

nx::impl::check_handle nx::impl::check_handle::succeed_note() &&
{
    return cc::move(*this).add_extra_line("note: test succeeded");
}

nx::impl::check_handle nx::impl::check_handle::succeed_note(cc::string msg) &&
{
    return cc::move(*this).add_extra_line(cc::format("note: {}", msg));
}
