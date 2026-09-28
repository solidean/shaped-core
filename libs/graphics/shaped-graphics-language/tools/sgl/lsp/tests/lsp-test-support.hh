#pragma once

#include "../protocol/server.hh"

#include <clean-core/string/format.hh>
#include <clean-core/thread/thread_bound_scheduler.hh>
#include <nexus/test.hh>

namespace lsp_test
{
using namespace cc::primitive_defines;

/// Drives `s` until nothing is in flight: its asyncs run on compute, which only this loop steps without threads.
/// Waits for the condition however long a slow build takes; a server that never settles is the watchdog's to fail.
inline void settle(lsp::server& s)
{
    while (!s.is_idle())
    {
        s.poll();
        cc::pump_main_thread(1.0);
    }
    s.poll();
}

/// A request message, as a client sends it.
inline cc::string request(i32 id, cc::string_view method, cc::string_view params = "{}")
{
    return cc::format(R"({{"jsonrpc":"2.0","id":{},"method":"{}","params":{}}})", id, method, params);
}

inline cc::string notification(cc::string_view method, cc::string_view params = "{}")
{
    return cc::format(R"({{"jsonrpc":"2.0","method":"{}","params":{}}})", method, params);
}

/// The `initialize` request and the `initialized` notification, with UTF-8 offered or not.
inline void initialize(lsp::server& s, bool offers_utf8 = false)
{
    s.receive(request(0, "initialize",
                      offers_utf8 ? R"({"capabilities":{"general":{"positionEncodings":["utf-8","utf-16"]}}})"
                                  : R"({"capabilities":{}})"));
    s.receive(notification("initialized"));
    settle(s);
}

/// Every outgoing message parsed, oldest first.
inline cc::vector<babel::json::document> outgoing(lsp::server& s)
{
    auto out = cc::vector<babel::json::document>();
    for (auto const& m : s.take_outgoing())
    {
        auto doc = babel::json::read(m);
        REQUIRE(doc.has_value());
        out.push_back(cc::move(doc.value()));
    }
    return out;
}

/// The response to request `id` among `messages`, which must outlive the ref; an invalid ref when there is none.
inline babel::json::ref response_to(cc::vector<babel::json::document> const& messages, i32 id)
{
    for (auto const& m : messages)
        if (m.root()["id"].is_number() && i32(m.root()["id"].as_double()) == id && !m.root().has("method"))
            return m.root();
    return {};
}

/// Every notification of `method` among `messages`, as its params; `messages` must outlive the refs.
inline cc::vector<babel::json::ref> notifications_of(cc::vector<babel::json::document> const& messages,
                                                     cc::string_view method)
{
    auto out = cc::vector<babel::json::ref>();
    for (auto const& m : messages)
        if (m.root()["method"].as_string() == method && !m.root().has("id"))
            out.push_back(m.root()["params"]);
    return out;
}
} // namespace lsp_test
