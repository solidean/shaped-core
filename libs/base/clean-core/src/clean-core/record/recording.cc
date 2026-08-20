#include "recording.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/system.hh>

using namespace cc::primitive_defines;

namespace
{
/// What a decimated recording says in place of what it dropped.
constexpr cc::rec::field dropped_span_fields[] = {
    {.name = "events", .type = cc::rec::type_code::u64_, .offset = 0, .size = 8},
    {.name = "begin_cycles", .type = cc::rec::type_code::u64_, .offset = 8, .size = 8},
    {.name = "end_cycles", .type = cc::rec::type_code::u64_, .offset = 16, .size = 8},
};

struct dropped_span_payload
{
    u64 events = 0;
    u64 begin_cycles = 0;
    u64 end_cycles = 0;
};

constexpr cc::rec::desc dropped_span_desc = {
    .kind = cc::rec::event_kind::dropped_span,
    .enable_bit = cc::rec::enable_bit_of(cc::rec::category::logging),
    .name = "record.dropped_span",
    .dom = &cc::rec::g_system_domain,
    .fields = dropped_span_fields,
    .field_count = 3,
    .fixed_payload_size = sizeof(dropped_span_payload),
};

/// Builds one synthesized block out of a sequence of events copied from `source`.
///
/// The events a filter keeps are no longer contiguous, so they are copied into a buffer of their own.
/// That also means the result stops pinning the chunk it came from, which is usually what a caller narrowing a
/// capture down wanted anyway.
struct block_builder
{
    explicit block_builder(cc::rec::recorded_block const& source) : _template(&source) {}

    void add(cc::rec::event_view const& e)
    {
        auto const header_bytes = isize(sizeof(cc::rec::impl::event_header));
        auto const total = cc::rec::impl::event_bytes_for(e.payload.size());

        auto const offset = _bytes.size();
        _bytes.resize_to_uninitialized(offset + total);

        auto header = cc::rec::impl::event_header{
            .desc = e.desc,
            .cycles = e.cycles,
            .payload_size = u32(e.payload.size()),
            .core = u16(e.core),
            .flags = e.flags,
        };
        cc::memcpy(_bytes.data() + offset, &header, sizeof(header));
        if (!e.payload.empty())
            cc::memcpy(_bytes.data() + offset + header_bytes, e.payload.data(), size_t(e.payload.size()));

        ++_count;
    }

    [[nodiscard]] bool empty() const { return _count == 0; }

    /// Hands back the finished block, leaving the builder empty.
    [[nodiscard]] cc::rec::recorded_block take()
    {
        auto const owned = cc::make_pinned_data(cc::move(_bytes)).reinterpret_as<byte const>();

        auto block = cc::rec::recorded_block{
            .owned = owned,
            .from = 0,
            .to = u32(owned.size()),
            .thread_id = _template->thread_id,
            .thread_index = _template->thread_index,
            .thread_name = _template->thread_name,
            .chunk_seq = _template->chunk_seq,
            .layer = _template->layer,
            .base_cycles = _template->base_cycles,
            .base_wall_secs = _template->base_wall_secs,
            .seal_cycles = _template->seal_cycles,
            .seal_wall_secs = _template->seal_wall_secs,
            .state_at_start = _template->state_at_start,
        };

        _bytes = {};
        _count = 0;
        return block;
    }

private:
    cc::rec::recorded_block const* _template;
    cc::vector<byte> _bytes;
    isize _count = 0;
};

/// One event plus where it came from, so a query can order across blocks.
struct located_event
{
    cc::rec::chunk_view view;
    cc::rec::event_view event;
};

/// Every event in a recording, in timestamp order.
/// Blocks arrive per thread, so anything comparing across threads has to sort first.
cc::vector<located_event> sorted_events(cc::rec::recording const& r)
{
    cc::vector<located_event> all;
    r.for_each_event([&](cc::rec::chunk_view const& v, cc::rec::event_view const& e) { all.push_back({v, e}); });
    cc::sort(all, [](located_event const& a, located_event const& b) { return a.event.cycles < b.event.cycles; });
    return all;
}
} // namespace

f64 cc::rec::scope_span::duration_secs() const
{
    auto const rate = rec::cycles_per_second();
    return rate > 0 ? f64(duration_cycles()) / rate : 0;
}

cc::span<byte const> cc::rec::recorded_block::bytes() const
{
    if (source)
        return cc::span<byte const>(source.get()->data + from, isize(to - from));
    return owned.span().subspan({.offset = isize(from), .size = isize(to - from)});
}

cc::rec::chunk_view cc::rec::recorded_block::view() const
{
    return {
        .source = source.get(),
        .thread = {.id = thread_id, .index = thread_index, .name = thread_name},
        .state_at_start = state_at_start,
        .bytes = bytes(),
        .chunk_seq = chunk_seq,
        .layer = layer,
        .base_cycles = base_cycles,
        .base_wall_secs = base_wall_secs,
        .seal_cycles = seal_cycles,
        .seal_wall_secs = seal_wall_secs,
    };
}

void cc::rec::recording::append(cc::rec::chunk_view const& view)
{
    if (view.source == nullptr || view.bytes.empty())
        return;

    auto* const c = const_cast<rec::chunk*>(view.source);
    auto const offset = u32(view.bytes.data() - c->data);

    _blocks.push_back({
        .source = rec::chunk_ref(c),
        .from = offset,
        .to = offset + u32(view.bytes.size()),
        .thread_id = view.thread.id,
        .thread_index = view.thread.index,
        .thread_name = cc::string(view.thread.name),
        .chunk_seq = view.chunk_seq,
        .layer = view.layer,
        .base_cycles = view.base_cycles,
        .base_wall_secs = view.base_wall_secs,
        .seal_cycles = view.seal_cycles,
        .seal_wall_secs = view.seal_wall_secs,
        .state_at_start = view.state_at_start,
    });
}

void cc::rec::recording::append(cc::rec::recording const& other)
{
    for (auto const& b : other._blocks)
        _blocks.push_back(b);
}

void cc::rec::recording::append_block(cc::rec::recorded_block block)
{
    _blocks.push_back(cc::move(block));
}

bool cc::rec::recording::empty() const
{
    for (auto const& b : _blocks)
        if (b.to > b.from)
            return false;
    return true;
}

isize cc::rec::recording::event_count() const
{
    isize n = 0;
    for (auto const& b : _blocks)
    {
        auto const v = b.view();
        for (auto it = v.begin(); it != v.end(); ++it)
            ++n;
    }
    return n;
}

void cc::rec::recording::for_each_event(cc::function_ref<void(cc::rec::chunk_view const&, cc::rec::event_view const&)> f) const
{
    for (auto const& b : _blocks)
    {
        auto const v = b.view();
        for (auto it = v.begin(); it != v.end(); ++it)
            f(v, *it);
    }
}

void cc::rec::recording::replay(cc::rec::listener& l) const
{
    for (auto const& b : _blocks)
        l.on_chunk(b.view());
    l.on_batch_end();
}

//
// The algebra
//

cc::rec::recording cc::rec::recording::filtered(
    cc::function_ref<bool(cc::rec::chunk_view const&, cc::rec::event_view const&)> pred) const
{
    rec::recording out;

    for (auto const& b : _blocks)
    {
        auto const v = b.view();
        auto builder = block_builder(b);

        for (auto it = v.begin(); it != v.end(); ++it)
            if (auto const e = *it; pred(v, e))
                builder.add(e);

        if (!builder.empty())
            out._blocks.push_back(builder.take());
    }

    return out;
}

cc::rec::recording cc::rec::recording::from_thread(cc::thread_id id) const
{
    return filtered([id](rec::chunk_view const& v, rec::event_view const&) { return v.thread.id == id; });
}

cc::rec::recording cc::rec::recording::from_domain(cc::rec::domain const* d) const
{
    return filtered([d](rec::chunk_view const&, rec::event_view const& e) { return e.domain() == d; });
}

cc::rec::recording cc::rec::recording::from_domain(cc::string_view name) const
{
    return filtered([name](rec::chunk_view const&, rec::event_view const& e)
                    { return cc::string_view(e.domain()->name()) == name; });
}

cc::rec::recording cc::rec::recording::of_kind(cc::rec::event_kind k) const
{
    return filtered([k](rec::chunk_view const&, rec::event_view const& e) { return e.kind() == k; });
}

cc::rec::recording cc::rec::recording::in_cycle_range(u64 begin_cycles, u64 end_cycles) const
{
    return filtered([=](rec::chunk_view const&, rec::event_view const& e)
                    { return e.cycles >= begin_cycles && e.cycles < end_cycles; });
}

cc::rec::recording cc::rec::recording::decimated(cc::rec::decimation_options const& options) const
{
    // Scopes still open at the cutoff are what a reader is usually sitting inside, so they survive whatever their age.
    cc::vector<rec::desc const*> surviving_opens;
    if (options.keep_open_scopes)
        for (auto const& s : scopes())
            if (s.begin_cycles < options.keep_from_cycles && (s.is_open || s.end_cycles >= options.keep_from_cycles))
                surviving_opens.push_back(s.desc);

    auto const keeps = [&](rec::event_view const& e)
    {
        if (e.cycles >= options.keep_from_cycles)
            return true;
        if (e.kind() != rec::event_kind::scope_begin)
            return false;

        for (auto const* d : surviving_opens)
            if (d == e.desc)
                return true;
        return false;
    };

    rec::recording out;

    for (auto const& b : _blocks)
    {
        auto const v = b.view();
        auto builder = block_builder(b);

        auto dropped = dropped_span_payload{};
        auto reported = false;

        for (auto it = v.begin(); it != v.end(); ++it)
        {
            auto const e = *it;
            if (keeps(e))
            {
                // The report goes in just before the first survivor, so it reads as "everything up to here is gone".
                if (options.report_dropped && dropped.events > 0 && !reported)
                {
                    builder.add({
                        .desc = &dropped_span_desc,
                        .cycles = dropped.end_cycles,
                        .payload = cc::span<byte const>(reinterpret_cast<byte const*>(&dropped), isize(sizeof(dropped))),
                    });
                    reported = true;
                }
                builder.add(e);
                continue;
            }

            if (dropped.events == 0)
                dropped.begin_cycles = e.cycles;
            dropped.end_cycles = e.cycles;
            ++dropped.events;
        }

        // A block with nothing but dropped events still says so, rather than vanishing.
        if (options.report_dropped && dropped.events > 0 && !reported)
            builder.add({
                .desc = &dropped_span_desc,
                .cycles = dropped.end_cycles,
                .payload = cc::span<byte const>(reinterpret_cast<byte const*>(&dropped), isize(sizeof(dropped))),
            });

        if (!builder.empty())
            out._blocks.push_back(builder.take());
    }

    return out;
}

//
// Queries
//

isize cc::rec::recording::count(cc::string_view name) const
{
    isize n = 0;
    for_each_event([&](rec::chunk_view const&, rec::event_view const& e)
                   { n += cc::string_view(e.name()) == name ? 1 : 0; });
    return n;
}

isize cc::rec::recording::count_of_kind(cc::rec::event_kind k) const
{
    isize n = 0;
    for_each_event([&](rec::chunk_view const&, rec::event_view const& e) { n += e.kind() == k ? 1 : 0; });
    return n;
}

cc::optional<f64> cc::rec::recording::first_value(cc::string_view name) const
{
    for (auto const& le : sorted_events(*this))
        if (cc::string_view(le.event.name()) == name)
            if (auto const v = le.event.field_as_double("value"); v.has_value())
                return v;
    return {};
}

cc::optional<f64> cc::rec::recording::last_value(cc::string_view name) const
{
    cc::optional<f64> found;
    for (auto const& le : sorted_events(*this))
        if (cc::string_view(le.event.name()) == name)
            if (auto const v = le.event.field_as_double("value"); v.has_value())
                found = v;
    return found;
}

cc::vector<f64> cc::rec::recording::values(cc::string_view name) const
{
    cc::vector<f64> out;
    for (auto const& le : sorted_events(*this))
        if (cc::string_view(le.event.name()) == name)
            if (auto const v = le.event.field_as_double("value"); v.has_value())
                out.push_back(v.value());
    return out;
}

cc::optional<cc::string> cc::rec::recording::first_text(cc::string_view name) const
{
    for (auto const& le : sorted_events(*this))
        if (cc::string_view(le.event.name()) == name)
            if (auto const t = le.event.field_as_text("value"); t.has_value())
                return cc::string(t.value());
    return {};
}

bool cc::rec::recording::contains_in_order(cc::span<cc::string_view const> names) const
{
    if (names.empty())
        return true;

    isize next = 0;
    for (auto const& le : sorted_events(*this))
    {
        if (cc::string_view(le.event.name()) == names[next])
            ++next;
        if (next == names.size())
            return true;
    }
    return false;
}

cc::vector<cc::string> cc::rec::recording::messages() const
{
    cc::vector<cc::string> out;
    for (auto const& le : sorted_events(*this))
    {
        if (le.event.kind() != rec::event_kind::log)
            continue;

        // A site with no format arguments keeps its text in the descriptor, so the payload is empty by design.
        out.push_back(cc::string(le.event.payload.empty() ? le.event.name() : le.event.payload_as_text()));
    }
    return out;
}

cc::vector<cc::rec::scope_span> cc::rec::recording::scopes() const
{
    cc::vector<rec::scope_span> out;

    // One open stack per thread, since a scope opens and closes on the thread that recorded it.
    struct open_scope
    {
        cc::thread_id thread;
        rec::desc const* desc;
        u64 cycles;
        u32 depth;
    };
    cc::vector<open_scope> open;

    for (auto const& le : sorted_events(*this))
    {
        auto const kind = le.event.kind();
        if (kind != rec::event_kind::scope_begin && kind != rec::event_kind::scope_end)
            continue;

        auto const depth = u32(le.event.field_as_int("depth").value_or(0));

        if (kind == rec::event_kind::scope_begin)
        {
            open.push_back(
                {.thread = le.view.thread.id, .desc = le.event.desc, .cycles = le.event.cycles, .depth = depth});
            continue;
        }

        // Match innermost-first within the thread, by name and depth: two scopes sharing a name still nest correctly
        // because the depth separates them.
        for (isize i = open.size() - 1; i >= 0; --i)
        {
            auto const& o = open[i];
            if (o.thread != le.view.thread.id || o.depth != depth)
                continue;
            if (cc::string_view(o.desc->name) != cc::string_view(le.event.name()))
                continue;

            out.push_back({
                .desc = o.desc,
                .begin_cycles = o.cycles,
                .end_cycles = le.event.cycles,
                .depth = depth,
                .thread = o.thread,
            });
            open.remove_at(i);
            break;
        }
    }

    // Whatever never closed is reported as open rather than dropped — a truncated stream is the normal case here.
    for (auto const& o : open)
        out.push_back({
            .desc = o.desc,
            .begin_cycles = o.cycles,
            .end_cycles = o.cycles,
            .depth = o.depth,
            .thread = o.thread,
            .is_open = true,
        });

    cc::sort(out, [](rec::scope_span const& a, rec::scope_span const& b) { return a.begin_cycles < b.begin_cycles; });
    return out;
}

cc::vector<cc::rec::scope_span> cc::rec::recording::scopes(cc::string_view name) const
{
    cc::vector<rec::scope_span> out;
    for (auto const& s : scopes())
        if (s.name() == name)
            out.push_back(s);
    return out;
}

cc::rec::recording cc::rec::recording_listener::take()
{
    return cc::move(_recording);
}
