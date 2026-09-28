#pragma once

#include "fwd.hh"

#include <clean-core/record/fwd.hh>
#include <clean-core/record/listener.hh>
#include <clean-core/thread/atomic.hh>

/// Every log record at or above a level, sent to the client as `window/logMessage`, prefixed with its domain.
///
/// Until the server has answered `initialize` a record is written to stderr instead, which the editor shows in the
/// same output channel, so a server that dies while starting still says why.
/// It is one or the other and never both, so no line appears twice.
/// Records are drained on the recorder's own thread, or wherever a flush runs; `post_log` is what hands them over.
class lsp::log_listener final : public cc::rec::listener
{
public:
    explicit log_listener(server& s) : _server(&s) {}

    /// The lowest level forwarded, info by default, and safe to set from any thread.
    void set_min_level(cc::rec::level l) { _min_level.store(u8(l)); }

    void on_chunk(cc::rec::chunk_view const& view) override;
    [[nodiscard]] cc::string_view listener_name() const override { return "lsp window/logMessage"; }

private:
    server* _server;
    cc::atomic<u8> _min_level = u8(cc::rec::level::info);
};

namespace lsp
{
/// `trace`, `debug`, `info`, `warning`, `error`, as a client setting spells them; nothing for any other text.
[[nodiscard]] cc::optional<cc::rec::level> level_of(cc::string_view name);
} // namespace lsp
