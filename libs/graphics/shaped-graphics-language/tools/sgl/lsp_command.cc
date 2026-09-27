#include "lsp/protocol/log_listener.hh"
#include "lsp/protocol/stdio_host.hh"
#include "lsp/server.hh"

#include <clean-core/record/listener.hh>
#include <clean-core/record/system.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>

// `sgl lsp`: the SGL language server over stdin and stdout, which the VS Code extension starts.
// Arguments are ignored, `--stdio` among them, since stdio is the only transport.
// It owns the recorder, because nexus's console listener would write info lines to stdout, which is the protocol's.
ASYNC_COMMAND("lsp", nx::config::owns_recorder)
{
    // first, before anything could print: from here on stdout is the protocol's alone
    auto host = lsp::stdio_host::open();
    cc::rec::initialize();

    auto server = sgl_lsp::language_server::create();
    auto log = lsp::log_listener(server->protocol());
    auto const log_handle = cc::rec::register_listener(log);
    server->protocol().on_initialized(
        [&]
        {
            auto const name = server->protocol().initialize_params()["initializationOptions"]["logLevel"].as_string();
            if (auto const level = lsp::level_of(name); level.has_value())
                log.set_min_level(level.value());
        });

    auto const exit_code = co_await host->serve(server->protocol());

    // the host goes first: its pump drives the server
    host = nullptr;
    cc::rec::flush_blocking();
    cc::rec::unregister_listener(log_handle);
    cc::rec::shutdown();
    co_return exit_code;
}
