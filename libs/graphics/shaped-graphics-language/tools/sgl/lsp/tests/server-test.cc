#include "lsp-test-support.hh"

using namespace lsp_test;

namespace
{
struct echo_params
{
    cc::string text;
};

[[nodiscard]] bool read(lsp::json::ref in, echo_params& out)
{
    return lsp::json::read(in["text"], out.text);
}

struct echo_result
{
    cc::string text;
};

void write_fields(lsp::json::writer& w, echo_result const& v)
{
    lsp::json::put(w, "text", v.text);
}
} // namespace

TEST("lsp server - initialize answers the capabilities, and a request before it is refused", main_thread)
{
    auto s = lsp::server({
        .name = "test",
        .version = "1",
        .write_capabilities = [](lsp::json::writer& w) { w.write("hoverProvider", false); },
    });
    s.receive(request(1, "test/echo", R"({"text":"x"})"));
    s.receive(request(2, "initialize", R"({"capabilities":{"general":{"positionEncodings":["utf-16","utf-8"]}}})"));
    auto const out = outgoing(s);

    CHECK(response_to(out, 1)["error"]["code"].as_double() == lsp::error_code::server_not_initialized);
    auto const caps = response_to(out, 2)["result"]["capabilities"];
    CHECK(caps["positionEncoding"].as_string() == "utf-8");
    CHECK(caps["textDocumentSync"]["change"].as_double() == 2);
    CHECK(caps["hoverProvider"].is_bool());
    CHECK(response_to(out, 2)["result"]["serverInfo"]["name"].as_string() == "test");
    CHECK(s.encoding() == lsp::position_encoding::utf8);
}

TEST("lsp server - a request is answered from compute, an unknown one is not found, and bad params are refused",
     main_thread)
{
    auto s = lsp::server({.name = "test", .version = "1"});
    s.on_request<echo_params>(
        "test/echo",
        [](lsp::request_context const&, echo_params p)
        {
            return cc::make_async_lazy([p = cc::move(p)]
                                       { return lsp::answer<echo_result>(echo_result{.text = p.text + "!"}); });
        });
    initialize(s);
    (void)outgoing(s);

    s.receive(request(5, "test/echo", R"({"text":"hi"})"));
    s.receive(request(6, "test/nope"));
    s.receive(request(7, "test/echo", R"({"text":3})"));
    settle(s);
    auto const out = outgoing(s);
    CHECK(response_to(out, 5)["result"]["text"].as_string() == "hi!");
    CHECK(response_to(out, 6)["error"]["code"].as_double() == lsp::error_code::method_not_found);
    CHECK(response_to(out, 7)["error"]["code"].as_double() == lsp::error_code::invalid_params);
}

TEST("lsp server - a handler's error is the response's error, and a cancelled request says so", main_thread)
{
    auto s = lsp::server({.name = "test", .version = "1"});
    s.on_request<echo_params>(
        "test/fail",
        [](lsp::request_context const&, echo_params const&)
        {
            return cc::make_async_from_value(lsp::answer<echo_result>(
                cc::error(lsp::response_error{.code = lsp::error_code::content_modified, .message = "moved on"})));
        });
    // answers only once it sees its flag raised, which `$/cancelRequest` does
    s.on_request<echo_params>(
        "test/wait",
        [](lsp::request_context const& ctx, echo_params const&) -> cc::shared_async<lsp::answer<echo_result>>
        {
            auto const flag = ctx.flag;
            return cc::make_async_lazy<lsp::answer<echo_result>>(
                [flag](cc::async_context<lsp::answer<echo_result>>& actx) -> cc::async_step_status
                {
                    if (!flag->is_raised.load())
                        return actx.yield();
                    return actx.success(lsp::answer<echo_result>(cc::error(
                        lsp::response_error{.code = lsp::error_code::request_cancelled, .message = "cancelled"})));
                });
        });
    initialize(s);
    (void)outgoing(s);

    s.receive(request(8, "test/fail", R"({"text":""})"));
    s.receive(request(9, "test/wait", R"({"text":""})"));
    s.receive(notification("$/cancelRequest", R"({"id":9})"));
    settle(s);
    auto const out = outgoing(s);
    CHECK(response_to(out, 8)["error"]["code"].as_double() == lsp::error_code::content_modified);
    CHECK(response_to(out, 8)["error"]["message"].as_string() == "moved on");
    CHECK(response_to(out, 9)["error"]["code"].as_double() == lsp::error_code::request_cancelled);
}

TEST("lsp server - document notifications change the workspace in order, before a later request is answered", main_thread)
{
    auto s = lsp::server({.name = "test", .version = "1"});
    auto seen = cc::vector<cc::string>();
    s.on_document([&](cc::string_view uri)
                  { seen.push_back(cc::format("{} v{}", uri, s.workspace().snapshot().version_of(uri))); });
    initialize(s);

    s.receive(notification("textDocument/didOpen",
                           R"({"textDocument":{"uri":"file:///a.sgl","languageId":"sgl","version":1,"text":"ab\ncd"}})"));
    s.receive(notification(
        "textDocument/didChange",
        R"({"textDocument":{"uri":"file:///a.sgl","version":2},"contentChanges":[{"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":1}},"text":"X"}]})"));
    CHECK(s.workspace().find("file:///a.sgl")->text == "ab\nXd");
    s.receive(notification("textDocument/didClose", R"({"textDocument":{"uri":"file:///a.sgl"}})"));
    CHECK(s.workspace().find("file:///a.sgl") == nullptr);
    CHECK(seen.size() == 3);
    CHECK(seen[0] == "file:///a.sgl v1");
    CHECK(seen[1] == "file:///a.sgl v2");
    CHECK(seen[2] == "file:///a.sgl v-1");
}

TEST("lsp server - shutdown then exit is a clean exit, exit alone is not, and logs wait for initialize", main_thread)
{
    auto s = lsp::server({.name = "test", .version = "1"});
    s.post_log(lsp::message_type::info, "early");
    s.poll();
    CHECK(s.take_outgoing().empty());

    initialize(s);
    auto const messages = outgoing(s);
    auto const logs = notifications_of(messages, "window/logMessage");
    REQUIRE(logs.size() == 1);
    CHECK(logs[0]["message"].as_string() == "early");

    s.receive(request(3, "shutdown"));
    auto const after_shutdown = outgoing(s);
    CHECK(response_to(after_shutdown, 3)["result"].is_null());
    s.receive(notification("exit"));
    CHECK(s.has_exited());
    CHECK(s.exit_code() == 0);

    auto rude = lsp::server({.name = "test", .version = "1"});
    initialize(rude);
    rude.receive(notification("exit"));
    CHECK(rude.exit_code() == 1);
}

namespace
{
/// The one response among `messages` whose id is null; an invalid ref when there is none or several.
babel::json::ref response_to_null(cc::vector<babel::json::document> const& messages)
{
    auto found = babel::json::ref();
    auto count = 0;
    for (auto const& m : messages)
        if (m.root()["id"].is_null() && !m.root().has("method"))
        {
            found = m.root();
            ++count;
        }
    return count == 1 ? found : babel::json::ref();
}
} // namespace

TEST("lsp server - an id that is no exact integer is refused with a null id, never converted", main_thread)
{
    auto s = lsp::server({.name = "test", .version = "1"});
    initialize(s);
    (void)outgoing(s);

    for (auto const id : {"1.5", "1e300", "-1e19", "{}", "true"})
    {
        s.receive(cc::format(R"({{"jsonrpc":"2.0","id":{},"method":"shutdown"}})", id));
        auto const out = outgoing(s);
        REQUIRE(out.size() == 1);
        CHECK(response_to_null(out)["error"]["code"].as_double() == lsp::error_code::invalid_request);
    }
    CHECK(s.exit_code() == 1); // none of them shut the server down

    // a string id and a large exact integer are echoed as they came
    s.receive(R"({"jsonrpc":"2.0","id":"a\"b","method":"test/nope"})");
    s.receive(R"({"jsonrpc":"2.0","id":4294967296,"method":"test/nope"})");
    auto const out = outgoing(s);
    REQUIRE(out.size() == 2);
    CHECK(out[0].root()["id"].as_string() == "a\"b");
    CHECK(out[1].root()["id"].as_double() == 4294967296.0);
}

TEST("lsp server - a document notification that does not read is dropped with a warning naming it", main_thread)
{
    auto s = lsp::server({.name = "test", .version = "1"});
    initialize(s);

    nx::expect_warning("textDocument/didOpen for file:///a.sgl", nx::exactly(1));
    s.receive(notification("textDocument/didOpen",
                           R"({"textDocument":{"uri":"file:///a.sgl","languageId":"sgl","version":1.5,"text":""}})"));
    CHECK(s.workspace().find("file:///a.sgl") == nullptr);

    nx::expect_warning("textDocument/didClose: params that do not read", nx::exactly(1));
    s.receive(notification("textDocument/didClose", R"({"textDocument":{}})"));

    // a null range is the whole text, not a change that fails to read
    s.receive(notification("textDocument/didOpen",
                           R"({"textDocument":{"uri":"file:///b.sgl","languageId":"sgl","version":1,"text":"old"}})"));
    s.receive(notification(
        "textDocument/didChange",
        R"({"textDocument":{"uri":"file:///b.sgl","version":2},"contentChanges":[{"range":null,"text":"new"}]})"));
    CHECK(s.workspace().find("file:///b.sgl")->text == "new");
    CHECK(s.workspace().find("file:///b.sgl")->version == 2);
}

TEST("lsp server - after shutdown every request is invalid, and a second initialize changes nothing", main_thread)
{
    auto s = lsp::server({.name = "test", .version = "1"});
    s.receive(request(1, "initialize", R"({"capabilities":{},"clientInfo":{"name":"first"}})"));
    s.receive(request(2, "initialize",
                      R"({"capabilities":{"general":{"positionEncodings":["utf-8"]}},"clientInfo":{"name":"second"}})"));
    auto const init = outgoing(s);
    CHECK(response_to(init, 1)["result"]["capabilities"]["positionEncoding"].as_string() == "utf-16");
    CHECK(response_to(init, 2)["error"]["code"].as_double() == lsp::error_code::invalid_request);
    CHECK(s.initialize_params()["clientInfo"]["name"].as_string() == "first");
    CHECK(s.encoding() == lsp::position_encoding::utf16);

    // the hook runs once, for the shutdown that is answered
    auto shutdowns = 0;
    s.on_shutdown([&] { ++shutdowns; });
    s.receive(request(3, "shutdown"));
    s.receive(request(4, "shutdown"));
    s.receive(request(5, "test/nope"));
    CHECK(shutdowns == 1);
    auto const out = outgoing(s);
    CHECK(response_to(out, 3)["result"].is_null());
    CHECK(response_to(out, 4)["error"]["code"].as_double() == lsp::error_code::invalid_request);
    CHECK(response_to(out, 5)["error"]["code"].as_double() == lsp::error_code::invalid_request);

    s.receive(notification("exit"));
    CHECK(s.exit_code() == 0);
}

TEST("lsp server - a message with an id but no method, and a batch, are invalid requests; a response is not", main_thread)
{
    auto s = lsp::server({.name = "test", .version = "1"});
    initialize(s);
    (void)outgoing(s);

    s.receive(R"({"jsonrpc":"2.0","id":11})");
    s.receive(R"({"jsonrpc":"2.0","id":12,"method":7})");
    // responses to requests of ours, which are dropped
    s.receive(R"({"jsonrpc":"2.0","id":13,"result":null})");
    s.receive(R"({"jsonrpc":"2.0","id":14,"error":{"code":1,"message":"x"}})");
    auto const out = outgoing(s);
    REQUIRE(out.size() == 2);
    CHECK(response_to(out, 11)["error"]["code"].as_double() == lsp::error_code::invalid_request);
    CHECK(response_to(out, 12)["error"]["code"].as_double() == lsp::error_code::invalid_request);

    s.receive(R"([{"jsonrpc":"2.0","id":15,"method":"shutdown"}])");
    auto const batch = outgoing(s);
    REQUIRE(batch.size() == 1);
    CHECK(response_to_null(batch)["error"]["code"].as_double() == lsp::error_code::invalid_request);
    CHECK(s.exit_code() == 1);
}

TEST("lsp server - a request of the server's own carries a plain integer id", main_thread)
{
    auto s = lsp::server({.name = "test", .version = "1"});
    initialize(s);
    (void)outgoing(s);

    s.request("workspace/inlayHint/refresh", lsp::json::null_t());
    s.request("workspace/inlayHint/refresh", lsp::json::null_t());
    auto const out = outgoing(s);
    REQUIRE(out.size() == 2);
    CHECK(out[0].root()["id"].is_number());
    CHECK(out[1].root()["id"].as_double() == out[0].root()["id"].as_double() + 1);
}
