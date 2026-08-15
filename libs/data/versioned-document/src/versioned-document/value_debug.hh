#pragma once

#include <clean-core/string/string.hh>
#include <versioned-document/value.hh>

/// A one-way text projection of a value, for dumps and test-failure output.
///
/// This is NOT a serialization format, and NOTHING may parse it back.
/// JSON is the display metaphor for the model, never its storage — the canonical encoding is the only durable form,
/// and it is what equality, hashing and content addressing commit to.
///
/// The projection is deliberately lossy in the direction that matters for reading a dump, and deliberately exact where the
/// format promises exactness: a NaN payload and the sign of a zero are shown, because the codec preserves them.

namespace vdoc
{
/// Renders `v` as JSON-ish text.
///
/// - `bytes` becomes `bytes(<hex>)`, which no JSON reader would accept, and that is the point.
/// - a string escapes `"`, `\`, and every byte outside printable ASCII as `\xNN`, so the output stays printable
///   even though the codec never validates that a string is UTF-8.
/// - `-0.0` prints as `-0.0`, and a NaN whose payload is not the canonical quiet one prints as `NaN(0x...)`.
///
/// Containers are walked by index, so this is quadratic in a container's element count.
/// That is fine for what it is for — values are small by design, and a dump is not on any hot path.
[[nodiscard]] cc::string to_debug_string(value_view v);
} // namespace vdoc
