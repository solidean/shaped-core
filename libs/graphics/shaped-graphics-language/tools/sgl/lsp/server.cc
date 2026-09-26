#include "server.hh"

#include <clean-core/thread/async_coroutine.hh>

using namespace cc::primitive_defines;

namespace sgl_lsp
{
/// `sgl/preludeText`: the text of an `sgl-prelude:` document, which a note into the prelude points at.
struct prelude_text_params
{
    cc::string uri;
};

[[nodiscard]] bool read(lsp::json::ref in, prelude_text_params& out)
{
    return lsp::json::read(in["uri"], out.uri);
}

struct prelude_text
{
    cc::string text;
};

void write_fields(lsp::json::writer& w, prelude_text const& v)
{
    lsp::json::put(w, "text", v.text);
}
} // namespace sgl_lsp

namespace
{
template <class R>
[[nodiscard]] cc::shared_async<lsp::answer<R>> answer_now(R value)
{
    return cc::make_async_from_value(lsp::answer<R>(cc::move(value)));
}

[[nodiscard]] lsp::response_error cancelled()
{
    return {.code = lsp::error_code::request_cancelled, .message = "cancelled"};
}

cc::shared_async<lsp::answer<lsp::semantic_tokens>> tokens_request(sgl_lsp::analysis_cache::handle a,
                                                                   lsp::position_encoding e,
                                                                   cc::shared_ptr<lsp::cancel_flag> flag)
{
    auto const& analysis = co_await a;
    if (flag->is_raised.load())
        co_return lsp::answer<lsp::semantic_tokens>(cc::error(cancelled()));
    co_return lsp::answer<lsp::semantic_tokens>(sgl_lsp::semantic_tokens_of(*analysis, e));
}

cc::shared_async<lsp::answer<cc::vector<lsp::inlay_hint>>> hints_request(sgl_lsp::analysis_cache::handle a,
                                                                         lsp::range visible,
                                                                         lsp::position_encoding e,
                                                                         cc::shared_ptr<lsp::cancel_flag> flag)
{
    auto const& analysis = co_await a;
    if (flag->is_raised.load())
        co_return lsp::answer<cc::vector<lsp::inlay_hint>>(cc::error(cancelled()));
    co_return lsp::answer<cc::vector<lsp::inlay_hint>>(sgl_lsp::inlay_hints_of(*analysis, visible, e));
}
} // namespace

cc::unique_ptr<sgl_lsp::language_server> sgl_lsp::language_server::create()
{
    return cc::make_unique<language_server>();
}

sgl_lsp::language_server::language_server()
  : _server({
        .name = "sgl",
        .version = "0.1",
        .write_capabilities =
            [](lsp::json::writer& w)
        {
            w.begin_object("semanticTokensProvider");
            lsp::json::put(w, "legend", semantic_tokens_legend());
            w.write("full", true);
            w.end_object();
            w.write("inlayHintProvider", true);
        },
    })
{
    _server.on_document([this](cc::string_view uri) { impl_on_document(uri); });

    _server.on_request<lsp::text_document_params>("textDocument/semanticTokens/full",
                                                  [this](lsp::request_context const& ctx, lsp::text_document_params p)
                                                  {
                                                      auto a = _analyses.of(_server.workspace().snapshot(), p.uri);
                                                      if (!a)
                                                          return answer_now(lsp::semantic_tokens());
                                                      return tokens_request(cc::move(a), _server.encoding(), ctx.flag);
                                                  });

    _server.on_request<lsp::inlay_hint_params>("textDocument/inlayHint",
                                               [this](lsp::request_context const& ctx, lsp::inlay_hint_params p)
                                               {
                                                   auto a = _analyses.of(_server.workspace().snapshot(), p.uri);
                                                   if (!a)
                                                       return answer_now(cc::vector<lsp::inlay_hint>());
                                                   return hints_request(cc::move(a), p.range, _server.encoding(),
                                                                        ctx.flag);
                                               });

    _server.on_request<prelude_text_params>(
        "sgl/preludeText",
        [](lsp::request_context const&, prelude_text_params const& p)
        {
            auto const& prelude = the_prelude();
            for (auto i = isize(0); i < prelude.names.size(); ++i)
                if (prelude.uri_of(i) == p.uri)
                    return answer_now(prelude_text{.text = prelude.files[i].source});
            return cc::make_async_from_value(lsp::answer<prelude_text>(cc::error(
                lsp::response_error{.code = lsp::error_code::invalid_params, .message = "no such prelude file"})));
        });
}

void sgl_lsp::language_server::impl_on_document(cc::string_view uri)
{
    // whatever still runs for the version before is worthless now
    if (auto* const previous = _test_runs.get_ptr(uri))
        (*previous)->is_raised.store(true);

    auto checking = _analyses.of(_server.workspace().snapshot(), uri);
    if (!checking)
    {
        // closed: its squiggles go with it
        _analyses.forget(uri);
        _test_runs.erase(uri);
        _server.notify("textDocument/publishDiagnostics", lsp::publish_diagnostics_params{.uri = cc::string(uri)});
        return;
    }

    auto stop = cc::make_shared<lsp::cancel_flag>();
    _test_runs[cc::string(uri)] = stop;
    auto const e = _server.encoding();
    _server.when_ready(cc::move(checking),
                       [this, stop, e](cc::async<cc::shared_ptr<analysis>>& checked)
                       {
                           if (stop->is_raised.load() || !checked.has_value())
                               return;
                           auto const& a = checked.value();
                           _server.notify("textDocument/publishDiagnostics",
                                          lsp::publish_diagnostics_params{.uri = a->document->uri,
                                                                          .version = a->document->version,
                                                                          .diagnostics = diagnostics_of(*a, {}, e)});

                           _server.when_ready(run_tests(a, stop),
                                              [this, stop, e](cc::async<test_run>& ran)
                                              {
                                                  if (stop->is_raised.load() || !ran.has_value() || ran.value().is_stopped)
                                                      return;
                                                  auto const& run = ran.value();
                                                  auto const& a = *run.checked;
                                                  _server.notify("textDocument/publishDiagnostics",
                                                                 lsp::publish_diagnostics_params{
                                                                     .uri = a.document->uri,
                                                                     .version = a.document->version,
                                                                     .diagnostics = diagnostics_of(a, run.results, e),
                                                                 });
                                                  _server.notify("sgl/checkResults", check_results_of(run, e));
                                              });
                       });
}
