#include "../server.hh"
#include "lsp-test-support.hh"

#include <clean-core/string/uri.hh>
#include <shaped-graphics-language/driver/prelude.hh>

using namespace lsp_test;

namespace
{
/// A language server, initialized; with `source`, open as `uri` at version 1 and settled: checked, published, tests run.
struct session
{
    cc::unique_ptr<sgl_lsp::language_server> ls = sgl_lsp::language_server::create();
    cc::vector<babel::json::document> messages;

    session() { start(false); }

    explicit session(cc::string_view source, bool offers_utf8 = false, cc::string_view uri = "file:///t.sgl")
    {
        start(offers_utf8);
        send_open(source, uri);
        settle();
    }

    void start(bool offers_utf8)
    {
        initialize(ls->protocol(), offers_utf8);
        (void)outgoing(ls->protocol());
    }

    /// `didOpen`, not yet settled.
    void send_open(cc::string_view source, cc::string_view uri = "file:///t.sgl")
    {
        auto w = babel::json::string_writer();
        {
            auto o = w.object();
            auto d = o.write_object("textDocument");
            d.write("uri", uri);
            d.write("languageId", "sgl");
            d.write("version", 1);
            d.write("text", source);
        }
        ls->protocol().receive(notification("textDocument/didOpen", w.finish().value()));
    }

    /// `didChange` replacing the whole text, not yet settled.
    void send_change(cc::string_view source, i32 version)
    {
        auto w = babel::json::string_writer();
        {
            auto o = w.object();
            {
                auto d = o.write_object("textDocument");
                d.write("uri", "file:///t.sgl");
                d.write("version", version);
            }
            auto changes = o.write_array("contentChanges");
            auto c = changes.write_object();
            c.write("text", source);
        }
        ls->protocol().receive(notification("textDocument/didChange", w.finish().value()));
    }

    void send_close()
    {
        ls->protocol().receive(notification("textDocument/didClose", R"({"textDocument":{"uri":"file:///t.sgl"}})"));
    }

    void settle()
    {
        lsp_test::settle(ls->protocol());
        collect();
    }

    /// Drives the server until it sent `method` at least once.
    void settle_until_sent(cc::string_view method)
    {
        collect();
        while (notifications_of(messages, method).empty())
        {
            if (ls->protocol().is_idle())
                FAIL("the server settled without sending it");
            ls->protocol().poll();
            cc::pump_main_thread(1.0);
            collect();
        }
    }

    void collect()
    {
        for (auto& m : outgoing(ls->protocol()))
            messages.push_back(cc::move(m));
    }

    /// The params of the last notification of `method`.
    babel::json::ref last(cc::string_view method) const
    {
        auto const all = notifications_of(messages, method);
        REQUIRE(!all.empty());
        return all.back();
    }

    /// The response to a request sent now, settled.
    babel::json::ref ask(i32 id, cc::string_view method, cc::string_view params)
    {
        ls->protocol().receive(request(id, method, params));
        settle();
        auto const r = response_to(messages, id);
        REQUIRE(r.is_valid());
        return r;
    }
};

/// Every diagnostic of `params` as `line:character code message`, one per line.
cc::string diagnostics_text(babel::json::ref params)
{
    auto out = cc::string();
    auto const ds = params["diagnostics"];
    for (auto i = isize(0); i < ds.size(); ++i)
        out.appendf("{}:{} {} {}\n", i32(ds[i]["range"]["start"]["line"].as_double()),
                    i32(ds[i]["range"]["start"]["character"].as_double()), ds[i]["code"].as_string(),
                    ds[i]["message"].as_string());
    return out;
}
} // namespace

TEST("sgl lsp - opening a document publishes its diagnostics, with a summary where the check has no detail", main_thread)
{
    auto s = session("fun f() -> float:\n    return nope\n");
    auto const p = s.last("textDocument/publishDiagnostics");
    CHECK(p["uri"].as_string() == "file:///t.sgl");
    CHECK(p["version"].as_double() == 1);
    CHECK(diagnostics_text(p).starts_with("1:11 unknown-name"));

    auto clean = session("fun f() -> float => 1.0\n");
    CHECK(diagnostics_text(clean.last("textDocument/publishDiagnostics")) == "");
}

TEST("sgl lsp - a file of the prelude is checked in its own place, not behind a second copy of itself", main_thread)
{
    auto const dir = cc::string(sgl::impl::prelude_directory());
    auto const uri_of = [&](cc::string_view name)
    {
        return cc::string(dir.starts_with("/") ? "file://" : "file:///")
             + cc::percent_encode(dir + "/" + name, cc::uri_component::path);
    };
    auto const builtins = sgl::prelude_files()[0].source;
    auto own = session(builtins, false, uri_of("builtins.sgl"));
    CHECK(diagnostics_text(own.last("textDocument/publishDiagnostics")) == "");
    // the same text anywhere else declares every builtin type a second time, a user's `prelude/` folder included
    auto elsewhere = session(builtins, false, "file:///c%3A/sgl/copy/builtins.sgl");
    CHECK(diagnostics_text(elsewhere.last("textDocument/publishDiagnostics")) != "");
    auto users = session(builtins, false, "file:///c%3A/shaders/prelude/builtins.sgl");
    CHECK(diagnostics_text(users.last("textDocument/publishDiagnostics")) != "");

    // an edit of core.sgl is checked as the prelude, so its error is reported in it
    auto core = session(cc::string(sgl::prelude_files()[1].source) + "fun broken() => nope\n", false, uri_of("core.sgl"));
    CHECK(diagnostics_text(core.last("textDocument/publishDiagnostics")).contains("unknown-name"));
}

TEST("sgl lsp - a failing test is a diagnostic, and every check is a mark with its counts", main_thread)
{
    auto s = session("fun half(x: float) -> float => x * 0.5\n"
                     "test:\n"
                     "    half(2.0) == 1.0\n"
                     "    for i in 0 ..< 3:\n"
                     "        i < 2\n");
    CHECK(diagnostics_text(s.last("textDocument/publishDiagnostics")).starts_with("1:0 test-failed"));

    auto const marks = s.last("sgl/checkResults")["marks"];
    REQUIRE(marks.size() == 2);
    CHECK(marks[0]["range"]["start"]["line"].as_double() == 2);
    CHECK(marks[0]["passed"].as_double() == 1);
    CHECK(marks[0]["failed"].as_double() == 0);
    CHECK(marks[1]["range"]["start"]["line"].as_double() == 4);
    CHECK(marks[1]["passed"].as_double() == 2);
    CHECK(marks[1]["failed"].as_double() == 1);
}

TEST("sgl lsp - semantic tokens name what the checker resolved, relative to the token before", main_thread)
{
    auto s = session("struct light:\n    power: float\n");
    auto const r = s.ask(4, "textDocument/semanticTokens/full", R"({"textDocument":{"uri":"file:///t.sgl"}})");
    auto const data = r["result"]["data"];
    // `struct`, `light`, `power`, the `:` of the field, which the form tree reads as an operator, and `float`
    REQUIRE(data.size() == 5 * 5);
    auto const legend = sgl_lsp::semantic_tokens_legend();
    auto const type_of
        = [&](isize token) { return cc::string_view(legend.token_types[isize(data[token * 5 + 3].as_double())]); };
    CHECK(type_of(0) == "keyword");
    CHECK(type_of(1) == "struct");
    CHECK(type_of(2) == "property");
    CHECK(type_of(3) == "operator");
    CHECK(type_of(4) == "type");
    // `power` is one line down, at character 4
    CHECK(data[2 * 5 + 0].as_double() == 1);
    CHECK(data[2 * 5 + 1].as_double() == 4);
    CHECK(data[2 * 5 + 2].as_double() == 5);
}

TEST("sgl lsp - an unannotated let gets its type as a hint that inserts itself", main_thread)
{
    auto s = session("fun f(v: vec3) -> float:\n    let d = dot(v, v)\n    let e : float = d\n    return e\n");
    auto const r = s.ask(
        5, "textDocument/inlayHint",
        R"({"textDocument":{"uri":"file:///t.sgl"},"range":{"start":{"line":0,"character":0},"end":{"line":9,"character":0}}})");
    auto const hints = r["result"];
    REQUIRE(hints.size() == 1);
    CHECK(hints[0]["label"].as_string() == ": float");
    CHECK(hints[0]["position"]["line"].as_double() == 1);
    CHECK(hints[0]["position"]["character"].as_double() == 9);
    CHECK(hints[0]["textEdits"][0]["newText"].as_string() == " : float");
}

TEST("sgl lsp - an inferred return type is a hint before the arrow, which inserts itself where it may be written",
     main_thread)
{
    auto s = session("fun half(x: float) => x * 0.5\nstruct box:\n    w: float\n    area => self.w * self.w\n");
    auto const r = s.ask(
        5, "textDocument/inlayHint",
        R"({"textDocument":{"uri":"file:///t.sgl"},"range":{"start":{"line":0,"character":0},"end":{"line":9,"character":0}}})");
    auto const hints = r["result"];
    REQUIRE(hints.size() == 2);
    CHECK(hints[0]["label"].as_string() == "-> float");
    CHECK(hints[0]["position"]["line"].as_double() == 0);
    CHECK(hints[0]["position"]["character"].as_double() == 19);
    CHECK(hints[0]["textEdits"][0]["newText"].as_string() == "-> float ");
    // a property of a type body takes no `-> type`, so its hint only shows it
    CHECK(hints[1]["position"]["line"].as_double() == 3);
    CHECK(!hints[1].has("textEdits"));
}

TEST("sgl lsp - positions count UTF-16 units unless the client offers UTF-8", main_thread)
{
    // `é` is two bytes and one UTF-16 unit, so the unknown name after it on its line sits one character further left
    auto const source = cc::string_view("fun f() -> float:\n    let \xc3\xa9x = nope\n    return 1.0\n");
    auto utf16 = session(source);
    auto utf8 = session(source, true);
    CHECK(diagnostics_text(utf16.last("textDocument/publishDiagnostics")).starts_with("1:13 unknown-name"));
    CHECK(diagnostics_text(utf8.last("textDocument/publishDiagnostics")).starts_with("1:14 unknown-name"));
}

TEST("sgl lsp - a character beyond the BMP is two UTF-16 units and four bytes before the token after it", main_thread)
{
    // U+1F600 in a string, so the closing quote after it is a token of the same line
    auto const source = cc::string_view("fun f():\n    print \"\xf0\x9f\x98\x80\"\n");
    auto const line_1_tokens = [&](bool offers_utf8)
    {
        auto s = session(source, offers_utf8);
        auto const r = s.ask(4, "textDocument/semanticTokens/full", R"({"textDocument":{"uri":"file:///t.sgl"}})");
        auto const data = r["result"]["data"];
        // every token of line 1 as `start+length`, undoing the encoding relative to the token before
        auto out = cc::string();
        auto line = i32(0);
        auto start = i32(0);
        for (auto i = isize(0); i + 5 <= data.size(); i += 5)
        {
            auto const delta_line = i32(data[i].as_double());
            line += delta_line;
            start = (delta_line == 0 ? start : 0) + i32(data[i + 1].as_double());
            if (line == 1)
                out.appendf("{}+{} ", start, i32(data[i + 2].as_double()));
        }
        return out;
    };
    // `print`, the opening quote, the string's body, the closing quote
    CHECK(line_1_tokens(false) == "4+5 10+1 11+2 13+1 ");
    CHECK(line_1_tokens(true) == "4+5 10+1 11+4 15+1 ");
}

TEST("sgl lsp - a note into the prelude names a virtual document the client can open", main_thread)
{
    auto s = session("fun f() -> float => 1.0\n");
    auto const r = s.ask(6, "sgl/preludeText", R"({"uri":"sgl-prelude:/core.sgl"})");
    CHECK(!r["result"]["text"].as_string().empty());
    auto const bad = s.ask(7, "sgl/preludeText", R"({"uri":"sgl-prelude:/nope.sgl"})");
    CHECK(bad["error"]["code"].as_double() == lsp::error_code::invalid_params);
}

TEST("sgl lsp - closing a document clears its diagnostics", main_thread)
{
    auto s = session("fun f() -> float => nope\n");
    s.send_close();
    s.settle();
    CHECK(s.last("textDocument/publishDiagnostics")["diagnostics"].size() == 0);
}

TEST("sgl lsp - a version changed before its check settled publishes nothing, and the next one publishes all", main_thread)
{
    auto s = session();
    s.send_open("fun f() -> float => nope\ntest 2 < 1\n");
    s.send_change("fun f() -> float => 1.0\ntest f() == 1.0\n", 2);
    s.settle();

    auto const published = notifications_of(s.messages, "textDocument/publishDiagnostics");
    CHECK(published.size() == 2); // the check's, then again once the tests ran
    for (auto const& p : published)
    {
        CHECK(p["version"].as_double() == 2);
        CHECK(p["diagnostics"].size() == 0);
    }
    auto const results = notifications_of(s.messages, "sgl/checkResults");
    REQUIRE(results.size() == 1);
    CHECK(results[0]["version"].as_double() == 2);
    CHECK(results[0]["marks"][0]["passed"].as_double() == 1);
}

TEST("sgl lsp - a document closed while its check or tests are in flight is cleared once, and nothing follows",
     main_thread)
{
    // a test that runs to its fuel limit, so the run is what is in flight
    auto const source = cc::string_view("test:\n    let mut i = 0\n    while i >= 0:\n        i = i + 1\n    i < 0\n");
    auto s = session();
    s.send_open(source);

    SECTION("before the check settled")
    {
    }
    SECTION("while the tests run")
    {
        s.settle_until_sent("textDocument/publishDiagnostics");
        s.messages.clear();
    }

    s.send_close();
    s.settle();
    auto const published = notifications_of(s.messages, "textDocument/publishDiagnostics");
    REQUIRE(published.size() == 1);
    CHECK(published[0]["diagnostics"].size() == 0);
    CHECK(notifications_of(s.messages, "sgl/checkResults").empty());
}

TEST("sgl lsp - a test judged by the diagnostics it expects is one mark on its keyword, green when they occurred",
     main_thread)
{
    auto s = session("@expect(error = \"unknown-name\")\ntest nope\n@expect(error = \"type-mismatch\")\ntest 1 < 2\n");
    auto const marks = s.last("sgl/checkResults")["marks"];
    REQUIRE(marks.size() == 2);
    CHECK(marks[0]["range"]["start"]["line"].as_double() == 1);
    CHECK(marks[0]["passed"].as_double() == 1);
    CHECK(marks[1]["range"]["start"]["line"].as_double() == 3);
    CHECK(marks[1]["failed"].as_double() == 1);
}

TEST("sgl lsp - an error in a module the document uses is shown on its `use`, pointing into the module's file",
     main_thread)
{
    sgl_lsp::set_module_dirs({cc::string(SGL_LSP_TEST_MODULES_DIR)});
    auto s = session("// uses a broken module\nuse broken\nfun g() -> float => broken.f()\n");
    sgl_lsp::set_module_dirs({});
    auto const p = s.last("textDocument/publishDiagnostics");
    CHECK(diagnostics_text(p) == "1:0 unknown-name module broken does not check: nope\n");
    auto const related = p["diagnostics"][0]["relatedInformation"][0];
    CHECK(related["location"]["uri"].as_string().ends_with("/tests/modules/broken.sgl"));
    CHECK(related["location"]["range"]["start"]["line"].as_double() == 3);
}
