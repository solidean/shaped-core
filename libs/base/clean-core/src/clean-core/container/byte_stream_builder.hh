#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/string_view.hh>

#include <cstring> // std::memcpy
#include <type_traits>

/// Accumulates data into one contiguous byte buffer — the serialization front end for hashing
/// (feed written_bytes() to cc::hash128::create / cc::make_hash_of_bytes). Length-prefixing helpers
/// keep different splits of the same data distinct, so a stream of heterogeneous fields hashes
/// unambiguously.
///
/// Built for reuse: clear() resets the length without releasing the allocation, so the buffer stays
/// hot for the next build. thread_local_scratch() hands out a per-thread instance for exactly this.

namespace cc
{
class byte_stream_builder
{
public:
    /// An empty per-thread builder, cleared on each call — amortizes allocations when building a
    /// local blob to hash. Do NOT hold two references across a nested build on the same thread.
    [[nodiscard]] static byte_stream_builder& thread_local_scratch()
    {
        static thread_local byte_stream_builder scratch;
        scratch.clear();
        return scratch;
    }

    /// Append raw bytes.
    void add(cc::span<cc::byte const> bytes)
    {
        auto const offset = _buffer.size();
        _buffer.resize_to_uninitialized(offset + bytes.size());
        if (!bytes.empty())
            std::memcpy(_buffer.data() + offset, bytes.data(), size_t(bytes.size()));
    }

    /// Append the byte representation of a single trivially-copyable value.
    template <class T>
    void add_pod(T const& value)
    {
        static_assert(std::is_trivially_copyable_v<T>, "add_pod needs a trivially-copyable T");
        this->add(cc::span<T const>(&value, 1).as_bytes());
    }

    /// Append the byte representation of every element in a contiguous range (elements trivially copyable).
    template <class Range>
    void add_pod_span(Range const& range)
    {
        this->add(cc::as_bytes(range));
    }

    /// Append a u64 element-count prefix, then the elements' bytes. Prevents ambiguity between
    /// different splits of the same total data.
    template <class Range>
    void add_pod_span_sized(Range const& range)
    {
        this->add_pod(cc::u64(cc::span(range).size()));
        this->add_pod_span(range);
    }

    /// Append a u64 length prefix, then the raw character bytes (disambiguates concatenated strings).
    void add_string(cc::string_view sv)
    {
        this->add_pod(cc::u64(sv.size()));
        this->add(sv.as_bytes());
    }

    /// Append a presence byte, then the value's bytes if present.
    template <class T>
    void add_optional(cc::optional<T> const& opt)
    {
        this->add_bool(opt.has_value());
        if (opt.has_value())
            this->add_pod(opt.value());
    }

    /// Append a bool as a single byte (0/1). bool lacks a unique object representation, so we never
    /// add_pod it directly.
    void add_bool(bool b) { this->add_pod(cc::u8(b ? 1 : 0)); }

    /// Reset the length to zero, keeping the allocation for reuse.
    void clear() { _buffer.clear(); }

    /// View the accumulated bytes. Invalidated by the next mutating call.
    [[nodiscard]] cc::span<cc::byte const> written_bytes() const { return _buffer; }

private:
    cc::vector<cc::byte> _buffer;
};
} // namespace cc
