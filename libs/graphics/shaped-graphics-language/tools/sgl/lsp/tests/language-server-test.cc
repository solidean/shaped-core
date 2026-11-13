#include "../server.hh"
#include "lsp-test-support.hh"

#include <clean-core/string/uri.hh>
#include <nexus/source_root.hh>
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
    auto const dir = nx::source_path(SGL_PRELUDE_DIR);
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

TEST("sgl lsp - a barrier call is a control builtin, a type the legend carries", main_thread)
{
    auto const legend = sgl_lsp::semantic_tokens_legend();
    auto has_type = false;
    for (auto const& t : legend.token_types)
        has_type = has_type || t == "controlBuiltin";
    CHECK(has_type);

    auto s = session("fun sync():\n    workgroup_barrier()\n");
    auto const r = s.ask(4, "textDocument/semanticTokens/full", R"({"textDocument":{"uri":"file:///t.sgl"}})");
    auto const data = r["result"]["data"];
    // `fun`, `sync`, and `workgroup_barrier` one line down; the brackets and the colon have no class
    REQUIRE(data.size() >= 3 * 5);
    auto const last = data.size() / 5 - 1;
    CHECK(data[last * 5 + 0].as_double() == 1);
    CHECK(data[last * 5 + 1].as_double() == 4);
    CHECK(legend.token_types[isize(data[last * 5 + 3].as_double())] == "controlBuiltin");
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

TEST("sgl lsp - a diagnostic the checker knows a fix for offers it as a preferred quick fix", main_thread)
{
    auto s = session("fun f(a: int, b: int) -> float:\n    let d = a as float - b as float\n    return d\n");
    auto const r = s.ask(
        5, "textDocument/codeAction",
        R"({"textDocument":{"uri":"file:///t.sgl"},"range":{"start":{"line":1,"character":12},"end":{"line":1,"character":12}},"context":{"diagnostics":[]}})");
    auto const actions = r["result"];
    REQUIRE(actions.size() == 1);
    CHECK(actions[0]["kind"].as_string() == "quickfix");
    CHECK(actions[0]["isPreferred"].as_bool());
    CHECK(actions[0]["diagnostics"][0]["code"].as_string() == "as-precedence");
    auto const edit = actions[0]["edit"]["changes"]["file:///t.sgl"][0];
    CHECK(edit["newText"].as_string() == "(a as float) - (b as float)");
    CHECK(edit["range"]["start"]["character"].as_double() == 12);
    CHECK(edit["range"]["end"]["character"].as_double() == 35);

    // a range that touches no diagnostic offers nothing
    auto const elsewhere = s.ask(
        6, "textDocument/codeAction",
        R"({"textDocument":{"uri":"file:///t.sgl"},"range":{"start":{"line":2,"character":4},"end":{"line":2,"character":4}},"context":{"diagnostics":[]}})");
    CHECK(elsewhere["result"].size() == 0);
}

namespace
{
/// The params of a request about one position of `file:///t.sgl`.
cc::string at_position(i32 line, i32 character)
{
    return cc::format(R"({{"textDocument":{{"uri":"file:///t.sgl"}},"position":{{"line":{},"character":{}}}}})", line,
                      character);
}
} // namespace

TEST("sgl lsp - a hover shows the declaration as SGL writes it, with its doc comment, over the name's range", main_thread)
{
    auto s = session("/// Half of `x`.\nfun half(x: float) -> float => x * 0.5\nfun f() -> float => half(2.0)\n");
    auto const r = s.ask(5, "textDocument/hover", at_position(2, 22));
    auto const contents = r["result"]["contents"];
    CHECK(contents["kind"].as_string() == "markdown");
    CHECK(contents["value"].as_string() == "```sgl\nfun half(x: float) -> float\n```\n\nHalf of `x`.");
    CHECK(r["result"]["range"]["start"]["line"].as_double() == 2);
    CHECK(r["result"]["range"]["start"]["character"].as_double() == 20);
    CHECK(r["result"]["range"]["end"]["character"].as_double() == 24);

    // a keyword has nothing to show, which is a null result
    auto const nothing = s.ask(6, "textDocument/hover", at_position(2, 1));
    CHECK(nothing.has("result"));
    CHECK(nothing["result"].is_null());
}

TEST("sgl lsp - go to definition lands on the declared name, a builtin's in its virtual prelude document", main_thread)
{
    auto s = session("fun f(v: vec3) -> float:\n    let d = dot(v, v)\n    return d\n");
    auto const local = s.ask(5, "textDocument/definition", at_position(2, 11));
    CHECK(local["result"]["uri"].as_string() == "file:///t.sgl");
    CHECK(local["result"]["range"]["start"]["line"].as_double() == 1);
    CHECK(local["result"]["range"]["start"]["character"].as_double() == 8);

    auto const builtin = s.ask(6, "textDocument/definition", at_position(1, 13));
    CHECK(builtin["result"]["uri"].as_string().starts_with("sgl-prelude:/"));
    // the virtual document the client fetches holds the declaration at that line
    auto const text = s.ask(7, "sgl/preludeText", cc::format(R"({{"uri":"{}"}})", builtin["result"]["uri"].as_string()));
    auto const line = i32(builtin["result"]["range"]["start"]["line"].as_double());
    auto const prelude = text["result"]["text"].as_string();
    auto start = isize(0);
    for (auto i = 0; i < line; ++i)
    {
        start = prelude.find('\n', start) + 1;
        REQUIRE(start > 0);
    }
    auto const end = prelude.find('\n', start);
    CHECK(prelude.subview({.start = start, .end = end < 0 ? prelude.size() : end}).contains("fun dot("));
}

TEST("sgl lsp - completion after a dot lists the object's members, and announces the dot as its trigger", main_thread)
{
    auto s = session("struct light:\n    power: float\nfun f(l: light) -> float:\n    let p = l.\n    return 0.0\n");
    auto const r = s.ask(5, "textDocument/completion", at_position(3, 14));
    auto const items = r["result"];
    REQUIRE(items.size() == 1);
    CHECK(items[0]["label"].as_string() == "power");
    CHECK(items[0]["kind"].as_double() == i32(lsp::completion_item_kind::field));
    CHECK(items[0]["detail"].as_string() == "light.power : float");

    // a name's completions include the parameter, the prelude's builtins and the keywords
    auto const names = s.ask(6, "textDocument/completion", at_position(4, 11));
    auto has = [&](cc::string_view label)
    {
        for (auto i = isize(0); i < names["result"].size(); ++i)
            if (names["result"][i]["label"].as_string() == label)
                return true;
        return false;
    };
    CHECK(has("l"));
    CHECK(has("dot"));
    CHECK(has("return"));
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
    sgl_lsp::set_module_dirs({nx::source_path(SGL_LSP_TEST_MODULES_DIR)});
    auto s = session("// uses a broken module\nuse broken\nfun g() -> float => broken.f()\n");
    sgl_lsp::set_module_dirs({});
    auto const p = s.last("textDocument/publishDiagnostics");
    CHECK(diagnostics_text(p) == "1:0 unknown-name module broken does not check: nope\n");
    auto const related = p["diagnostics"][0]["relatedInformation"][0];
    CHECK(related["location"]["uri"].as_string().ends_with("/tests/modules/broken.sgl"));
    CHECK(related["location"]["range"]["start"]["line"].as_double() == 3);
}

TEST("sgl lsp - every edit of a document continues from one parse and one check of the modules it uses", main_thread)
{
    auto const document_of = [](cc::string text)
    {
        auto d = cc::make_shared<lsp::document>();
        d->uri = "file:///t.sgl";
        d->version = 1;
        d->text = cc::move(text);
        d->index = lsp::text_index(d->text);
        return d;
    };
    sgl_lsp::set_module_dirs({nx::source_path(SGL_LSP_TEST_CLEAN_MODULES_DIR)});
    auto const first = sgl_lsp::analyze(document_of("use shading\nfun g() -> float => shading.lit(1.0)\n"));
    auto const edited = sgl_lsp::analyze(document_of("use shading\nfun g() -> float => shading.lit(2.0)\n"));
    auto const other = sgl_lsp::analyze(document_of("use noise\nfun h() -> float => noise.grain(1.0)\n"));
    sgl_lsp::set_module_dirs({});

    REQUIRE(first->layer != nullptr);
    CHECK(first->diagnostics.empty());
    CHECK(edited->diagnostics.empty());
    CHECK(other->diagnostics.empty());
    CHECK(edited->library == first->library);
    CHECK(edited->layer == first->layer);
    CHECK(other->layer == first->layer);
    // the layer holds both modules, and each document reaches only what it uses
    CHECK(first->module.reached_library_files.size() == 2);
    REQUIRE(other->module.reached_library_files.size() == 1);
    CHECK(other->library->names[other->module.reached_library_files[0]].ends_with("noise.sgl"));
    CHECK(first->text_of(first->module.prelude_file_count()).contains("module "));
}

TEST("sgl lsp - an edit of a module checks it and the modules after it again, and keeps the layers of those before",
     main_thread)
{
    auto const document_of = [](cc::string text)
    {
        auto d = cc::make_shared<lsp::document>();
        d->uri = "file:///t.sgl";
        d->version = 1;
        d->text = cc::move(text);
        d->index = lsp::text_index(d->text);
        return d;
    };
    auto const dir = nx::source_path(SGL_LSP_TEST_CLEAN_MODULES_DIR);
    sgl_lsp::set_module_dirs({dir});
    auto const doc = document_of("use shading\nfun g() -> float => shading.lit(1.0)\n");
    auto const first = sgl_lsp::analyze(doc);
    // `shading` uses `noise`, so it comes after it in the chain; an unsaved edit of it is a new last layer
    auto const shading_uri = lsp::uri_of_path(dir + "/shading.sgl");
    auto const edited_text
        = cc::string("module shading\n\nuse noise\n\nfun lit(x: float) -> float => noise.grain(x) * 3.0\n");
    auto const edited = sgl_lsp::analyze(doc, cc::vector<sgl_lsp::open_text>{{.uri = shading_uri, .text = edited_text}});
    sgl_lsp::set_module_dirs({});

    CHECK(first->diagnostics.empty());
    CHECK(edited->diagnostics.empty());
    REQUIRE(first->layer != nullptr);
    REQUIRE(edited->layer != nullptr);
    CHECK(edited->layer != first->layer);
    auto const before_of = [](sgl::shared_library const& library)
    {
        return library.checked.lock([](sgl::shared_library::layers& l)
                                    { return l.library != nullptr ? l.library->before : nullptr; });
    };
    auto const kept = before_of(*first->library);
    REQUIRE(kept != nullptr);
    CHECK(before_of(*edited->library) == kept);
}

TEST("sgl lsp - an edit of a module that leaves what it declares alone moves the modules using it rather than checking "
     "them again",
     main_thread)
{
    auto const document_of = [](cc::string text)
    {
        auto d = cc::make_shared<lsp::document>();
        d->uri = "file:///t.sgl";
        d->version = 1;
        d->text = cc::move(text);
        d->index = lsp::text_index(d->text);
        return d;
    };
    auto const dir = nx::source_path(SGL_LSP_TEST_CLEAN_MODULES_DIR);
    sgl_lsp::set_module_dirs({dir});
    auto const doc = document_of("use shading\ntest shading.lit(4.0) == 2.0\n");
    auto const first = sgl_lsp::analyze(doc);
    // `noise` comes first, since `shading` uses it; its body changes, and nothing it declares
    auto const noise_uri = lsp::uri_of_path(dir + "/noise.sgl");
    auto const edited_text
        = cc::string("module noise\n\n// a line more\nfun grain(x: float) -> float => x * 0.25 + 0.0\n");
    auto const edited = sgl_lsp::analyze(doc, cc::vector<sgl_lsp::open_text>{{.uri = noise_uri, .text = edited_text}});
    sgl_lsp::set_module_dirs({});

    CHECK(first->diagnostics.empty());
    CHECK(edited->diagnostics.empty());
    REQUIRE(edited->layer != nullptr);
    CHECK(edited->layer != first->layer);
    CHECK(edited->layer->module == "shading");
    CHECK(edited->layer->layer->is_relocated());
    CHECK(!edited->layer->before->layer->is_relocated());
    // the document's test reads the module as it moved, and still passes
    CHECK(edited->module.tests.size() == first->module.tests.size());
}

TEST("sgl lsp - an edit moves the modules after it from the library last used, though another edit was made since",
     main_thread)
{
    auto const document_of = [](cc::string text)
    {
        auto d = cc::make_shared<lsp::document>();
        d->uri = "file:///t.sgl";
        d->version = 1;
        d->text = cc::move(text);
        d->index = lsp::text_index(d->text);
        return d;
    };
    auto const dir = nx::source_path(SGL_LSP_TEST_CLEAN_MODULES_DIR);
    sgl_lsp::set_module_dirs({dir});
    auto const doc = document_of("use shading\ntest shading.lit(4.0) == 2.0\n");
    auto const clean = sgl_lsp::analyze(doc);
    // an edit of `shading` that makes `lit` triple, then the clean files again: found in the library cache, or made
    // again where other tests' libraries pushed it out, and the last library either way
    auto const shading_uri = lsp::uri_of_path(dir + "/shading.sgl");
    auto const tripled
        = cc::string("module shading\n\nuse noise\n\nfun lit(x: float) -> float => noise.grain(x) * 3.0\n");
    auto const other_edit = sgl_lsp::analyze(doc, cc::vector<sgl_lsp::open_text>{{.uri = shading_uri, .text = tripled}});
    auto const found_again = sgl_lsp::analyze(doc);
    // an edit of `noise` starts from the clean library, so `shading` moves as the clean text, never as the tripled one
    auto const noise_uri = lsp::uri_of_path(dir + "/noise.sgl");
    auto const edited_text
        = cc::string("module noise\n\n// a line more\nfun grain(x: float) -> float => x * 0.25 + 0.0\n");
    auto const edited = sgl_lsp::analyze(doc, cc::vector<sgl_lsp::open_text>{{.uri = noise_uri, .text = edited_text}});
    sgl_lsp::set_module_dirs({});

    CHECK(found_again->diagnostics.empty());
    CHECK(edited->diagnostics.empty());
    REQUIRE(edited->layer != nullptr);
    CHECK(edited->layer->module == "shading");
    CHECK(edited->layer->layer->is_relocated());
    // the document's test reads `shading` as it is on disk: `lit` doubles, where the other edit's tripled
    REQUIRE(edited->module.tests.size() == clean->module.tests.size());
    for (auto t = isize(0); t < edited->module.tests.size(); ++t)
        if (edited->module.tests[t].file == edited->user_file())
            CHECK(sgl::test::run_test(edited->module, edited->module_files(), i32(t), {}).is_passed());
    CHECK(other_edit->diagnostics.empty());
}

TEST("sgl lsp - a module file open in the editor is what the documents using it are checked against", main_thread)
{
    sgl_lsp::set_module_dirs({nx::source_path(SGL_LSP_TEST_MODULES_DIR)});
    auto s = session("use broken\nfun g() -> float => broken.f()\n");
    auto const last_of_document = [&]
    {
        auto result = cc::string();
        for (auto const& p : notifications_of(s.messages, "textDocument/publishDiagnostics"))
            if (p["uri"].as_string() == "file:///t.sgl")
                result = diagnostics_text(p);
        return result;
    };
    CHECK(last_of_document() == "0:0 unknown-name module broken does not check: nope\n");

    // the module opened with an unsaved fix: the document that uses it is checked again, against the fix
    auto const dir = nx::source_path(SGL_LSP_TEST_MODULES_DIR);
    auto const module_uri = dir.starts_with("/") ? "file://" + dir + "/broken.sgl" : "file:///" + dir + "/broken.sgl";
    s.send_open("module broken\n\nfun f() -> float => 1.0\n", module_uri);
    s.settle();
    sgl_lsp::set_module_dirs({});
    CHECK(last_of_document() == "");
}

TEST("sgl lsp - signature help shows the call's overloads and the argument the cursor is in", main_thread)
{
    auto s = session("/// Half of `x`.\nfun halve(x: float, y: float) -> float => x * y\nfun f() -> float => "
                     "halve(2.0, 1.0)\n");
    auto const r = s.ask(5, "textDocument/signatureHelp", at_position(2, 31));
    auto const help = r["result"];
    REQUIRE(help["signatures"].size() == 1);
    CHECK(help["signatures"][0]["label"].as_string() == "halve(x: float, y: float) -> float");
    CHECK(help["signatures"][0]["documentation"]["value"].as_string() == "Half of `x`.");
    REQUIRE(help["signatures"][0]["parameters"].size() == 2);
    CHECK(help["signatures"][0]["parameters"][1]["label"].as_string() == "y: float");
    CHECK(help["activeParameter"].as_double() == 1);

    // outside any call's parens it is a null result
    auto const nothing = s.ask(6, "textDocument/signatureHelp", at_position(2, 5));
    CHECK(nothing["result"].is_null());
}

TEST("sgl lsp - references list the declaration and every use, and leave the declaration out when asked", main_thread)
{
    auto s = session("fun halve(x: float) -> float => x * 0.5\nfun f() -> float => halve(2.0) + halve(4.0)\n");
    auto const params = [](bool include)
    {
        return cc::format(R"({{"textDocument":{{"uri":"file:///t.sgl"}},"position":{{"line":1,"character":22}},)"
                          R"("context":{{"includeDeclaration":{}}}}})",
                          include ? "true" : "false");
    };
    auto const all = s.ask(5, "textDocument/references", params(true));
    REQUIRE(all["result"].size() == 3);
    CHECK(all["result"][0]["range"]["start"]["line"].as_double() == 0);
    CHECK(all["result"][0]["range"]["start"]["character"].as_double() == 4);
    CHECK(all["result"][2]["range"]["start"]["character"].as_double() == 33);
    auto const uses = s.ask(6, "textDocument/references", params(false));
    CHECK(uses["result"].size() == 2);
}

TEST("sgl lsp - a rename edits every occurrence, and refuses a keyword and a name the prelude declares", main_thread)
{
    auto s = session("fun f(v: vec3) -> float:\n    let d = dot(v, v)\n    return d * d\n");
    auto const rename = [&](i32 id, i32 line, i32 character, cc::string_view name)
    {
        return s.ask(id, "textDocument/rename",
                     cc::format(R"({{"textDocument":{{"uri":"file:///t.sgl"}},"position":{{"line":{},"character":{}}},)"
                                R"("newName":"{}"}})",
                                line, character, name));
    };
    auto const ok = rename(5, 2, 11, "len2");
    auto const edits = ok["result"]["changes"]["file:///t.sgl"];
    REQUIRE(edits.size() == 3);
    for (auto i = isize(0); i < edits.size(); ++i)
        CHECK(edits[i]["newText"].as_string() == "len2");
    CHECK(edits[0]["range"]["start"]["line"].as_double() == 1);

    CHECK(rename(6, 2, 11, "return")["error"]["message"].as_string().contains("keyword"));
    CHECK(rename(7, 2, 11, "2d")["error"]["message"].as_string().contains("no name"));
    // `dot` is the prelude's, so neither a rename nor its preparation is offered
    CHECK(rename(8, 1, 13, "inner")["error"]["message"].as_string().contains("prelude"));
    auto const prepare = s.ask(9, "textDocument/prepareRename", at_position(1, 13));
    CHECK(prepare["result"].is_null());
    auto const local = s.ask(10, "textDocument/prepareRename", at_position(2, 11));
    CHECK(local["result"]["placeholder"].as_string() == "d");
}

TEST("sgl lsp - a hover on an overloaded function lists the rest of the set after a rule", main_thread)
{
    auto s = session("fun halve(x: float) -> float => x * 0.5\nfun halve(x: int) -> int => x / 2\nfun f() -> int => "
                     "halve(2)\n");
    auto const r = s.ask(5, "textDocument/hover", at_position(2, 20));
    CHECK(r["result"]["contents"]["value"].as_string()
          == "```sgl\nfun halve(x: int) -> int\n```\n\n---\n\nOther overloads:\n\n```sgl\nfun halve(x: float) -> "
             "float\n```");
}

TEST("sgl lsp - formatting replaces the whole document, edits nothing where it is formatted, and skips around errors",
     main_thread)
{
    auto const doc = R"({"textDocument":{"uri":"file:///t.sgl"},"options":{"tabSize":4,"insertSpaces":true}})";
    auto s = session("fun f(a: float,b: float) -> float => a*b\n");
    auto const r = s.ask(5, "textDocument/formatting", doc);
    REQUIRE(r["result"].size() == 1);
    CHECK(r["result"][0]["newText"].as_string() == "fun f(a: float, b: float) -> float => a * b\n");
    CHECK(r["result"][0]["range"]["start"]["line"].as_double() == 0);
    CHECK(r["result"][0]["range"]["end"]["line"].as_double() == 1);

    auto tidy = session("fun f(a: float, b: float) -> float => a * b\n");
    CHECK(tidy.ask(5, "textDocument/formatting", doc)["result"].size() == 0);

    // the block around a syntax error stays as written, and the rest is formatted
    auto broken = session("fun f(a:float) -> float:\n    return (1.0\n");
    auto const edits = broken.ask(5, "textDocument/formatting", doc)["result"];
    REQUIRE(edits.size() == 1);
    CHECK(edits[0]["newText"].as_string() == "fun f(a: float) -> float:\n    return (1.0\n");
}

TEST("sgl lsp - the outline is the document's declarations, each holding its members, with its first line as detail",
     main_thread)
{
    auto s = session("struct light:\n    power: float\n\nfun shade(l: light) -> float:\n    return l.power\n");
    auto const r = s.ask(5, "textDocument/documentSymbol", R"({"textDocument":{"uri":"file:///t.sgl"}})");
    auto const symbols = r["result"];
    REQUIRE(symbols.size() == 2);
    CHECK(symbols[0]["name"].as_string() == "light");
    CHECK(symbols[0]["kind"].as_double() == i32(lsp::symbol_kind::struct_));
    REQUIRE(symbols[0]["children"].size() == 1);
    CHECK(symbols[0]["children"][0]["name"].as_string() == "power");
    CHECK(symbols[1]["name"].as_string() == "shade");
    CHECK(symbols[1]["detail"].as_string() == "fun shade(l: light) -> float");
    CHECK(symbols[1]["range"]["end"]["line"].as_double() == 4);
    CHECK(symbols[1]["selectionRange"]["start"]["character"].as_double() == 4);
}

TEST("sgl lsp - folding ranges are the lines that own deeper ones, and runs of comments", main_thread)
{
    auto s = session("// a\n// b\nfun f() -> float:\n    return 1.0\n");
    auto const r = s.ask(5, "textDocument/foldingRange", R"({"textDocument":{"uri":"file:///t.sgl"}})");
    auto const folds = r["result"];
    REQUIRE(folds.size() == 2);
    auto has = [&](i32 start, i32 end, bool is_comment)
    {
        for (auto i = isize(0); i < folds.size(); ++i)
            if (folds[i]["startLine"].as_double() == start && folds[i]["endLine"].as_double() == end
                && folds[i].has("kind") == is_comment)
                return true;
        return false;
    };
    CHECK(has(2, 3, false));
    CHECK(has(0, 1, true));
}

TEST("sgl lsp - workspace symbols match the query's characters in order, across the open documents", main_thread)
{
    auto s = session("struct light:\n    power: float\nfun shade_light(l: light) -> float => l.power\n");
    auto const r = s.ask(5, "workspace/symbol", R"({"query":"shli"})");
    auto const found = r["result"];
    REQUIRE(found.size() == 1);
    CHECK(found[0]["name"].as_string() == "shade_light");
    CHECK(found[0]["location"]["uri"].as_string() == "file:///t.sgl");
    // a member names the declaration that holds it
    auto const member = s.ask(6, "workspace/symbol", R"({"query":"power"})");
    REQUIRE(member["result"].size() == 1);
    CHECK(member["result"][0]["containerName"].as_string() == "light");
}
