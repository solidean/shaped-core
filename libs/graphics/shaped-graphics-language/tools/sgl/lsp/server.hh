#pragma once

#include "analysis.hh"
#include "features.hh"
#include "fwd.hh"
#include "protocol/server.hh"

#include <clean-core/container/map.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/memory/unique_ptr.hh>

/// The SGL language server: the protocol core with SGL's handlers registered on it.
///
/// On every open or change of a document it checks it, publishes its diagnostics, then runs its tests and publishes
/// again with their failures and the check marks.
/// The next version of the document stops that run, between tests and inside one through `run_limits::stop`, and so
/// does the server's destruction.
/// Requests read the analysis of the version they were asked about, which one check serves however many ask.
/// Everything here runs on the thread driving the protocol server, except the analyses and tests, which run on compute.
class sgl_lsp::language_server
{
public:
    [[nodiscard]] static cc::unique_ptr<language_server> create();

    language_server(language_server&&) = delete;
    language_server& operator=(language_server&&) = delete;

    [[nodiscard]] lsp::server& protocol() { return _server; }

    // implementation, public for make_unique
public:
    language_server();
    ~language_server();

private:
    void impl_on_document(cc::string_view uri);
    /// Raises the flag of every test run in flight.
    void impl_stop_test_runs();

    lsp::server _server;
    analysis_cache _analyses;
    /// The flag of the test run of each document in flight, which its next version raises.
    cc::map<cc::string, cc::shared_ptr<lsp::cancel_flag>> _test_runs;
};
