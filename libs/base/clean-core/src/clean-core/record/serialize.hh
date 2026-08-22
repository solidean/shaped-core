#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/string/string_view.hh>

// Getting a recording out of the process it was made in.
//
// A live recording is process-local by construction: its events point at descriptors, and descriptors are static
// objects in this binary.
// Serializing is exactly the job of resolving those pointers — the names, units, domains, source locations and
// field layouts get written out, so the result is self-describing and readable by a build that has never heard of
// the code that produced it.
//
// **The format carries no stability guarantee**, and will not for a good while.
// Durability comes from an exporter — babel::chrome_trace — not from these bytes.
// Every reader checks the version and refuses a file it does not understand, rather than misreading one.

/// A recording that came from bytes rather than from the running process.
///
/// It OWNS the descriptors, domains, units, field layouts and strings its events point at, so the recording inside is
/// only valid for as long as this object is.
/// That is the one way a loaded recording differs from a live one; everything else — the algebra, the queries, replay
/// — works identically.
struct cc::rec::loaded_recording
{
    loaded_recording() = default;

    loaded_recording(loaded_recording&&) = default;
    loaded_recording& operator=(loaded_recording&&) = default;
    loaded_recording(loaded_recording const&) = delete;
    loaded_recording& operator=(loaded_recording const&) = delete;

    /// The events.
    /// Borrowed from this object, so it must outlive whatever the caller does with them.
    [[nodiscard]] rec::recording const& events() const { return _events; }

    /// The wall-clock second the dump was taken, or 0.
    [[nodiscard]] f64 dumped_at_wall_secs() const { return _dumped_at_wall_secs; }

    /// The cycle rate the recording process measured, so a duration in cycles becomes one in seconds.
    [[nodiscard]] f64 cycles_per_second() const { return _cycles_per_second; }

    /// True when the writer hit its byte cap and stopped early, which a crash dump under a cap can.
    [[nodiscard]] bool is_truncated() const { return _is_truncated; }

private:
    friend struct rec::impl::recording_loader;

    rec::recording _events;

    // The owned backing for everything the events point at.
    // Sized once and never grown afterwards, because the events hold pointers into them.
    cc::vector<char> _strings;

    // The same, for strings a PAYLOAD points at rather than a descriptor — a `type_code::cstring` value.
    // Separate because its size is only known after the event stream has been walked and validated, and growing
    // `_strings` at that point would dangle every descriptor pointer already handed out.
    cc::vector<char> _payload_strings;

    // The bytes behind every `type_code::pinned_bytes` payload, which the file carries in a section of its own.
    // A live recording's pins point at the caller's storage; a loaded one points in here, and `field_as_bytes` reads
    // the same way either way.
    cc::vector<byte> _blobs;
    cc::vector<rec::field> _fields;
    cc::vector<rec::unit> _units;
    cc::vector<rec::relation_type> _relations;
    cc::vector<rec::desc> _descs;
    cc::vector<cc::unique_ptr<rec::domain>> _domains;

    f64 _dumped_at_wall_secs = 0;
    f64 _cycles_per_second = 0;
    bool _is_truncated = false;
};

namespace cc::rec
{
/// The format version these bytes are written at.
/// A reader refuses anything else rather than guessing.
inline constexpr u32 serialized_version = 4;

/// Writes `r` to a self-contained buffer.
[[nodiscard]] cc::vector<byte> serialize(rec::recording const& r);

/// serialize, then write to a file.
[[nodiscard]] cc::result<cc::unit> save_recording(rec::recording const& r, cc::string_view path);

/// Write bytes that `serialize` already produced.
///
/// The split exists for a caller that must serialize EARLY and write late: a recording holds chunk references, so it
/// cannot outlive the recorder that owns the pool, while the file it belongs in may not be known until afterwards.
[[nodiscard]] cc::result<cc::unit> save_serialized_recording(cc::span<byte const> bytes, cc::string_view path);

/// Rebuilds a recording from bytes, owning everything its events point at.
[[nodiscard]] cc::result<rec::loaded_recording> deserialize(cc::span<byte const> bytes);

/// Reads a file and rebuilds the recording in it.
[[nodiscard]] cc::result<rec::loaded_recording> load_recording(cc::string_view path);
} // namespace cc::rec
