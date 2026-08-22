#include "recording.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/pair.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/overhead.hh>
#include <clean-core/record/system.hh>
#include <clean-core/record/trace.hh>

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

    /// Where this event sat in capture order, which is what breaks a timestamp tie.
    isize index = 0;
};

/// Every event in a recording, in timestamp order.
/// Blocks arrive per thread, so anything comparing across threads has to sort first.
///
/// **Capture order breaks a tie, because a tie is normal rather than exotic.**
/// `cc::sort` is deterministic but not stable, and a tick is coarse enough on some platforms that adjacent events
/// routinely share one — 42 ns on Apple silicon against a scope that took 20.
/// Without the tiebreaker a scope's begin could sort after the begin it encloses, and the pairing below would nest
/// them the wrong way round from data that was never ambiguous: within a thread the stream IS the order.
cc::vector<located_event> sorted_events(cc::rec::recording const& r)
{
    cc::vector<located_event> all;
    r.for_each_event([&](cc::rec::chunk_view const& v, cc::rec::event_view const& e)
                     { all.push_back({v, e, all.size()}); });

    cc::sort(all,
             [](located_event const& a, located_event const& b)
             {
                 if (a.event.cycles != b.event.cycles)
                     return a.event.cycles < b.event.cycles;
                 return a.index < b.index;
             });
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
    if (view.bytes.empty())
        return;

    // A view with no chunk behind it is a SYNTHESIZED block — what a filter, a decimation or a live splice produces.
    // Its bytes belong to whoever built it and are only guaranteed for this call, so retaining it means copying.
    // Dropping it instead is the alternative, and it would lose a spliced stream silently.
    if (view.source == nullptr)
    {
        cc::vector<byte> copy;
        copy.push_back_range(view.bytes);

        _blocks.push_back({
            .owned = cc::make_pinned_data(cc::move(copy)).reinterpret_as<byte const>(),
            .from = 0,
            .to = u32(view.bytes.size()),
            .thread_id = view.thread.id,
            .thread_index = view.thread.index,
            .thread_name = cc::string(view.thread.name),
            .chunk_seq = view.chunk_seq,
            .layer = view.layer,
            .base_cycles = view.base_cycles,
            .base_wall_secs = view.base_wall_secs,
            .seal_cycles = view.seal_cycles,
            .seal_wall_secs = view.seal_wall_secs,
        });
        return;
    }

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

namespace
{
/// One sample lifted out of the sideband, with the anchor it has to go back to.
struct anchored_sample
{
    u32 thread_index = 0;
    u64 chunk_seq = 0;
    u32 chunk_offset = 0;
    cc::rec::event_view event;
    bool placed = false;
};

/// Where in its chunk the event under `it` begins.
/// A block's `from` is a chunk offset, so this is what an anchor is comparable against.
[[nodiscard]] u32 chunk_offset_of(cc::rec::recorded_block const& b, byte const* position)
{
    return b.from + u32(position - b.bytes().data());
}
} // namespace

cc::rec::recording cc::rec::recording::spliced_samples() const
{
    // Collect first, because a sample has to be matched against a block that may come before or after it.
    //
    // **Only out of a chunk-backed block.**
    // A sample sitting in a synthesized one was put there by an earlier splice — offline or by splicing_listener — and
    // lifting it out again would take it away from the position that splice worked out for it.
    // That is what makes splicing idempotent in POSITION rather than merely in count.
    cc::vector<anchored_sample> samples;
    for (auto const& b : _blocks)
    {
        if (!b.source)
            continue;

        auto const v = b.view();
        for (auto it = v.begin(); it != v.end(); ++it)
        {
            auto const e = *it;
            if (e.kind() != rec::event_kind::sample)
                continue;

            samples.push_back({
                .thread_index = u32(e.field_as_u64("thread_index").value_or(0)),
                .chunk_seq = e.field_as_u64("chunk_seq").value_or(0),
                .chunk_offset = u32(e.field_as_u64("chunk_offset").value_or(0)),
                .event = e,
            });
        }
    }

    if (samples.empty())
        return *this;

    rec::recording out;

    for (auto const& b : _blocks)
    {
        // A synthesized block has no chunk identity, so nothing can be anchored into it — and whatever it already
        // carries is where a previous splice decided it goes.
        // It passes through untouched, which is the other half of idempotence.
        // A synthesized block has no chunk identity, so nothing can be anchored into it — and whatever it already
        // carries is where a previous splice decided it goes.
        // It passes through untouched, which is what keeps a MIXED recording — live-spliced blocks alongside fresh
        // chunk-backed ones — idempotent rather than only a wholly-spliced one.
        if (!b.source)
        {
            out._blocks.push_back(b);
            continue;
        }

        // Which samples land in this block, in the order they land.
        cc::vector<isize> incoming;
        for (isize i = 0; i < samples.size(); ++i)
        {
            auto& s = samples[i];
            if (s.placed || s.thread_index != b.thread_index || s.chunk_seq != b.chunk_seq)
                continue;
            if (s.chunk_offset < b.from || s.chunk_offset > b.to)
                continue;
            incoming.push_back(i);
        }

        cc::sort(incoming, [&](isize a, isize c) { return samples[a].chunk_offset < samples[c].chunk_offset; });

        // Which sample events this block itself carries, so a spliced one is not left behind as well.
        auto const carries_samples = [&]
        {
            auto const v = b.view();
            for (auto it = v.begin(); it != v.end(); ++it)
                if ((*it).kind() == rec::event_kind::sample)
                    return true;
            return false;
        }();

        if (incoming.empty() && !carries_samples)
        {
            out._blocks.push_back(b);
            continue;
        }

        auto const v = b.view();
        auto builder = block_builder(b);
        auto next = isize(0);

        for (auto it = v.begin(); it != v.end(); ++it)
        {
            auto const offset = chunk_offset_of(b, it.position());
            while (next < incoming.size() && samples[incoming[next]].chunk_offset <= offset)
            {
                builder.add(samples[incoming[next]].event);
                samples[incoming[next]].placed = true;
                ++next;
            }

            auto const e = *it;
            if (e.kind() == rec::event_kind::sample)
                continue; // held back until every block has been offered it

            builder.add(e);
        }

        // Anything anchored past the last event belongs at the end.
        while (next < incoming.size())
        {
            builder.add(samples[incoming[next]].event);
            samples[incoming[next]].placed = true;
            ++next;
        }

        if (!builder.empty())
            out._blocks.push_back(builder.take());
    }

    // A sample nobody could place — its bytes were dropped, or simply never captured — is kept rather than dropped.
    // It comes back on its recording thread's own lane instead of the thread it caught, which is the honest answer:
    // where it was taken is the only position this recording can still justify.
    cc::vector<isize> unplaced;
    for (isize i = 0; i < samples.size(); ++i)
        if (!samples[i].placed)
            unplaced.push_back(i);

    if (!unplaced.empty())
        for (auto const& b : _blocks)
        {
            // Synthesized blocks went out whole above, samples included, so re-adding from one would duplicate.
            if (!b.source)
                continue;

            auto const v = b.view();
            auto builder = block_builder(b);

            for (auto it = v.begin(); it != v.end(); ++it)
            {
                auto const e = *it;
                if (e.kind() != rec::event_kind::sample)
                    continue;

                for (auto const i : unplaced)
                    if (samples[i].event.payload.data() == e.payload.data())
                    {
                        builder.add(e);
                        break;
                    }
            }

            if (!builder.empty())
                out._blocks.push_back(builder.take());
        }

    return out;
}

cc::rec::recording cc::rec::recording::from_trace(cc::rec::trace_id id) const
{
    // Trace membership is carried forward per thread, because a thread publishes a delta rather than tagging events.
    //
    // Two kinds set it: a chunk's preamble, which states the trace outright, and an ambient delta, which changes it.
    // The preamble is why a capture that starts mid-trace still attributes — before it existed, a recording missing
    // the original delta attributed nothing at all.
    cc::map<cc::thread_id, rec::trace_id> current;

    return filtered(
        [&](rec::chunk_view const& v, rec::event_view const& e)
        {
            auto& running = current[v.thread.id];

            if (e.kind() == rec::event_kind::ambient_changed || e.kind() == rec::event_kind::stream_state)
            {
                // Read as a raw u64: a trace id is opaque, and a double would quietly lose everything past 2^53.
                running = rec::trace_id(e.field_as_u64("trace").value_or(0));

                // Either belongs to the context it names, so entering a trace is visible inside it.
                return running == id;
            }

            return running == id;
        });
}

cc::vector<cc::rec::trace_relation> cc::rec::recording::trace_relations() const
{
    cc::vector<rec::trace_relation> out;
    for (auto const& le : sorted_events(*this))
    {
        if (le.event.kind() != rec::event_kind::trace_relation)
            continue;

        auto edge = rec::trace_relation{.type = le.event.relation(), .cycles = le.event.cycles};
        for (auto const id : le.event.field_as_u64_array("members"))
            edge.members.push_back(rec::trace_id(id));

        out.push_back(cc::move(edge));
    }
    return out;
}

f64 cc::rec::recording::estimated_overhead_cycles() const
{
    auto const& model = rec::overhead();

    auto total = 0.0;
    for_each_event(
        [&](rec::chunk_view const&, rec::event_view const& e)
        {
            // An event that measured itself beats the model: a stacktrace capture costs orders of magnitude more than
            // the line would predict, which is exactly why it carries its own end timestamp.
            if ((e.flags & rec::impl::flag_has_end_cycles) != 0 && e.payload.size() >= 8)
            {
                u64 end_cycles = 0;
                cc::memcpy(&end_cycles, e.payload.data(), sizeof(end_cycles));
                if (end_cycles > e.cycles)
                {
                    total += f64(end_cycles - e.cycles);
                    return;
                }
            }

            total += model.fixed_cycles + model.cycles_per_byte * f64(e.payload.size());
        });

    return total;
}

f64 cc::rec::recording::estimated_overhead_ratio() const
{
    // Summed over threads rather than wall-clock: overhead is spent ON threads, so two threads recording for a second
    // each had two thread-seconds to spend it in.
    cc::map<cc::thread_id, cc::pair<u64, u64>> spans;

    for_each_event(
        [&](rec::chunk_view const& v, rec::event_view const& e)
        {
            auto& span = spans[v.thread.id];
            if (span.first == 0 || e.cycles < span.first)
                span.first = e.cycles;
            if (e.cycles > span.second)
                span.second = e.cycles;
        });

    auto total_span = 0.0;
    for (auto const& [thread, span] : spans)
        if (span.second > span.first)
            total_span += f64(span.second - span.first);

    return total_span > 0 ? estimated_overhead_cycles() / total_span : 0.0;
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

    // Depth breaks a tie, for the same reason capture order breaks one in sorted_events: two scopes opening within one
    // tick are not ambiguous — the outer one is the shallower one.
    cc::sort(out,
             [](rec::scope_span const& a, rec::scope_span const& b)
             {
                 if (a.begin_cycles != b.begin_cycles)
                     return a.begin_cycles < b.begin_cycles;
                 return a.depth < b.depth;
             });
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

namespace
{
/// When a block's coverage ENDED, in wall time.
///
/// A live block has not been sealed, so its base is the only time it has; using zero there would make every unsealed
/// block look infinitely old and evict exactly the events a bounded capture exists to keep.
[[nodiscard]] f64 block_end_wall_secs(cc::rec::recorded_block const& b)
{
    return b.seal_wall_secs > 0 ? b.seal_wall_secs : b.base_wall_secs;
}
} // namespace

isize cc::rec::recording::total_bytes() const
{
    isize n = 0;
    for (auto const& b : _blocks)
        n += b.bytes().size();
    return n;
}

isize cc::rec::recording::trim(rec::retention_policy const& policy)
{
    if (policy.keeps_everything() || _blocks.empty())
        return 0;

    // Newest across every thread rather than per thread: a thread that went quiet must not drag its own window along
    // behind the rest of the process.
    auto newest = 0.0;
    for (auto const& b : _blocks)
        newest = cc::max(newest, block_end_wall_secs(b));

    cc::vector<bool> keep;
    keep.reserve(_blocks.size());
    for (isize i = 0; i < _blocks.size(); ++i)
        keep.push_back(true);

    // The age limit first, because it is the stricter promise: nothing older than max_secs is kept whatever the byte
    // budget would have allowed.
    if (policy.max_secs > 0)
    {
        auto const floor = newest - policy.max_secs;
        for (isize i = 0; i < _blocks.size(); ++i)
            if (block_end_wall_secs(_blocks[i]) < floor)
                keep[i] = false;
    }

    if (policy.max_bytes > 0)
    {
        isize held = 0;
        for (isize i = 0; i < _blocks.size(); ++i)
            if (keep[i])
                held += _blocks[i].bytes().size();

        if (held > policy.max_bytes)
        {
            // Oldest first, which is the only eviction order that leaves a usable window rather than a sieve.
            cc::vector<isize> order;
            order.reserve(_blocks.size());
            for (isize i = 0; i < _blocks.size(); ++i)
                if (keep[i])
                    order.push_back(i);

            cc::sort(order, [&](isize a, isize b)
                     { return block_end_wall_secs(_blocks[a]) < block_end_wall_secs(_blocks[b]); });

            // The guarantee is what makes a byte cap safe for forensics: without it a burst of logging evicts the
            // seconds before a crash, which is the only part anybody wanted.
            auto const guaranteed_floor = policy.guaranteed_secs > 0 ? newest - policy.guaranteed_secs : newest + 1;

            for (auto const i : order)
            {
                if (held <= policy.max_bytes)
                    break;
                if (block_end_wall_secs(_blocks[i]) >= guaranteed_floor)
                    continue; // promised, so the cap is the thing that gives

                keep[i] = false;
                held -= _blocks[i].bytes().size();
            }
        }
    }

    cc::vector<rec::recorded_block> kept;
    kept.reserve(_blocks.size());
    isize dropped = 0;
    for (isize i = 0; i < _blocks.size(); ++i)
    {
        if (keep[i])
            kept.push_back(cc::move(_blocks[i])); // order preserved, so a trimmed recording replays like any other
        else
            ++dropped;
    }

    _blocks = cc::move(kept);
    return dropped;
}

cc::rec::recording cc::rec::recording::retained(rec::retention_policy const& policy) const
{
    auto out = *this;
    out.trim(policy);
    return out;
}

void cc::rec::recording_listener::on_chunk(rec::chunk_view const& view)
{
    _recording.append(view);

    // As chunks arrive rather than on read, so the listener never holds more than the policy allows even for a moment.
    if (!_policy.keeps_everything())
        _dropped_blocks += _recording.trim(_policy);
}

cc::rec::recording cc::rec::recording_listener::take()
{
    return cc::move(_recording);
}
