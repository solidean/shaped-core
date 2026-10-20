#include "dx12-test-common.hh"

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/mutex.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/transfer/stream_source.hh>

#include <atomic>

using namespace cc::primitive_defines;

// dx12 async-upload internals the backend-agnostic tier-1 suite (tests/transfer/upload-async-test.cc) can't reach.
// That is the staging pipeline's window packing and recycling, forced by shrinking the shared context's window for one test.
// The public upload/sync contract is pinned in tier 1.
// See libs/graphics/shaped-graphics/docs/testing.md and libs/graphics/shaped-graphics/docs/concepts/upload.async.md.

namespace
{
namespace dx12 = sg::backend::dx12;

// A pinned byte buffer filled by fn(i), moved into the pin (owns it, zero-copy).
cc::pinned_data<byte const> make_bytes(isize n, auto&& fn)
{
    cc::vector<byte> data;
    data.reserve(n);
    for (isize i = 0; i < n; ++i)
        data.push_back(byte(fn(i)));
    return cc::make_pinned_data(cc::move(data));
}

/// Opened from the test's thread, and outliving the source: once opened, the actor may finish and destroy the source before release() has returned.
struct gate
{
    std::atomic<bool> open = false;
    cc::mutex<cc::unique_function<void()>> waker; // installed on the actor thread

    void release()
    {
        open.store(true, std::memory_order_release);
        waker.lock(
            [](cc::unique_function<void()>& w)
            {
                if (w)
                    w();
            });
    }
};

/// A stream source that holds its one chunk back until its gate opens, so its transfer is in flight for as long as a test needs.
class gated_source final : public sg::stream_source
{
public:
    gated_source(cc::pinned_data<byte const> bytes, std::shared_ptr<gate> g)
      : _bytes(cc::move(bytes)), _gate(cc::move(g))
    {
    }

    [[nodiscard]] sg::stream_poll try_next_chunk() override
    {
        if (!_gate->open.load(std::memory_order_acquire))
            return {.status = sg::stream_source_status::not_yet};
        if (_handed_over)
            return {.status = sg::stream_source_status::done};
        _handed_over = true;
        return {.status = sg::stream_source_status::ready, .chunk = {.data = _bytes}};
    }

    void set_waker(cc::unique_function<void()> waker) override
    {
        _gate->waker.lock([&](cc::unique_function<void()>& w) { w = cc::move(waker); });
    }

private:
    cc::pinned_data<byte const> _bytes;
    std::shared_ptr<gate> _gate;
    bool _handed_over = false;
};
} // namespace

// A single upload larger than one staging window must pack across several windows, pipelining and recycling as it goes.
ASYNC_INVOCABLE_TEST("sg dx12 - async upload larger than a staging window packs across windows",
                     (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;
    auto const staging = dx12::scoped_transfer_sizes(c, {.async_upload_window_bytes = 4096});

    auto const n = isize(20000); // several windows, non-aligned so partial windows are exercised
    auto buf = c.persistent.create_raw_buffer(n, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    c.upload.bytes_to_buffer(buf, make_bytes(n, [](isize i) { return i * 7 + 1; }));

    auto down = c.create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.bytes_from_buffer(buf, 0, n);
    c.submit_command_list(cc::move(down));

    auto const bytes = co_await future.bytes();
    REQUIRE(bytes.size() == n);
    dx12::require_async_upload_window(c, 4096); // the guard's size applied, so the upload spans several windows
    bool matches = true;
    for (isize i = 0; i < n; ++i)
        if (bytes[i] != byte(i * 7 + 1))
            matches = false;
    CHECK(matches);
}

// A resource keeps the value of its last upload forever, so a list touching it would otherwise wait on that timeline in every frame.
ASYNC_INVOCABLE_TEST("sg dx12 - a list touching a buffer whose upload has landed makes no queue wait",
                     (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const n = isize(256);
    auto buf = c.persistent.create_raw_buffer(n, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);
    c.upload.bytes_to_buffer(buf, make_bytes(n, [](isize i) { return i; }));

    // A first read waits on the copy, so once its bytes are back the upload has landed.
    auto first = c.create_command_list();
    auto landed = first->download.bytes_from_buffer(buf, 0, n);
    c.submit_command_list(cc::move(first));
    REQUIRE((co_await landed.bytes()).size() == n);

    auto const before = c.metrics.stats();
    auto second = c.create_command_list();
    auto again = second->download.bytes_from_buffer(buf, 0, n);
    c.submit_command_list(cc::move(second));
    CHECK((c.metrics.stats() - before)[sg::stat::queue_waits] == 0);

    auto const bytes = co_await again.bytes();
    REQUIRE(bytes.size() == n);
    CHECK(bytes[n - 1] == byte(n - 1));
}

// Many uploads whose aggregate far exceeds the staging buffer must all land, forcing the actor to wait on the window fence and recycle windows repeatedly.
// Each targets its own buffer; all must read back intact.
// Window 0 shares its staging slot with window 3, so the window fence must observe window 0's completion before window 3 overwrites that slot.
// That aliasing is what the 1-based window-fence values guard — see submit_window in dx12_upload_async.cc.
ASYNC_INVOCABLE_TEST("sg dx12 - many async uploads recycle the staging windows",
                     (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;
    auto const staging = dx12::scoped_transfer_sizes(c, {.async_upload_window_bytes = 1024});

    int const count = 24;
    isize const each = 1024;
    cc::vector<sg::raw_buffer_handle> bufs;
    for (int k = 0; k < count; ++k)
    {
        auto buf = c.persistent.create_raw_buffer(each, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
        REQUIRE(buf != nullptr);
        c.upload.bytes_to_buffer(buf, make_bytes(each, [k](isize i) { return i + k; }));
        bufs.push_back(buf);
    }

    bool all_ok = true;
    for (int k = 0; k < count; ++k)
    {
        auto down = c.create_command_list();
        REQUIRE(down != nullptr);
        auto future = down->download.bytes_from_buffer(bufs[k], 0, each);
        c.submit_command_list(cc::move(down));

        auto const bytes = co_await future.bytes();
        for (isize i = 0; i < each; ++i)
            if (bytes[i] != byte(i + k))
                all_ok = false;
    }
    CHECK(all_ok);
    dx12::require_async_upload_window(c, 1024); // the guard's size applied, so the uploads recycle windows
}

// Uneven upload sizes (none a window multiple) force the actor to both pack several jobs into one window and split a single job across windows, all while recycling.
// That is a shape the exact-fill and single-large-upload tests miss.
// Distinct buffers; each must read back intact.
ASYNC_INVOCABLE_TEST("sg dx12 - uneven async uploads pack and straddle staging windows",
                     (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;
    auto const staging = dx12::scoped_transfer_sizes(c, {.async_upload_window_bytes = 1024});

    // Sizes chosen so windows carry parts of two uploads and single uploads span windows.
    // The aggregate spans several windows, so slots recycle.
    // Byte k*13+7 keeps each buffer's pattern distinct.
    isize const sizes[] = {700, 300, 900, 1500, 200, 1100, 640, 1300, 480, 760};
    int const count = 10;
    cc::vector<sg::raw_buffer_handle> bufs;
    for (int k = 0; k < count; ++k)
    {
        isize const n = sizes[k];
        auto buf = c.persistent.create_raw_buffer(n, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
        REQUIRE(buf != nullptr);
        c.upload.bytes_to_buffer(buf, make_bytes(n, [k](isize i) { return i * (k * 13 + 7); }));
        bufs.push_back(buf);
    }

    bool all_ok = true;
    for (int k = 0; k < count; ++k)
    {
        isize const n = sizes[k];
        auto down = c.create_command_list();
        REQUIRE(down != nullptr);
        auto future = down->download.bytes_from_buffer(bufs[k], 0, n);
        c.submit_command_list(cc::move(down));

        auto const bytes = co_await future.bytes();
        REQUIRE(bytes.size() == n);
        for (isize i = 0; i < n; ++i)
            if (bytes[i] != byte(i * (k * 13 + 7)))
                all_ok = false;
    }
    CHECK(all_ok);
    dx12::require_async_upload_window(c, 1024); // the guard's size applied, so the uploads recycle windows
}

// A transfer still in flight at submit is one queue wait, which the skip above must not swallow.
// The stream's source holds its bytes back, so the wait is certain however fast the copy queue is.
ASYNC_TEST("sg dx12 - a list touching a buffer whose transfer is in flight makes one queue wait")
{
    auto ctx = dx12::make_test_context({});
    if (ctx.has_error())
        SKIP("no dx12 adapter");
    auto& c = *ctx.value();

    auto const n = isize(256);
    auto buf = c.persistent.create_raw_buffer(n, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);
    auto const opener = std::make_shared<gate>();
    auto stream = c.stream.from_source_to_buffer(
        buf, std::make_unique<gated_source>(make_bytes(n, [](isize i) { return i * 3 + 1; }), opener));
    // The list below waits on purpose, so the stream does not warn about it.
    stream.promote_to_async();

    auto const before = c.metrics.stats();
    auto list = c.create_command_list();
    auto readback = list->download.bytes_from_buffer(buf, 0, n);
    c.submit_command_list(cc::move(list));
    CHECK((c.metrics.stats() - before)[sg::stat::queue_waits] == 1);

    // The list ran after the stream landed, so it reads the streamed bytes.
    opener->release();
    auto const bytes = co_await readback.bytes();
    REQUIRE(bytes.size() == n);
    CHECK(bytes[0] == byte(1));
    CHECK(bytes[n - 1] == byte((n - 1) * 3 + 1));
}
