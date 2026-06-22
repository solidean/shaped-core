#include <nexus/tests/check.hh>
#include <nexus/tests/execute.hh>

// TODO: remove me
#include <string>
#include <vector>

// MIGRATE ME
struct nx::impl::check_handle::impl_context
{
    check_kind kind;
    cmp_op op;
    std::string expr_text;
    bool passed;
    std::source_location location;
    std::vector<std::string> extra_lines;
};

nx::impl::check_handle nx::impl::check_handle::make(check_kind kind,
                                                    cmp_op op,
                                                    char const* expr_text,
                                                    bool passed,
                                                    std::source_location loc)
{
    check_handle handle;
    handle.ctx = std::make_unique<impl_context>(impl_context{
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
        // MIGRATE ME
        nx::impl::report_check_result(ctx->kind, ctx->op, std::move(ctx->expr_text), ctx->passed,
                                      std::move(ctx->extra_lines), ctx->location);
    }
}

nx::impl::check_handle nx::impl::check_handle::add_extra_line(cc::string line) &&
{
    if (!passed)
        ctx->extra_lines.emplace_back(line.data(), line.size());
    return std::move(*this);
}

nx::impl::check_handle nx::impl::check_handle::context(cc::string msg) &&
{
    // MIGRATE ME
    return std::move(*this).add_extra_line(msg);
}

nx::impl::check_handle nx::impl::check_handle::note(cc::string msg) &&
{
    // MIGRATE ME
    return std::move(*this).add_extra_line(std::format("note: {}", msg.c_str_materialize()));
}

nx::impl::check_handle nx::impl::check_handle::fail_note() &&
{
    // MIGRATE ME
    return std::move(*this).add_extra_line("note: test failed");
}

nx::impl::check_handle nx::impl::check_handle::fail_note(cc::string msg) &&
{
    // MIGRATE ME
    return std::move(*this).add_extra_line(std::format("note: {}", msg.c_str_materialize()));
}

nx::impl::check_handle nx::impl::check_handle::succeed_note() &&
{
    // MIGRATE ME
    return std::move(*this).add_extra_line("note: test succeeded");
}

nx::impl::check_handle nx::impl::check_handle::succeed_note(cc::string msg) &&
{
    // MIGRATE ME
    return std::move(*this).add_extra_line(std::format("note: {}", msg.c_str_materialize()));
}
