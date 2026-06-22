#include <clean-core/native.hh>
#include <clean-core/result.hh>
#include <clean-core/to_debug_string.hh>
#include <clean-core/to_string.hh>


struct cc::any_error::context_node
{
    cc::string message;
    cc::source_location site;
    cc::node_allocation<context_node> next;

    context_node(cc::string msg, cc::source_location s) : message(cc::move(msg)), site(s) {}
};

cc::any_error::payload::payload(cc::string msg, cc::source_location s) : message(cc::move(msg)), site(s)
{
}

cc::any_error::payload::~payload() = default;

cc::any_error::any_error(cc::string message, cc::source_location site)
  : any_error(cc::default_node_allocator(), cc::move(message), site)
{
}

cc::any_error::any_error(cc::node_allocator& alloc, cc::string message, cc::source_location site)
{
    _payload = cc::node_allocation<payload>::create_from(alloc, cc::move(message), site);
}

// must be added here because payload is only fwd declared in any_error
cc::any_error::any_error(any_error&& other) noexcept = default;
cc::any_error& cc::any_error::operator=(any_error&& other) noexcept = default;
cc::any_error::~any_error() = default;

void cc::any_error::impl_ensure_payload()
{
    if (_payload.is_valid())
        return;

    _payload = cc::node_allocation<payload>::create_from(cc::default_node_allocator(), "<empty cc::any_error>",
                                                         cc::source_location::current());
}

cc::any_error& cc::any_error::with_context(cc::string message, cc::source_location site) &
{
    this->impl_ensure_payload();

    auto new_ctx = cc::node_allocation<context_node>::create_from(cc::default_node_allocator(), cc::move(message), site);
    new_ctx->next = cc::move(_payload->ctx);
    _payload->ctx = cc::move(new_ctx);
    return *this;
}

cc::any_error cc::any_error::with_context(cc::string message, cc::source_location site) &&
{
    with_context(message, site);
    return cc::move(*this);
}

bool cc::any_error::is_empty() const
{
    return !_payload.is_valid();
}

cc::source_location cc::any_error::site() const
{
    return _payload.is_valid() ? _payload->site : cc::source_location::current();
}

bool cc::any_error::has_stacktrace() const
{
    return _payload.is_valid() && _payload->trace.is_valid();
}

cc::stacktrace const* cc::any_error::get_stacktrace() const
{
    return has_stacktrace() ? _payload->trace.ptr : nullptr;
}

cc::string cc::any_error::to_string() const
{
    cc::string result;

    // Add main error message
    if (_payload.is_valid())
    {
        result += "error: ";
        result += _payload->message;
        result += "\n";
    }
    else
    {
        result += "error: <empty cc::any_error>\n";
    }

    // Add source location
    if (_payload.is_valid())
    {
        auto const s = _payload->site;
        result += "  at ";
        result += s.file_name();
        result += ":";
        result += cc::to_string(s.line());
        result += " - ";
        result += cc::demangle_symbol(s.function_name());
        result += "\n";
    }

    // Add context chain
    if (_payload.is_valid())
    {
        auto* ctx_ptr = _payload->ctx.is_valid() ? _payload->ctx.ptr : nullptr;
        while (ctx_ptr != nullptr)
        {
            auto const s = ctx_ptr->site;
            result += "  context: ";
            result += ctx_ptr->message;
            result += " (at ";
            result += s.file_name();
            result += ":";
            result += cc::to_string(s.line());
            result += " - ";
            result += cc::demangle_symbol(s.function_name());
            result += ")\n";

            ctx_ptr = ctx_ptr->next.is_valid() ? ctx_ptr->next.ptr : nullptr;
        }
    }

    return result;
}
