#pragma once

#include <clean-core/string/string.hh>
#include <shaped-graphics-language/fwd.hh>

namespace sgl
{
/// slib's `impl/pipeline_fields.hh`, as the prelude's mirror of `sg::raster_pipeline_description` generates it.
///
/// One typed setter per field of the mirror, which a generated pipeline calls directly, and the tables a hot reload
/// applies a re-described setting through: each field by its path, and each enum's cases by name.
/// Every setter names the sg field and every case the sg enumerator, so a mirror that drifted from sg fails to compile.
/// An sg field or enumerator the mirror lacks is simply not settable from SGL.
///
/// The text names sg and slib, which this library links neither of: it is only ever written out, by `sgl pipeline-fields`.
[[nodiscard]] cc::string pipeline_fields_text();
} // namespace sgl
