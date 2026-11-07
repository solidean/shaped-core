#include <clean-core/container/map.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/platform/file_access.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/mutex.hh>

using namespace cc::primitive_defines;

namespace cc::file_access::impl
{
cc::atomic<bool> g_recording = {false};
} // namespace cc::file_access::impl

namespace
{
using cc::file_access::impl::access;
using cc::file_access::impl::counters;

/// One recorded (context, path).
/// Never freed: a stream holds its counters for as long as it is open, which may outlive any reset a caller could ask for.
struct slot
{
    u64 context = 0;
    cc::string path;
    bool read = false;
    bool written = false;
    bool listed = false;
    counters bytes;
};

/// Keyed by context and path, so one test's reads never merge with another's.
auto g_slots = cc::mutex<cc::map<cc::string, cc::unique_ptr<slot>>>();

auto g_hook = cc::atomic<cc::file_access::context_hook>(nullptr);
} // namespace

counters* cc::file_access::impl::note_slow(access kind, cc::string_view path)
{
    auto const context = cc::file_access::current_context();
    auto key = cc::format("{}|{}", context, path);
    return g_slots.lock(
        [&](cc::map<cc::string, cc::unique_ptr<slot>>& slots) -> counters*
        {
            auto& s = slots[key];
            if (s == nullptr)
            {
                s = cc::make_unique<slot>();
                s->context = context;
                s->path = cc::string(path);
            }
            switch (kind)
            {
            case access::read:
                s->read = true;
                break;
            case access::write:
                s->written = true;
                break;
            case access::list:
                s->listed = true;
                break;
            }
            return &s->bytes;
        });
}

void cc::file_access::start_recording()
{
    impl::g_recording.store(true, cc::memory_order_relaxed);
}

void cc::file_access::stop_recording()
{
    impl::g_recording.store(false, cc::memory_order_relaxed);
}

void cc::file_access::set_context_hook(context_hook hook)
{
    g_hook.store(hook, cc::memory_order_release);
}

u64 cc::file_access::current_context()
{
    auto const hook = g_hook.load(cc::memory_order_acquire);
    return hook == nullptr ? 0 : hook();
}

cc::vector<cc::file_access::record> cc::file_access::snapshot()
{
    return g_slots.lock(
        [](cc::map<cc::string, cc::unique_ptr<slot>>& slots)
        {
            auto out = cc::vector<record>();
            for (auto const& entry : slots)
            {
                auto const& s = entry.value;
                auto& r = out.emplace_back();
                r.context = s->context;
                r.path = s->path;
                r.read = s->read;
                r.written = s->written;
                r.listed = s->listed;
                r.bytes_read = s->bytes.bytes_read.load(cc::memory_order_relaxed);
                r.bytes_written = s->bytes.bytes_written.load(cc::memory_order_relaxed);
            }
            return out;
        });
}
