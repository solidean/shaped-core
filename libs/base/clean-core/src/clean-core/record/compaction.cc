#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/impl/block_builder.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/system.hh>
#include <clean-core/record/writer.hh>

// The compaction transforms of the recording algebra: scopes collapsed into summaries, stat lanes summarized per window.
//
// Both work per stream, one thread at one listener layer, because nesting and time order only hold within one.
// A stream's blocks are walked in recording order with the state carried across them, and each block is rebuilt in place,
// so the result keeps the blocks, their order and their metadata, and only their events change.

using namespace cc::primitive_defines;

namespace
{
using cc::rec::impl::block_builder;

/// A thread at a listener layer: the unit inside which events are ordered and scopes nest.
struct stream_key
{
    u32 thread_index = 0;
    u16 layer = 0;

    [[nodiscard]] bool operator==(stream_key const&) const = default;
};

[[nodiscard]] stream_key key_of(cc::rec::recorded_block const& b)
{
    return {.thread_index = b.thread_index, .layer = b.layer};
}

/// The indices of each stream's blocks, in recording order.
[[nodiscard]] cc::vector<cc::vector<isize>> streams_of(cc::span<cc::rec::recorded_block const> blocks)
{
    auto keys = cc::vector<stream_key>();
    auto streams = cc::vector<cc::vector<isize>>();
    for (auto i = isize(0); i < blocks.size(); ++i)
    {
        auto const key = key_of(blocks[i]);
        auto found = isize(-1);
        for (auto s = isize(0); s < keys.size(); ++s)
            if (keys[s] == key)
                found = s;
        if (found < 0)
        {
            found = keys.size();
            keys.push_back(key);
            streams.push_back({});
        }
        streams[found].push_back(i);
    }
    return streams;
}

/// Whether a site's name is its descriptor's, so a summary that keeps only the descriptor keeps the name too.
[[nodiscard]] bool has_static_name(cc::rec::desc const* d)
{
    return d != nullptr && d->name != nullptr && d->name[0] != char(0);
}

[[nodiscard]] cc::span<byte const> bytes_of(cc::rec::summary const& s)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(&s), isize(sizeof(s)));
}

[[nodiscard]] cc::rec::summary summary_of_one(f64 value, u64 begin_cycles, u64 end_cycles)
{
    return {
        .count = 1,
        .sum = value,
        .min = value,
        .max = value,
        .last = value,
        .begin_cycles = begin_cycles,
        .end_cycles = end_cycles,
    };
}

/// What the collapse decided for one event, found by its block and its byte offset there.
struct scope_mark
{
    u32 offset = 0;
    /// A collapsed scope's begin is dropped, and its end becomes the summary `summary` describes.
    bool is_end = false;
    cc::rec::desc const* site = nullptr;
    cc::rec::summary summary;
};

/// An open scope while the collapse walks a stream.
struct open_scope
{
    isize block = 0;
    u32 offset = 0;
    cc::rec::desc const* site = nullptr;
    u64 cycles = 0;
    i64 depth = 0;
};

/// One lane being summarized: a site inside one scope.
struct lane
{
    cc::rec::desc const* site = nullptr;
    u32 core = 0;
    u64 window = 0;
    cc::rec::summary summary;
};

/// The lanes open inside one scope, and the oldest window any of them is in.
struct lane_frame
{
    cc::vector<lane> lanes;
    u64 oldest_window = ~u64(0);
};
} // namespace

cc::rec::recording cc::rec::recording::scopes_collapsed(u64 below_cycles) const
{
    if (below_cycles == 0)
        return *this;

    // First the decisions, since a scope's begin may sit in an earlier block than the end that decides it.
    auto marks = cc::vector<cc::vector<scope_mark>>();
    marks.resize_to_defaulted(_blocks.size());

    for (auto const& stream : streams_of(_blocks))
    {
        auto open = cc::vector<open_scope>();
        for (auto const b : stream)
        {
            auto const v = _blocks[b].view();
            for (auto it = v.begin(); it != v.end(); ++it)
            {
                auto const e = *it;
                auto const offset = u32(it.position() - v.bytes.data());
                auto const depth = e.field_as_int("depth").value_or(-1);
                if (e.kind() == rec::event_kind::scope_begin)
                    open.push_back({.block = b, .offset = offset, .site = e.desc, .cycles = e.cycles, .depth = depth});
                else if (e.kind() == rec::event_kind::scope_end)
                {
                    // Matched by depth, not by the stack alone: a scope whose end the stream lost leaves its begin
                    // open, and an end whose begin came before the stream pairs with nothing.
                    while (!open.empty() && open.back().depth > depth)
                        open.remove_back();
                    if (open.empty() || open.back().depth != depth)
                        continue;
                    auto const begin = open.back();
                    open.remove_back();
                    auto const duration = e.cycles >= begin.cycles ? e.cycles - begin.cycles : u64(0);
                    if (duration >= below_cycles || !has_static_name(begin.site))
                        continue;
                    marks[begin.block].push_back({.offset = begin.offset});
                    marks[b].push_back({
                        .offset = offset,
                        .is_end = true,
                        .site = begin.site,
                        .summary = summary_of_one(f64(duration), begin.cycles, e.cycles),
                    });
                }
            }
        }
    }

    auto out = rec::recording();
    out._modules = _modules;
    for (auto b = isize(0); b < _blocks.size(); ++b)
    {
        auto& block_marks = marks[b];
        if (block_marks.empty())
        {
            out._blocks.push_back(_blocks[b]);
            continue;
        }
        cc::sort(block_marks, [](scope_mark const& l, scope_mark const& r) { return l.offset < r.offset; });

        auto const v = _blocks[b].view();
        auto builder = block_builder(_blocks[b]);
        auto next = isize(0);
        for (auto it = v.begin(); it != v.end(); ++it)
        {
            auto const e = *it;
            auto const offset = u32(it.position() - v.bytes.data());
            if (next < block_marks.size() && block_marks[next].offset == offset)
            {
                auto const& m = block_marks[next++];
                if (m.is_end)
                    builder.add({
                        .desc = m.site,
                        .cycles = e.cycles,
                        .core = e.core,
                        .flags = rec::impl::flag_summary,
                        .payload = bytes_of(m.summary),
                    });
                continue;
            }
            builder.add(e);
        }
        if (!builder.empty())
            out._blocks.push_back(builder.take());
    }
    return out;
}

cc::rec::recording cc::rec::recording::stats_summarized(u64 window_cycles) const
{
    if (window_cycles == 0)
        return *this;

    // Rebuilt in place, block by block, so a stream's state can carry across its blocks.
    auto rebuilt = cc::vector<rec::recorded_block>();
    rebuilt.resize_to_defaulted(_blocks.size());
    auto kept = cc::vector<bool>();
    kept.resize_to_filled(_blocks.size(), false);

    for (auto const& stream : streams_of(_blocks))
    {
        // The root frame stands for whatever was open before the stream starts, and is never popped.
        auto frames = cc::vector<lane_frame>();
        frames.push_back({});
        auto last_cycles = u64(0);

        auto const emit = [&](block_builder& builder, lane const& l)
        {
            // Written at the latest time seen so far, so the stream's timestamps never go back.
            // The summary's own begin and end say when what it folds happened.
            last_cycles = cc::max(last_cycles, l.summary.end_cycles);
            builder.add({
                .desc = l.site,
                .cycles = last_cycles,
                .core = l.core,
                .flags = rec::impl::flag_summary,
                .payload = bytes_of(l.summary),
            });
        };

        auto const flush_frame = [&](block_builder& builder, lane_frame& f)
        {
            for (auto const& l : f.lanes)
                emit(builder, l);
            f.lanes.clear();
            f.oldest_window = ~u64(0);
        };

        // Every lane of the innermost frame whose window has ended by `window`, oldest first.
        auto const flush_ended = [&](block_builder& builder, u64 window)
        {
            auto& f = frames.back();
            if (f.oldest_window >= window)
                return;
            auto staying = cc::vector<lane>();
            f.oldest_window = ~u64(0);
            for (auto const& l : f.lanes)
                if (l.window < window)
                    emit(builder, l);
                else
                {
                    staying.push_back(l);
                    f.oldest_window = cc::min(f.oldest_window, l.window);
                }
            f.lanes = cc::move(staying);
        };

        for (auto si = isize(0); si < stream.size(); ++si)
        {
            auto const b = stream[si];
            auto const v = _blocks[b].view();
            auto builder = block_builder(_blocks[b]);

            for (auto it = v.begin(); it != v.end(); ++it)
            {
                auto const e = *it;
                auto const window = e.cycles / window_cycles;
                flush_ended(builder, window);

                auto const kind = e.kind();
                auto const is_stat = kind == rec::event_kind::stat_snapshot || kind == rec::event_kind::stat_accumulate;
                if ((is_stat || kind == rec::event_kind::summary) && has_static_name(e.desc))
                {
                    auto folded = rec::summary{};
                    if (is_stat)
                        folded = summary_of_one(e.field_as_double("value").value_or(0), e.cycles, e.cycles);
                    else
                        folded = e.as_summary().value_or(rec::summary{});

                    auto& f = frames.back();
                    auto* found = static_cast<lane*>(nullptr);
                    for (auto& l : f.lanes)
                        if (l.site == e.desc)
                            found = &l;
                    if (found != nullptr && found->window != window)
                    {
                        emit(builder, *found);
                        found->summary = {};
                        found->window = window;
                    }
                    if (found == nullptr)
                        found = &f.lanes.push_back({.site = e.desc, .core = e.core, .window = window});
                    found->summary.merge(folded);
                    found->core = e.core;
                    f.oldest_window = cc::min(f.oldest_window, window);
                    continue;
                }

                if (kind == rec::event_kind::scope_begin)
                    frames.push_back({});
                else if (kind == rec::event_kind::scope_end)
                {
                    // A lane never outlives its scope, so what it folds stays attributed to that scope.
                    flush_frame(builder, frames.back());
                    if (frames.size() > 1)
                        frames.remove_back();
                }

                last_cycles = cc::max(last_cycles, e.cycles);
                builder.add(e);
            }

            // Whatever is still open goes out at the stream's end, innermost first.
            if (si == stream.size() - 1)
                for (auto f = frames.size() - 1; f >= 0; --f)
                    flush_frame(builder, frames[f]);

            if (!builder.empty())
            {
                rebuilt[b] = builder.take();
                kept[b] = true;
            }
        }
    }

    auto out = rec::recording();
    out._modules = _modules;
    for (auto b = isize(0); b < _blocks.size(); ++b)
        if (kept[b])
            out._blocks.push_back(cc::move(rebuilt[b]));
    return out;
}

cc::rec::recording cc::rec::recording::compacted(cc::rec::compaction_options const& options,
                                                 cc::rec::compaction_report* report) const
{
    auto const rate = options.cycles_per_second > 0 ? options.cycles_per_second : rec::cycles_per_second();
    auto const to_cycles = [rate](f64 secs) { return rate > 0 && secs > 0 ? u64(secs * rate) : u64(0); };

    // A threshold past the recording's whole span collapses everything it ever will, so a round that changes nothing
    // ends the rounds only then: below it, a larger threshold may still fold what this one could not.
    auto first = ~u64(0);
    auto last = u64(0);
    for_each_event(
        [&](rec::chunk_view const&, rec::event_view const& e)
        {
            first = cc::min(first, e.cycles);
            last = cc::max(last, e.cycles);
        });
    auto const span = last >= first ? last - first : u64(0);

    auto r = rec::compaction_report{.bytes_before = total_bytes()};
    auto current = *this;
    auto scope_secs = options.min_scope_secs;
    auto window_secs = options.stat_window_secs;

    while (r.rounds < cc::max(options.max_rounds, 1))
    {
        auto const before_bytes = current.total_bytes();
        auto const scope_cycles = to_cycles(scope_secs);
        auto const window_cycles = to_cycles(window_secs);
        current = current.scopes_collapsed(scope_cycles).stats_summarized(window_cycles);
        ++r.rounds;
        r.min_scope_secs = scope_secs;
        r.stat_window_secs = window_secs;

        auto const after_bytes = current.total_bytes();
        if (options.target_bytes <= 0)
            break;
        if (after_bytes <= options.target_bytes)
        {
            r.reached_target = true;
            break;
        }
        if (after_bytes == before_bytes && scope_cycles > span && window_cycles > span)
            break;
        scope_secs *= options.growth;
        window_secs *= options.growth;
    }

    r.bytes_after = current.total_bytes();
    if (report != nullptr)
        *report = r;
    return current;
}
