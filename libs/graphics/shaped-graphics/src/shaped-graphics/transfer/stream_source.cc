#include <shaped-graphics/transfer/stream_source.hh>

namespace sg
{
namespace
{
/// One resident blob, handed over whole on the first poll.
/// An empty payload never reaches here — the sg layer settles an empty transfer without building a source at all.
class pinned_stream_source final : public stream_source
{
public:
    pinned_stream_source(cc::pinned_data<byte const> data, isize offset)
      : _hint(i64(data.size())), _data(cc::move(data)), _offset(offset)
    {
    }

    [[nodiscard]] stream_poll try_next_chunk() override
    {
        if (_handed_over)
            return {.status = stream_source_status::done};
        _handed_over = true;
        return {.status = stream_source_status::ready, .chunk = {.data = cc::move(_data), .offset = _offset}};
    }

    [[nodiscard]] i64 total_size_hint() const override { return _hint; }

private:
    i64 _hint = 0; // captured before the move below, which empties _data
    cc::pinned_data<byte const> _data;
    isize _offset = 0;
    bool _handed_over = false;
};
} // namespace

std::unique_ptr<stream_source> make_pinned_stream_source(cc::pinned_data<byte const> data, isize offset)
{
    return std::make_unique<pinned_stream_source>(cc::move(data), offset);
}
} // namespace sg
