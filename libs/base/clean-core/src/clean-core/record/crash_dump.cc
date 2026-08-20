#include "crash_dump.hh"

#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/error/crash_handler.hh>
#include <clean-core/record/chunk.hh>
#include <clean-core/record/impl/serialized_format.hh>
#include <clean-core/record/impl/system_state.hh>
#include <clean-core/record/impl/thread_state.hh>
#include <clean-core/record/serialize.hh>
#include <clean-core/record/system.hh>
#include <clean-core/record/writer.hh>
#include <clean-core/streams/impl/native_file.hh>

namespace
{
using namespace cc::primitive_defines;

/// The longest path a dump will remember.
/// A fixed buffer rather than a cc::string, because copying one inside a crash handler would allocate.
constexpr isize max_path_bytes = 1024;

struct installed_dump
{
    bool is_installed = false;
    char path[max_path_bytes] = {};
    isize path_length = 0;
    isize max_event_bytes = 0;
    bool seal_calling_thread = true;

    /// Reserved at install time.
    /// Everything the builder needs comes out of this.
    cc::vector<byte> arena;
};

installed_dump g_dump;

/// Writes `bytes` in full, looping over short writes.
bool write_all(cc::impl::native_file& file, cc::span<byte const> bytes)
{
    isize written = 0;
    while (written < bytes.size())
    {
        auto n = file.write(cc::span<byte const>(bytes.data() + written, bytes.size() - written));
        if (n.has_error() || n.value() <= 0)
            return false;
        written += n.value();
    }
    return true;
}

/// Walks one thread's chunk queue, offering every published block to `f`.
///
/// Reads only up to each chunk's committed watermark, which is release-stored after the bytes it covers.
/// So a live thread can never hand back a torn event, and none of this needs the thread stopped.
void for_each_published_block(cc::rec::impl::thread_state const& ts, cc::function_ref<bool(cc::rec::chunk_view const&)> f)
{
    auto const info = cc::rec::thread_info{.id = ts.tid, .index = ts.index, .name = cc::string_view(ts.name)};

    for (auto const* c = ts.queue_head.load(cc::memory_order_acquire); c != nullptr;
         c = c->next_in_thread.load(cc::memory_order_acquire))
    {
        auto const committed = c->committed.load(cc::memory_order_acquire);
        if (committed == 0)
            continue;

        auto const view = cc::rec::chunk_view{
            .source = c,
            .thread = info,
            .state_at_start = nullptr,
            .bytes = cc::span<byte const>(c->data, isize(committed)),
            .chunk_seq = c->seq,
            .layer = c->layer,
            .base_cycles = c->base_cycles,
            .base_wall_secs = c->base_wall_secs,
            .seal_cycles = c->seal_cycles,
            .seal_wall_secs = c->seal_wall_secs,
        };

        if (!f(view))
            return;
    }
}

/// The whole dump, allocation-free from here down.
bool write_dump()
{
    if (!g_dump.is_installed || !cc::rec::is_initialized())
        return false;

    if (g_dump.seal_calling_thread)
        cc::rec::seal_current_thread_chunk();

    auto builder = cc::rec::impl::dump_builder(cc::span<byte>(g_dump.arena));
    builder.set_meta(cc::current_time_wall_secs(), cc::rec::cycles_per_second());

    isize event_bytes = 0;
    cc::rec::impl::for_each_thread_state(
        [&](cc::rec::impl::thread_state& ts)
        {
            for_each_published_block(ts,
                                     [&](cc::rec::chunk_view const& view)
                                     {
                                         if (event_bytes + view.bytes.size() > g_dump.max_event_bytes)
                                             return false;
                                         if (!builder.add_block(view))
                                             return false;
                                         event_bytes += view.bytes.size();
                                         return true;
                                     });
        });

    auto const parts = builder.finish();

    auto file = cc::impl::native_file::open(cc::string_view(g_dump.path, g_dump.path_length),
                                            cc::impl::file_mode::write_truncate);
    if (file.has_error())
        return false;

    auto const header_bytes
        = cc::span<byte const>(reinterpret_cast<byte const*>(&parts.header), isize(sizeof(parts.header)));
    if (!write_all(file.value(), header_bytes) || !write_all(file.value(), parts.strings)
        || !write_all(file.value(), parts.domains) || !write_all(file.value(), parts.units)
        || !write_all(file.value(), parts.fields) || !write_all(file.value(), parts.descs)
        || !write_all(file.value(), parts.threads) || !write_all(file.value(), parts.blocks))
        return false;

    // The event bytes go out straight from the chunks, with the descriptor pointer in each header rewritten into its
    // table index on the way.
    // The rewrite happens in a small stack buffer, one event at a time, because the chunk itself belongs to a thread
    // that may still be running.
    for (isize i = 0; i < builder.block_count(); ++i)
    {
        auto const source = builder.block_at(i);

        isize offset = 0;
        while (offset < isize(source.size))
        {
            auto header = cc::rec::impl::event_header{};
            cc::memcpy(&header, source.data + offset, sizeof(header));

            auto const payload = isize(header.payload_size);
            auto const index = builder.desc_index_of_pointer(header.desc);
            header.desc = reinterpret_cast<cc::rec::desc const*>(uintptr_t(index));

            if (!write_all(file.value(),
                           cc::span<byte const>(reinterpret_cast<byte const*>(&header), isize(sizeof(header)))))
                return false;

            auto const rest = cc::rec::impl::event_bytes_for(payload) - isize(sizeof(header));
            if (rest > 0 && !write_all(file.value(), cc::span<byte const>(source.data + offset + sizeof(header), rest)))
                return false;

            offset += isize(sizeof(header)) + rest;
        }
    }

    return true;
}

void crash_hook() noexcept
{
    (void)write_dump();
}
} // namespace

void cc::rec::install_crash_dump(cc::rec::crash_dump_options const& options)
{
    auto const already_installed = g_dump.is_installed;

    g_dump.path_length = cc::min(options.path.size(), max_path_bytes - 1);
    for (isize i = 0; i < g_dump.path_length; ++i)
        g_dump.path[i] = options.path[i];
    g_dump.path[g_dump.path_length] = '\0';

    g_dump.max_event_bytes = options.max_event_bytes;
    g_dump.seal_calling_thread = options.seal_calling_thread;

    // Reserved here, and never touched again except by the dump itself.
    g_dump.arena.resize_to_uninitialized(options.arena_bytes);
    g_dump.is_installed = true;

    if (!already_installed)
        cc::add_crash_context_hook(&crash_hook);
}

bool cc::rec::write_crash_dump_now()
{
    return write_dump();
}

cc::string_view cc::rec::crash_dump_path()
{
    return g_dump.is_installed ? cc::string_view(g_dump.path, g_dump.path_length) : cc::string_view();
}
