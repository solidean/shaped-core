#include "debug_windows.hh"

#include <clean-core/math/bit.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/binding/reserved_bindings.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/types.hh>

using namespace sg;

namespace
{
/// The header a window starts with, in words: where the next record goes, how many words it holds, and how many records it had no room for.
constexpr isize k_header_words = debug_header_words;

[[nodiscard]] cc::string text_of_float(f32 f)
{
    if (f != f)
        return cc::string("nan");
    if (f - f != 0.0f)
        return cc::string(f < 0.0f ? "-inf" : "inf");
    // `1.0` and not `1`, as the interpreter writes a float
    auto text = cc::format("{}", f);
    if (!text.contains('.') && !text.contains('e'))
        text += ".0";
    return text;
}

[[nodiscard]] cc::string text_of_lane(debug_scalar scalar, u32 word)
{
    switch (scalar)
    {
    case debug_scalar::float32:
    case debug_scalar::float16:
        return text_of_float(cc::bit_cast<f32>(word));
    case debug_scalar::int32:
        return cc::format("{}", cc::bit_cast<i32>(word));
    case debug_scalar::uint32:
        return cc::format("{}", word);
    case debug_scalar::boolean:
        return cc::string(word != 0 ? "true" : "false");
    }
    return {};
}

/// `float3(1.0, 2.5, -3.0)` for a vector and the bare lane for a scalar, as the interpreter writes a value.
[[nodiscard]] cc::string text_of_value(debug_value const& value, cc::span<u32 const> words)
{
    if (value.lanes == 1)
        return text_of_lane(value.scalar, words[0]);
    auto text = cc::format("{}(", value.type);
    for (auto i = isize(0); i < value.lanes; ++i)
        text.appendf("{}{}", i == 0 ? "" : ", ", text_of_lane(value.scalar, words[i]));
    text += ")";
    return text;
}

/// `shade.sgl:12`, and the calls it was inlined through.
[[nodiscard]] cc::string place_of(shader_debug_site const& site)
{
    auto text = site.at.line > 0 ? cc::format("{}:{}", site.at.file, site.at.line) : cc::string("<shader>");
    for (auto const& call : site.inlined_from)
        text.appendf(", inlined from {}:{}", call.file, call.line);
    return text;
}
} // namespace

void impl::debug_windows::ensure_windows()
{
    if (has_windows())
        return;
    _creation.lock(
        [&](int&)
        {
            if (has_windows())
                return;
            for (auto i = isize(0); i < k_window_count; ++i)
            {
                _windows[i].buffer = _ctx.persistent.create_raw_buffer(
                    k_window_bytes, buffer_usage::readwrite_buffer | buffer_usage::copy_src | buffer_usage::copy_dst);
                _windows[i].needs_reset.store(true);
            }
            _sink = _ctx.persistent.create_raw_buffer(
                k_header_words * 4, buffer_usage::readwrite_buffer | buffer_usage::copy_src | buffer_usage::copy_dst);
            _sink_needs_reset.store(true);
            _created.store(true);
        });
}

void impl::debug_windows::create()
{
    if (k_enabled)
        ensure_windows();
}

raw_buffer_handle impl::debug_windows::window_for_recording(bool writes)
{
    ensure_windows();
    auto const index = index_of(_ctx.current_epoch());
    auto& w = _windows[index];
    if (w.is_busy.load())
    {
        if (writes && _sink_needs_reset.exchange(false))
        {
            u32 const empty[k_header_words] = {0, 0, 0};
            _ctx.upload.bytes_to_buffer(_sink, cc::make_pinned_data(cc::span<u32 const>(empty).as_bytes()));
        }
        return _sink;
    }
    if (writes)
    {
        // a list that binds the window waits on this copy, so the reset lands before the first record
        if (w.needs_reset.exchange(false))
            reset(index);
        w.is_written.store(true);
    }
    return w.buffer;
}

void impl::debug_windows::add_sites(cc::span<shader_debug_site const> sites)
{
    if (sites.empty())
        return;
    _sites.lock(
        [&](cc::map<u32, shader_debug_site>& known)
        {
            for (auto const& site : sites)
            {
                // a 32-bit id of a different site is a collision, which says so once and keeps the first
                if (auto const* const other = known.get_ptr(site.id); other != nullptr && !(*other == site))
                    CC_LOG_WARNING("two shader debug sites share the id {}: {} and {}", site.id, place_of(*other),
                                   place_of(site));
                else
                    known[site.id] = site;
            }
        });
}

void impl::debug_windows::on_closing(epoch closing)
{
    if (!has_windows())
        return;
    auto const index = index_of(closing);
    auto& w = _windows[index];
    if (!w.is_written.exchange(false))
        return;
    w.is_busy.store(true);
    w.written_in = closing;
    w.is_requested = false;
    // asked for once the epoch has retired, so the copy reads what every list of the epoch wrote
    _ctx.defer_until_retired([this, index] { request(index); });
    w.pending = bytes_future();
}

void impl::debug_windows::request(isize index)
{
    auto& w = _windows[index];
    w.pending = _ctx.download.bytes_from_buffer(w.buffer, 0, k_window_bytes);
    w.is_requested = true;
}

void impl::debug_windows::on_opened(epoch opened)
{
    if (!has_windows())
        return;
    auto const index = index_of(opened);
    auto& w = _windows[index];
    if (!w.pending.has_value())
        return;
    if (_ctx.execution() == execution_model::never_block)
    {
        CC_LOG_WARNING("the shader debug drain fell more than an epoch behind, so epoch {}'s records are dropped",
                       u64(opened));
        return;
    }
    // only a drain more than a whole epoch late waits here
    if (!w.is_requested)
    {
        _ctx.wait_for_epoch(w.written_in);
        _ctx.process_completed_epochs();
    }
    if (w.pending.has_value() && w.is_requested)
    {
        auto const bytes = cc::try_async_blocking_get(w.pending.value().bytes());
        finish(index, bytes.has_value() ? bytes.value().span() : cc::span<byte const>());
    }
}

void impl::debug_windows::poll()
{
    if (!has_windows())
        return;
    for (auto i = isize(0); i < k_window_count; ++i)
    {
        auto& w = _windows[i];
        if (!w.pending.has_value() || !w.is_requested)
            continue;
        if (auto const bytes = w.pending.value().try_get_bytes(); bytes.has_value())
            finish(i, bytes.value().span());
    }
}

bool impl::debug_windows::has_records_due() const
{
    if (!has_windows())
        return false;
    for (auto const& w : _windows)
        if (w.is_written.load() || w.pending.has_value())
            return true;
    return false;
}

bool impl::debug_windows::is_recording_written() const
{
    return has_windows() && _windows[index_of(_ctx.current_epoch())].is_written.load();
}

void impl::debug_windows::drain()
{
    if (!has_windows())
        return;
    if (_ctx.execution() == execution_model::never_block)
    {
        poll();
        return;
    }
    for (auto i = isize(0); i < k_window_count; ++i)
        if (_windows[i].pending.has_value())
            on_opened(_windows[i].written_in);
}

void impl::debug_windows::release()
{
    if (!has_windows())
        return;
    for (auto& w : _windows)
    {
        w.pending = {};
        w.buffer = {};
        w.is_busy.store(false);
    }
    _sink = {};
    _created.store(false);
}

void impl::debug_windows::finish(isize index, cc::span<byte const> bytes)
{
    auto& w = _windows[index];
    w.pending = {};
    w.is_requested = false;
    // reset by the next bind that writes it, which a list waits on before its first record
    w.needs_reset.store(true);
    w.is_busy.store(false);

    auto const word_count = bytes.size() / 4;
    if (word_count < k_header_words)
        return;
    auto const words = cc::span<u32 const>(reinterpret_cast<u32 const*>(bytes.data()), word_count);
    auto const capacity = cc::min(isize(words[1]), word_count - k_header_words);
    auto const used = cc::min(isize(words[0]), capacity);
    auto const dropped = words[2];
    auto const records = words.subspan({.offset = k_header_words, .size = used});

    _sites.lock(
        [&](cc::map<u32, shader_debug_site>& known)
        {
            auto at = isize(0);
            while (at + 2 <= records.size())
            {
                auto const id = records[at];
                auto const count = isize(records[at + 1]);
                // a record claims its own length, and one that claims past the window ends the read
                if (count < 2 || at + count > records.size())
                    break;
                auto const values = records.subspan({.offset = at + 2, .size = count - 2});
                at += count;

                auto const* const site = known.get_ptr(id);
                if (site == nullptr)
                {
                    CC_LOG_WARNING("a shader debug record of site {}, which no pipeline of this context stated", id);
                    continue;
                }
                auto texts = cc::vector<cc::string>();
                auto lane = isize(0);
                for (auto const& value : site->values)
                {
                    if (lane + value.lanes > values.size())
                        break;
                    texts.push_back(text_of_value(value, values.subspan({.offset = lane, .size = value.lanes})));
                    lane += value.lanes;
                }
                auto message = cc::string();
                if (site->pieces.empty())
                    for (auto const& t : texts)
                        message.appendf("{}{}", message.empty() ? "" : " ", t);
                else
                    for (auto i = isize(0); i < site->pieces.size(); ++i)
                    {
                        message += site->pieces[i];
                        if (i < texts.size())
                            message += texts[i];
                    }
                if (site->is_assert)
                    CC_LOG_ERROR("{}: assert failed{}{}", place_of(*site), message.empty() ? "" : ": ", message);
                else
                    CC_LOG_INFO("{}: {}", place_of(*site), message);
            }
        });
    if (dropped > 0)
        CC_LOG_WARNING("{} shader debug records did not fit the window of {} bytes", dropped, k_window_bytes);
}

void impl::debug_windows::reset(isize index)
{
    u32 const header[k_header_words] = {0, u32(k_window_bytes / 4 - k_header_words), 0};
    _ctx.upload.bytes_to_buffer(_windows[index].buffer, cc::make_pinned_data(cc::span<u32 const>(header).as_bytes()));
}
