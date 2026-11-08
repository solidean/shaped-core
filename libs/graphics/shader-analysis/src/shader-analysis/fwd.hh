#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/record/domain_fwd.hh>

/// Forward declarations for shader-analysis, namespace `shan`.
namespace shan
{
using namespace cc::primitive_defines;

enum class op_kind : u8;
struct function_metrics;
struct module_metrics;
struct difference;

/// The domain every recording site in shan is attributed to.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace shan
