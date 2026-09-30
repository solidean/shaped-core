#include <shaped-rendering/impl/dlss_ngx.hh>

// The NGX seam where there is no NGX: no SDK fetched, no dx12 backend, or not a Windows x64 build.
//
// Present rather than absent so `sr::dlss_rr_routine` and every symbol around it exist in every build.
// A caller naming `sr::denoise_method::dlss_rr` compiles everywhere and is told `unsupported` here, which is the same
// shape `sr::window_system` takes without SDL3 — the API does not disappear, the answer changes.

namespace sr::impl
{
struct dlss_instance
{
};

bool dlss_is_available(sg::context const& ctx)
{
    (void)ctx;
    return false;
}

std::shared_ptr<dlss_instance> dlss_open(sg::context& ctx)
{
    (void)ctx;
    return nullptr;
}

void dlss_close(std::shared_ptr<dlss_instance> const& instance)
{
    (void)instance;
}

dlss_stream* dlss_create_stream(sg::command_list& cmd,
                                std::shared_ptr<dlss_instance> const& instance,
                                dlss_feature_desc const& desc)
{
    (void)cmd;
    (void)instance;
    (void)desc;
    return nullptr;
}

void dlss_release_stream(dlss_stream* stream)
{
    delete stream;
}

int dlss_released_stream_count()
{
    return 0;
}

bool dlss_evaluate(sg::command_list& cmd, dlss_stream& stream, dlss_eval_desc const& desc)
{
    (void)cmd;
    (void)stream;
    (void)desc;
    return false;
}
} // namespace sr::impl
