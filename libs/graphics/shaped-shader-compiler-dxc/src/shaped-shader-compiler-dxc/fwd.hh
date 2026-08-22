#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/record/domain_fwd.hh>

/// Forward declarations for shaped-shader-compiler-dxc (namespace `ssc::dxc`). Include a concrete
/// header for the full type; this is for signatures that only need the name.

namespace ssc::dxc
{
// Vocabulary types (i32/u32/u64/isize/byte/...) available bare inside ssc::dxc, not leaked globally.
using namespace cc::primitive_defines;

class compiler;
class shader_cache;

struct shader_description;
struct compile_options;
struct preprocessed_source;

// The compile_options vocabulary (see compile_options.hh).
enum class compile_target;
enum class shader_model;
enum class optimization_level;

/// The domain every recording site in ssc::dxc is attributed to.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace ssc::dxc
