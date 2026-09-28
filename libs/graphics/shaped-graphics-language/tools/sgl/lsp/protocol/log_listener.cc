#include "log_listener.hh"

#include "server.hh"

#include <clean-core/record/domain.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>

using namespace cc::primitive_defines;

namespace
{
[[nodiscard]] lsp::message_type message_type_of(cc::rec::level l)
{
    switch (l)
    {
    case cc::rec::level::error:
        return lsp::message_type::error;
    case cc::rec::level::warning:
        return lsp::message_type::warning;
    case cc::rec::level::info:
        return lsp::message_type::info;
    default:
        return lsp::message_type::log;
    }
}
} // namespace

void lsp::log_listener::on_chunk(cc::rec::chunk_view const& view)
{
    auto const min_level = cc::rec::level(_min_level.load());
    for (auto it = view.begin(); it != view.end(); ++it)
    {
        auto const e = *it;
        if (e.kind() != cc::rec::event_kind::log || e.level() < min_level)
            continue;
        auto const domain = e.domain() != nullptr ? e.domain()->name() : cc::string_view("?");
        auto text = cc::format("[{}] {}", domain, e.payload.empty() ? e.name() : e.payload_as_text());
        if (_server->is_initialized())
            _server->post_log(message_type_of(e.level()), cc::move(text));
        else
            cc::eprintln("{}", text);
    }
}

cc::optional<cc::rec::level> lsp::level_of(cc::string_view name)
{
    if (name == "trace")
        return cc::rec::level::trace;
    if (name == "debug")
        return cc::rec::level::debug;
    if (name == "info")
        return cc::rec::level::info;
    if (name == "warning")
        return cc::rec::level::warning;
    if (name == "error")
        return cc::rec::level::error;
    return cc::nullopt;
}
