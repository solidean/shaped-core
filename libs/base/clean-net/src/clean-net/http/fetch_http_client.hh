#pragma once

#include <clean-core/string/string.hh>
#include <clean-net/http/http_client.hh>

/// The client over a browser's own `fetch`, which is the only HTTP a wasm program has.
///
/// **`level()` is `http_level::fetch`, and a request needing more fails rather than degrading.**
/// `follow_redirects = false` is refused with `level_not_supported`: a browser follows redirects itself, and the one
/// way to stop it hands back an opaque response with no status and no headers.
/// The browser also owns connection reuse, TLS and the forbidden headers -- a `Host` or `Cookie` set here is dropped
/// by the browser, not by this client.
///
/// **Nothing here blocks**, which is what makes it usable on a browser main thread.
/// The JavaScript side runs on the main runtime thread whichever thread sent the request, because a pool worker
/// never returns to its event loop and a `fetch` started there would never answer.
/// Its completions reach the `io_system` as signals, so the response is stepped by the reactor like any other.
///
/// `max_body_bytes` holds as it does everywhere, and the body is pulled from the response stream one chunk at a time:
/// a sink that takes less than it was offered stops the reading until it resumes.
class cnet::fetch_http_client final : public cnet::http_client
{
public:
    /// Over an `io_system`, whose clock bounds each request and whose reactor steps it.
    /// The io_system must outlive this client.
    explicit fetch_http_client(io_system& io) : _io(io) {}

    /// Whether this platform has a `fetch`: wasm only.
    /// Elsewhere every request fails with `unsupported`.
    [[nodiscard]] static bool is_supported();

    /// The URL of the page or script this program runs in, which a relative reference is resolved against.
    /// Absent off wasm, and wherever the host has no `location`.
    [[nodiscard]] static cc::optional<cc::string> document_url();

    [[nodiscard]] http_level level() const override { return http_level::fetch; }

    [[nodiscard]] cc::shared_async<http_response_head> send_streaming(http_request request,
                                                                      body_sink sink,
                                                                      request_options const& options,
                                                                      cancel_token const& token) override;

private:
    io_system& _io;
};
