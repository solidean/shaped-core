#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/binding/debug_site.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>

// SC_SHADER_DIAGNOSTICS, which a build without the root CMakeLists has not set.
#ifndef SG_SHADER_DIAGNOSTICS
#define SG_SHADER_DIAGNOSTICS 1
#endif

/// What shaders write their print and assert records into, and how they come back to the host.
///
/// A context has one window per epoch in flight and one more, and epoch `e` writes window `e % count`.
/// A window is read back once its epoch completes, decoded against the sites its pipelines stated, logged, and reset.
/// Opening the epoch that writes a window again waits for that, which only a drain more than a whole epoch late makes it do.
/// A context that never blocks cannot wait, so that epoch writes a sink instead, whose every record counts as dropped.
/// Nothing here orders two epochs against each other on the GPU: their overlap is what a dev build must still show.
/// libs/graphics/shaped-graphics-language/docs/issues/feat-sgl-gpu-print-and-assert.md has the design.
class sg::impl::debug_windows
{
public:
    /// The bytes one window holds, its three header words included.
    static constexpr isize k_window_bytes = 64 * 1024;
    static constexpr isize k_window_count = 4;
    /// SC_SHADER_DIAGNOSTICS: whether a backend binds a window at all, and slib asks SGL for records.
    static constexpr bool k_enabled = SG_SHADER_DIAGNOSTICS != 0;

    explicit debug_windows(context& ctx) : _ctx(ctx) {}

    /// The window the epoch being recorded writes, created on first use; safe from any recording thread.
    /// `writes` says the bound pipeline's code writes records, which has the window read back once the epoch completes.
    [[nodiscard]] raw_buffer_handle window_for_recording(bool writes);

    /// Creates the windows, where diagnostics are on; a backend calls it once the context exists.
    /// Creating them uploads nothing: a window's header is reset by the first bind that writes it.
    void create();

    /// Makes `sites` known, so a record that starts with one of their ids decodes; a pipeline's creation calls it.
    void add_sites(cc::span<shader_debug_site const> sites);

    /// The rest runs on the thread that advances and retires epochs, never beside it.

    /// Before `closing` is closed: has its window read back once it completes, where something wrote to it.
    void on_closing(epoch closing);
    /// Once `opened` is open: waits until its window is back from the epoch that last wrote it.
    void on_opened(epoch opened);
    /// Decodes and resets every window whose bytes have arrived; the retire sweep calls it.
    void poll();

    /// Whether a record written so far is still to be logged: the recording epoch's window was written, or a window is back from the GPU and not decoded yet.
    [[nodiscard]] bool has_records_due() const;
    /// Whether a pipeline that writes records was bound in the recording epoch, whose window is read back only once it closes.
    [[nodiscard]] bool is_recording_written() const;

    /// Waits for every window in flight and decodes it, for a context about to shut down.
    void drain();
    /// Drops the windows' buffers, which must not outlive the device.
    void release();

private:
    struct window
    {
        raw_buffer_handle buffer;
        /// A pipeline that writes records was bound in the epoch being recorded, so the window will be read back.
        cc::atomic<bool> is_written = false;
        /// Written in an epoch that has not been read back and reset yet, which a recording thread may ask from.
        cc::atomic<bool> is_busy = false;
        /// Its header has to be reset before a pipeline writes it: at creation, and once its records were read.
        /// Reset by the first bind that writes, so a context that never prints uploads nothing for it.
        cc::atomic<bool> needs_reset = true;
        /// Read back after its epoch, and not decoded yet; nullopt where nothing is pending.
        cc::optional<bytes_future> pending;
        /// Its download has been asked for, which only happens once its epoch has retired.
        bool is_requested = false;
        epoch written_in = epoch(0);
    };

    [[nodiscard]] bool has_windows() const { return _created.load(); }
    void ensure_windows();
    void request(isize index);
    void finish(isize index, cc::span<byte const> bytes);
    void reset(isize index);
    [[nodiscard]] isize index_of(epoch e) const { return isize(u64(e) % u64(k_window_count)); }

    context& _ctx;
    window _windows[k_window_count];
    /// Where an epoch whose window is still busy writes, on a context that cannot wait for it: room for nothing.
    raw_buffer_handle _sink;
    cc::atomic<bool> _sink_needs_reset = true;
    cc::atomic<bool> _created = false;
    cc::mutex<int> _creation;
    cc::mutex<cc::map<u32, shader_debug_site>> _sites;
};
