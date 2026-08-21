#include <clean-core/common/assert.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/impl/texture_copy_region.hh>
#include <shaped-graphics/resource/raw_buffer.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/transfer/stream.hh>

namespace sg
{
namespace
{
/// A handle over an already-settled transfer, for calls with nothing to do (an empty pin, an empty region).
/// It still carries a real control block, so progress and completion answer the same way they would for real work.
[[nodiscard]] std::shared_ptr<impl::stream_control> make_settled_control()
{
    auto control = std::make_shared<impl::stream_control>();
    control->completion = make_ready_completion();
    control->total_hint.store(0, std::memory_order_relaxed);
    return control;
}

/// The static half of the streaming contract: the claimed extent must be one the resource was created to allow.
/// Checked at the call rather than at the copy, so a missing usage flag names the line that asked for it.
void assert_buffer_scope(raw_buffer_handle const& buffer, stream_scope scope)
{
    CC_ASSERT(scope != stream_scope::subresource, "stream_scope::subresource is not meaningful for a buffer, which "
                                                  "has no subresources — use resource or region");
    if (scope == stream_scope::region)
        CC_ASSERT(buffer->usage().has(buffer_usage::allow_region_stream),
                  "streaming into a region of a buffer other work may use concurrently needs "
                  "buffer_usage::allow_region_stream");
}

void assert_texture_scope(raw_texture_handle const& texture, stream_scope scope)
{
    auto const usage = texture->usage();
    if (scope == stream_scope::subresource)
        CC_ASSERT(usage.has_any(texture_usage::allow_subresource_stream | texture_usage::allow_region_stream),
                  "streaming a whole subresource of a texture other work may use concurrently needs "
                  "texture_usage::allow_subresource_stream");
    if (scope == stream_scope::region)
        CC_ASSERT(usage.has(texture_usage::allow_region_stream),
                  "streaming into a region INSIDE a subresource other work may use concurrently needs "
                  "texture_usage::allow_region_stream");
}
} // namespace

stream_upload_handle context_stream_scope::bytes_to_buffer(raw_buffer_handle buffer,
                                                           cc::pinned_data<byte const> data,
                                                           isize offset_in_bytes,
                                                           stream_scope scope)
{
    CC_ASSERT(buffer != nullptr, "stream upload target buffer is null");
    CC_ASSERT(offset_in_bytes >= 0 && offset_in_bytes + data.size() <= buffer->size_in_bytes(),
              "stream upload range is out of the buffer's bounds");
    assert_buffer_scope(buffer, scope);

    if (data.empty())
        return stream_upload_handle(make_settled_control());
    return _ctx.stream_bytes_to_buffer(cc::move(buffer), cc::move(data), offset_in_bytes, scope);
}

stream_upload_handle context_stream_scope::bytes_to_texture(raw_texture_handle texture,
                                                            cc::pinned_data<byte const> data,
                                                            subresource_index const& subresource,
                                                            cc::optional<texture_region> region,
                                                            stream_scope scope)
{
    CC_ASSERT(texture != nullptr, "stream upload target texture is null");
    impl::assert_valid_subresource(texture, subresource);
    texture_region const box = region.has_value() ? region.value() : impl::full_subresource_region(texture, subresource);
    impl::assert_texture_region_in_bounds(texture, subresource, box);
    assert_texture_scope(texture, scope);

    if (box.is_empty() || data.empty())
        return stream_upload_handle(make_settled_control());
    return _ctx.stream_bytes_to_texture(cc::move(texture), cc::move(data), subresource, box, scope);
}

stream_upload_handle context_stream_scope::from_source_to_buffer(raw_buffer_handle buffer,
                                                                 std::unique_ptr<stream_source> source,
                                                                 isize offset_in_bytes,
                                                                 stream_scope scope)
{
    CC_ASSERT(buffer != nullptr, "stream upload target buffer is null");
    CC_ASSERT(source != nullptr, "stream upload source is null");
    CC_ASSERT(offset_in_bytes >= 0 && offset_in_bytes <= buffer->size_in_bytes(), "stream upload offset is out of the "
                                                                                  "buffer's bounds");
    assert_buffer_scope(buffer, scope);
    return _ctx.stream_source_to_buffer(cc::move(buffer), cc::move(source), offset_in_bytes, scope);
}

stream_upload_handle context_stream_scope::from_source_to_texture(raw_texture_handle texture,
                                                                  std::unique_ptr<stream_source> source,
                                                                  subresource_index const& subresource,
                                                                  cc::optional<texture_region> region,
                                                                  stream_scope scope)
{
    CC_ASSERT(texture != nullptr, "stream upload target texture is null");
    CC_ASSERT(source != nullptr, "stream upload source is null");
    impl::assert_valid_subresource(texture, subresource);
    texture_region const box = region.has_value() ? region.value() : impl::full_subresource_region(texture, subresource);
    impl::assert_texture_region_in_bounds(texture, subresource, box);
    assert_texture_scope(texture, scope);

    if (box.is_empty())
        return stream_upload_handle(make_settled_control());
    return _ctx.stream_source_to_texture(cc::move(texture), cc::move(source), subresource, box, scope);
}

stream_download_handle context_stream_scope::bytes_from_buffer(raw_buffer_handle buffer,
                                                               isize offset_in_bytes,
                                                               isize size_in_bytes,
                                                               stream_scope scope)
{
    CC_ASSERT(buffer != nullptr, "stream download source buffer is null");
    CC_ASSERT(size_in_bytes >= 0, "stream download size must be non-negative");
    CC_ASSERT(offset_in_bytes >= 0 && offset_in_bytes + size_in_bytes <= buffer->size_in_bytes(),
              "stream download range is out of the buffer's bounds");
    assert_buffer_scope(buffer, scope);

    if (size_in_bytes == 0)
        return stream_download_handle(make_settled_control(),
                                      bytes_future(cc::pinned_data<byte const>(), make_ready_completion()));
    return _ctx.stream_bytes_from_buffer(cc::move(buffer), offset_in_bytes, size_in_bytes, scope);
}

stream_download_handle context_stream_scope::bytes_from_texture(raw_texture_handle texture,
                                                                subresource_index const& subresource,
                                                                cc::optional<texture_region> region,
                                                                stream_scope scope)
{
    CC_ASSERT(texture != nullptr, "stream download source texture is null");
    impl::assert_valid_subresource(texture, subresource);
    texture_region const box = region.has_value() ? region.value() : impl::full_subresource_region(texture, subresource);
    impl::assert_texture_region_in_bounds(texture, subresource, box);
    assert_texture_scope(texture, scope);

    if (box.is_empty())
        return stream_download_handle(make_settled_control(),
                                      bytes_future(cc::pinned_data<byte const>(), make_ready_completion()));
    return _ctx.stream_bytes_from_texture(cc::move(texture), subresource, box, scope);
}

void context_stream_scope::set_upload_ratio(float ratio)
{
    _ctx.set_stream_upload_ratio(ratio);
}
void context_stream_scope::set_download_ratio(float ratio)
{
    _ctx.set_stream_download_ratio(ratio);
}
void context_stream_scope::set_upload_aging(float per_second)
{
    _ctx.set_stream_upload_aging(per_second);
}
void context_stream_scope::set_download_aging(float per_second)
{
    _ctx.set_stream_download_aging(per_second);
}
} // namespace sg
