#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/record/domain_fwd.hh>

/// Forward declarations for shaped-shader-compiler-msl (namespace `ssc::msl`).
/// Include a concrete header for the full type; this is for signatures that only need the name.

namespace ssc::msl
{
// Vocabulary types (i32/u32/u64/isize/byte/...) available bare inside ssc::msl, not leaked globally.
using namespace cc::primitive_defines;

class compiler;
class shader_cache;

struct shader_description;
struct compile_options;
struct reflected_bindings;
struct toolchain_info;

// The compile_options vocabulary (see compile_options.hh).
enum class artifact_kind;
enum class optimization_level;

/// The domain every recording site in ssc::msl is attributed to.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace ssc::msl
