#include <shaped-rendering/impl/dlss_ngx.hh>

// The NGX seam where there is no NGX: no SDK fetched, no dx12 backend, or not a Windows x64 build.
//
// Present rather than absent so `sr::dlss_rr_routine` and every symbol around it exist in every build.
// A caller naming `sr::denoise_method::dlss_rr` compiles everywhere and is told `unsupported` here, which is the same
// shape `sr::window_system` takes without SDL3 — the API does not disappear, the answer changes.

namespace sr::impl
{
bool dlss_is_available(sg::context const& ctx)
{
    (void)ctx;
    return false;
}

void* dlss_create_feature(sg::command_list& cmd, dlss_feature_desc const& desc)
{
    (void)cmd;
    (void)desc;
    return nullptr;
}

void dlss_release_feature(void* feature)
{
    (void)feature;
}

bool dlss_evaluate(sg::command_list& cmd, void* feature, dlss_eval_desc const& desc)
{
    (void)cmd;
    (void)feature;
    (void)desc;
    return false;
}
} // namespace sr::impl
