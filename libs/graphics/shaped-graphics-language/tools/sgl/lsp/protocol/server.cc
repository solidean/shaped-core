#include "server.hh"

using namespace cc::primitive_defines;

lsp::server::server(options opts) : _options(cc::move(opts))
{
}

namespace
{
/// A request id as JSON text, so it is echoed exactly: `7`, or `"abc"` with its escapes.
[[nodiscard]] cc::string id_text_of(lsp::json::ref id)
{
    if (id.is_string())
        return lsp::json::to_text(cc::string(id.as_string()));
    if (id.is_number())
        return cc::format("{}", i64(id.as_double()));
    return "null";
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
    auto const method = cc::string(root["method"].as_string());
    if (method.empty())
        return; // a response to a request of ours, none of which waits for its answer
    if (!root.has("id"))
    {
        impl_handle_notification(method, root["params"]);
        return;
    }

    auto id = id_text_of(root["id"]);
    if (method == "initialize")
    {
        // kept whole, since what the client said about itself is read later
        _initialize_document = cc::move(doc);
        impl_initialize(id, _initialize_document.value().root()["params"]);
        return;
    }
    impl_handle_request(cc::move(id), method, root["params"]);
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
    if (method == "shutdown")
    {
        _has_shut_down = true;
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
    _pending.push_back({.id = cc::move(id), .method = cc::string(method), .reply = r.value(), .flag = ctx.flag});
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
            if (p.id == id)
                p.flag->is_raised.store(true);
        return;
    }
    if (method == "textDocument/didOpen")
    {
        auto p = did_open_params();
        if (read(params, p))
        {
            _workspace.open(p.text_document);
            if (_on_document)
                _on_document(p.text_document.uri);
        }
        return;
    }
    if (method == "textDocument/didChange")
    {
        auto p = did_change_params();
        if (read(params, p) && _workspace.change(p, _encoding) && _on_document)
            _on_document(p.uri);
        return;
    }
    if (method == "textDocument/didClose")
    {
        auto p = did_close_params();
        if (read(params, p))
        {
            _workspace.close(p.uri);
            if (_on_document)
                _on_document(p.uri);
        }
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
        if (!p.reply->is_ready())
        {
            ++i;
            continue;
        }
        if (auto const* r = p.reply->try_value())
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
}

lsp::json::ref lsp::server::initialize_params() const
{
    return _initialize_document.has_value() ? _initialize_document.value().root()["params"] : json::ref();
}
