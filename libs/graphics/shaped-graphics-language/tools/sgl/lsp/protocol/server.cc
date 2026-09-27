#include "server.hh"

using namespace cc::primitive_defines;

lsp::server::server(options opts) : _options(cc::move(opts))
{
}

namespace
{
/// A request id as JSON text, so it is echoed exactly: `7`, `"abc"` with its escapes, or `null`.
/// Nothing for an id that cannot be echoed exactly, such as `1.5`, `1e300` or an object.
[[nodiscard]] cc::optional<cc::string> id_text_of(lsp::json::ref id)
{
    if (id.is_string())
        return lsp::json::to_text(cc::string(id.as_string()));
    if (auto n = i64(0); lsp::json::read(id, n))
        return cc::format("{}", n);
    if (id.is_null())
        return cc::string("null");
    return cc::nullopt;
}

[[nodiscard]] lsp::impl::reply invalid_request(cc::string text)
{
    return {.is_error = true, .error_code = lsp::error_code::invalid_request, .text = cc::move(text)};
}

/// Warns that a document notification was dropped, naming its document when that much reads.
void warn_unread(cc::string_view method, lsp::json::ref params)
{
    auto const uri = params["textDocument"]["uri"].as_string();
    if (uri.empty())
        CC_LOG_WARNING("{}: params that do not read, dropped", method);
    else
        CC_LOG_WARNING("{} for {}: params that do not read, dropped", method, uri);
}
} // namespace

void lsp::server::receive(cc::string_view message)
{
    auto parsed = babel::json::read(message);
    if (!parsed.has_value())
    {
        CC_LOG_WARNING("a message that is no JSON: {}", parsed.error().to_string());
        impl_respond("null", {.is_error = true, .error_code = error_code::parse_error, .text = "the message is no JSON"});
        return;
    }
    auto doc = cc::move(parsed.value());
    auto const root = doc.root();
    if (!root.is_object())
    {
        // a batch array among them, which LSP does not use
        impl_respond("null", invalid_request("a message must be one object"));
        return;
    }
    auto const has_method = root["method"].is_string();
    if (!has_method && (root.has("result") || root.has("error")))
        return; // a response to a request of ours, none of which waits for its answer

    auto const id = root.has("id") ? id_text_of(root["id"]) : cc::optional<cc::string>();
    if (root.has("id") && !id.has_value())
    {
        impl_respond("null", invalid_request("an id must be a string or an integer"));
        return;
    }
    if (!has_method)
    {
        auto const echoed = id.has_value() ? cc::string_view(id.value()) : cc::string_view("null");
        impl_respond(echoed, invalid_request("a message names no method"));
        return;
    }
    auto const method = cc::string(root["method"].as_string());
    if (!id.has_value())
    {
        impl_handle_notification(method, root["params"]);
        return;
    }

    if (method == "initialize")
    {
        if (_initialize_document.has_value())
        {
            impl_respond(id.value(), invalid_request("initialize was sent before"));
            return;
        }
        // kept whole, since what the client said about itself is read later
        _initialize_document = cc::move(doc);
        impl_initialize(id.value(), _initialize_document.value().root()["params"]);
        return;
    }
    impl_handle_request(id.value(), method, root["params"]);
}

void lsp::server::impl_initialize(cc::string const& id, json::ref params)
{
    auto const encodings = params["capabilities"]["general"]["positionEncodings"];
    for (auto i = isize(0); i < encodings.size(); ++i)
        if (encodings[i].as_string() == "utf-8")
            _encoding = position_encoding::utf8;

    auto w = babel::json::string_writer();
    auto& j = w.underlying();
    j.begin_object();
    j.begin_object("capabilities");
    j.write("positionEncoding", _encoding == position_encoding::utf8 ? "utf-8" : "utf-16");
    j.begin_object("textDocumentSync");
    j.write("openClose", true);
    j.write("change", 2); // incremental
    j.end_object();
    if (_options.write_capabilities)
        _options.write_capabilities(j);
    j.end_object();
    j.begin_object("serverInfo");
    j.write("name", cc::string_view(_options.name));
    j.write("version", cc::string_view(_options.version));
    j.end_object();
    j.end_object();
    auto text = w.finish();
    impl_respond(id, {.text = text.has_value() ? cc::move(text.value()) : cc::string("{}")});
    _is_initialized.store(true);
}

void lsp::server::impl_handle_request(cc::string id, cc::string_view method, json::ref params)
{
    if (!is_initialized())
    {
        impl_respond(id, {.is_error = true, .error_code = error_code::server_not_initialized, .text = "not initialized"});
        return;
    }
    if (_has_shut_down)
    {
        impl_respond(id, invalid_request("the server was shut down"));
        return;
    }
    if (method == "shutdown")
    {
        _has_shut_down = true;
        if (_on_shutdown)
            _on_shutdown();
        impl_respond(id, {.text = "null"});
        return;
    }
    auto* const handler = _requests.get_ptr(method);
    if (handler == nullptr)
    {
        impl_respond(
            id, {.is_error = true, .error_code = error_code::method_not_found, .text = cc::format("no {}", method)});
        return;
    }

    auto const ctx = request_context{.flag = cc::make_shared<cancel_flag>()};
    auto r = (*handler)(ctx, params);
    if (!r.has_value())
    {
        impl_respond(id, {.is_error = true, .error_code = error_code::invalid_params, .text = "params that do not read"});
        return;
    }
    cc::async_start(r.value());
    _wakers.push_back(cc::async_start(impl::notify_when_settled(r.value())));
    _pending.push_back({.id = cc::move(id), .method = cc::string(method), .outcome = r.value(), .flag = ctx.flag});
}

void lsp::server::impl_handle_notification(cc::string_view method, json::ref params)
{
    if (method == "exit")
    {
        _has_exited = true;
        return;
    }
    if (!is_initialized())
        return; // LSP drops every notification but `exit` before `initialize`

    if (method == "initialized")
    {
        if (_on_initialized)
            _on_initialized();
        return;
    }
    if (method == "$/cancelRequest")
    {
        auto const id = id_text_of(params["id"]);
        for (auto& p : _pending)
            if (id.has_value() && p.id == id.value())
                p.flag->is_raised.store(true);
        return;
    }
    if (method == "textDocument/didOpen")
    {
        auto p = did_open_params();
        if (!read(params, p))
        {
            warn_unread(method, params);
            return;
        }
        _workspace.open(p.text_document);
        if (_on_document)
            _on_document(p.text_document.uri);
        return;
    }
    if (method == "textDocument/didChange")
    {
        auto p = did_change_params();
        if (!read(params, p))
        {
            warn_unread(method, params);
            return;
        }
        if (_workspace.change(p, _encoding) && _on_document)
            _on_document(p.uri);
        return;
    }
    if (method == "textDocument/didClose")
    {
        auto p = did_close_params();
        if (!read(params, p))
        {
            warn_unread(method, params);
            return;
        }
        _workspace.close(p.uri);
        if (_on_document)
            _on_document(p.uri);
        return;
    }

    if (auto* const handler = _notifications.get_ptr(method))
        (*handler)(params);
    else if (!method.starts_with("$/"))
        CC_LOG_DEBUG("{}: no handler, dropped", method);
}

void lsp::server::impl_respond(cc::string_view id, impl::reply const& r)
{
    auto w = babel::json::string_writer();
    auto& j = w.underlying();
    j.begin_object();
    j.write("jsonrpc", "2.0");
    j.write_raw("id", id);
    if (r.is_error)
    {
        j.begin_object("error");
        j.write("code", r.error_code);
        j.write("message", cc::string_view(r.text));
        j.end_object();
    }
    else
        j.write_raw("result", r.text);
    j.end_object();
    if (auto text = w.finish(); text.has_value())
        impl_send(cc::move(text.value()));
}

bool lsp::server::poll()
{
    auto progress = false;

    for (auto i = isize(0); i < _pending.size();)
    {
        auto const& p = _pending[i];
        if (!p.outcome->is_ready())
        {
            ++i;
            continue;
        }
        if (auto const* r = p.outcome->try_value())
            impl_respond(p.id, *r);
        else
            impl_respond(p.id, {.is_error = true, .error_code = error_code::internal_error, .text = "no answer"});
        _pending.remove_at(i);
        progress = true;
    }

    // taken out before any fires, since a callback may watch something new
    auto ready = cc::vector<impl::watch>();
    for (auto i = isize(0); i < _watches.size();)
    {
        if (_watches[i].is_ready())
        {
            ready.push_back(cc::move(_watches[i]));
            _watches.remove_at(i);
        }
        else
            ++i;
    }
    for (auto& w : ready)
        w.fire();
    progress = progress || !ready.empty();
    _wakers.remove_all_where([](cc::shared_async<int> const& w) { return w->is_ready(); });

    if (is_initialized())
    {
        auto logs = _logs.lock([](cc::vector<log_message_params>& l) { return cc::move(l); });
        for (auto const& l : logs)
            notify("window/logMessage", l);
        progress = progress || !logs.empty();
    }
    return progress;
}

cc::vector<cc::string> lsp::server::take_outgoing()
{
    return cc::move(_outgoing);
}

void lsp::server::post_log(message_type type, cc::string message)
{
    _logs.lock([&](cc::vector<log_message_params>& l) { l.push_back({.type = type, .message = cc::move(message)}); });
    cc::thread_pump_notify();
}

lsp::json::ref lsp::server::initialize_params() const
{
    return _initialize_document.has_value() ? _initialize_document.value().root()["params"] : json::ref();
}
