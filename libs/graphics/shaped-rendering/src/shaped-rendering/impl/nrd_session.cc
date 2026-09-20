#include <NRD.h>
#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/nrd_session.hh>

// NRD's dispatch list, executed through sg.
//
// The whole of the library is confined to this TU and `nrd_instance.cc`; `nrd_session.hh` names no NRD type.
//
// What makes this integration small is that NRD renders nothing.
// It reports the pipelines it needs (as embedded DXIL), the scratch textures it needs, and then, per frame, a list of
// dispatches over those plus the caller's own resources.
// Everything below is the mechanical half of that: build the pipelines, allocate the pools, bind and dispatch.

namespace sr::impl
{
namespace
{
/// NRD's texture formats, mapped onto sg's.
///
/// Only the ones NRD's pools actually ask for are listed; anything else is a version that added a format and is an
/// error rather than a guess, since a wrong format here is a silently wrong denoise.
[[nodiscard]] cc::optional<sg::pixel_format> pixel_format_of(nrd::Format format)
{
    switch (format)
    {
    case nrd::Format::R8_UNORM:
        return sg::pixel_format::r8_unorm;
    case nrd::Format::R8_SNORM:
        return sg::pixel_format::r8_snorm;
    case nrd::Format::R8_UINT:
        return sg::pixel_format::r8_uint;
    case nrd::Format::R8_SINT:
        return sg::pixel_format::r8_sint;

    case nrd::Format::RG8_UNORM:
        return sg::pixel_format::rg8_unorm;
    case nrd::Format::RG8_SNORM:
        return sg::pixel_format::rg8_snorm;
    case nrd::Format::RG8_UINT:
        return sg::pixel_format::rg8_uint;
    case nrd::Format::RG8_SINT:
        return sg::pixel_format::rg8_sint;

    case nrd::Format::RGBA8_UNORM:
        return sg::pixel_format::rgba8_unorm;
    case nrd::Format::RGBA8_SNORM:
        return sg::pixel_format::rgba8_snorm;
    case nrd::Format::RGBA8_UINT:
        return sg::pixel_format::rgba8_uint;
    case nrd::Format::RGBA8_SINT:
        return sg::pixel_format::rgba8_sint;
    case nrd::Format::RGBA8_SRGB:
        return sg::pixel_format::rgba8_unorm_srgb;

    case nrd::Format::R16_SFLOAT:
        return sg::pixel_format::r16_float;
    case nrd::Format::R16_UINT:
        return sg::pixel_format::r16_uint;
    case nrd::Format::R16_SINT:
        return sg::pixel_format::r16_sint;

    case nrd::Format::RG16_SFLOAT:
        return sg::pixel_format::rg16_float;
    case nrd::Format::RG16_UINT:
        return sg::pixel_format::rg16_uint;
    case nrd::Format::RG16_SINT:
        return sg::pixel_format::rg16_sint;

    case nrd::Format::RGBA16_SFLOAT:
        return sg::pixel_format::rgba16_float;
    case nrd::Format::RGBA16_UINT:
        return sg::pixel_format::rgba16_uint;
    case nrd::Format::RGBA16_SINT:
        return sg::pixel_format::rgba16_sint;

    case nrd::Format::R32_SFLOAT:
        return sg::pixel_format::r32_float;
    case nrd::Format::R32_UINT:
        return sg::pixel_format::r32_uint;
    case nrd::Format::R32_SINT:
        return sg::pixel_format::r32_sint;

    case nrd::Format::RG32_SFLOAT:
        return sg::pixel_format::rg32_float;
    case nrd::Format::RG32_UINT:
        return sg::pixel_format::rg32_uint;
    case nrd::Format::RG32_SINT:
        return sg::pixel_format::rg32_sint;

    case nrd::Format::RGBA32_SFLOAT:
        return sg::pixel_format::rgba32_float;
    case nrd::Format::RGBA32_UINT:
        return sg::pixel_format::rgba32_uint;
    case nrd::Format::RGBA32_SINT:
        return sg::pixel_format::rgba32_sint;

    case nrd::Format::R10_G10_B10_A2_UNORM:
        return sg::pixel_format::rgb10a2_unorm;
    case nrd::Format::R11_G11_B10_UFLOAT:
        return sg::pixel_format::rg11b10_float;

    default:
        // The normalized 16-bit formats and the shared-exponent one have no sg spelling, and the 3-channel 32-bit ones
        // are not a thing dx12 storage textures do.
        // Refusing names the format rather than guessing a near neighbour, because a pool texture in the wrong format
        // is a wrong denoise rather than an error.
        return {};
    }
}

/// The name a hand-written binding is matched by.
///
/// sg pairs a bound view with its layout entry by NAME rather than by register, and NRD's reflection is not available
/// to us — so the names are invented here and used on both sides.
/// Register class and index make them unique, which is all that is required of them.
[[nodiscard]] cc::string binding_name(bool storage, u32 index)
{
    return cc::format("{}{}", storage ? "u" : "t", index);
}

constexpr char const* k_constants_name = "nrd_constants";

/// The block NRD's constants travel in.
///
/// A fixed, generously sized POD rather than a buffer of the dispatch's own length: a uniform view is typed and its
/// size must be a multiple of 16, while NRD lays its constants out itself and reports a different length per dispatch.
/// Binding a block LARGER than the shader declared is legal everywhere and reads the same, so one type serves every
/// dispatch — `create` asserts NRD's own maximum fits.
struct nrd_constants_block
{
    tg::vec4f words[64];
};

/// The samplers NRD declares, as sg static samplers.
///
/// Static rather than bound per group, which is what NRD recommends and what keeps a dispatch's group to its textures.
[[nodiscard]] cc::vector<sg::named_sampler> samplers_of(nrd::InstanceDesc const& desc)
{
    auto out = cc::vector<sg::named_sampler>();
    for (auto i = u32(0); i < desc.samplersNum; ++i)
    {
        auto const filter
            = desc.samplers[i] == nrd::Sampler::LINEAR_CLAMP ? sg::sampler_filter::linear : sg::sampler_filter::nearest;
        out.push_back({.name = cc::format("s{}", desc.samplersBaseRegisterIndex + i),
                       .sampler = {.min_filter = filter,
                                   .mag_filter = filter,
                                   .mip_filter = filter,
                                   .address_u = sg::sampler_address_mode::clamp_edge,
                                   .address_v = sg::sampler_address_mode::clamp_edge,
                                   .address_w = sg::sampler_address_mode::clamp_edge}});
    }
    return out;
}

/// The bindings one NRD pipeline declares, in the register layout `InstanceDesc` states.
///
/// NRD numbers its two ranges from the same base but in different register CLASSES — sampled textures are `t` and
/// storage textures are `u` — so each range counts from the base within its own class.
[[nodiscard]] cc::vector<sg::binding> bindings_of(nrd::InstanceDesc const& instance, nrd::PipelineDesc const& pipeline)
{
    auto out = cc::vector<sg::binding>();

    if (pipeline.hasConstantData)
        out.push_back({.name = k_constants_name,
                       .space = instance.constantBufferAndSamplersSpaceIndex,
                       .index = instance.constantBufferRegisterIndex,
                       .type = sg::binding_type::uniform_buffer});

    for (auto i = u32(0); i < instance.samplersNum; ++i)
        out.push_back({.name = cc::format("s{}", instance.samplersBaseRegisterIndex + i),
                       .space = instance.constantBufferAndSamplersSpaceIndex,
                       .index = instance.samplersBaseRegisterIndex + i,
                       .type = sg::binding_type::sampler,
                       .sampler_type = sg::sampler_binding_type::filtering});

    u32 next[2] = {instance.resourcesBaseRegisterIndex, instance.resourcesBaseRegisterIndex};
    for (auto r = u32(0); r < pipeline.resourceRangesNum; ++r)
    {
        auto const& range = pipeline.resourceRanges[r];
        auto const storage = range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
        for (auto d = u32(0); d < range.descriptorsNum; ++d)
        {
            auto const index = next[storage ? 1 : 0]++;
            out.push_back({.name = binding_name(storage, index),
                           .space = instance.resourcesSpaceIndex,
                           .index = index,
                           .type = storage ? sg::binding_type::readwrite_texture : sg::binding_type::readonly_texture,
                           .texture_dimension = sg::texture_view_dimension::tex_2d});
        }
    }
    return out;
}
} // namespace

nrd_session::nrd_session(nrd_session&& other) noexcept
  : _instance(other._instance),
    _ctx(other._ctx),
    _extent(other._extent),
    _pipelines(cc::move(other._pipelines)),
    _layouts(cc::move(other._layouts)),
    _permanent(cc::move(other._permanent)),
    _transient(cc::move(other._transient))
{
    other._instance = nullptr;
    other._ctx = nullptr;
}

nrd_session& nrd_session::operator=(nrd_session&& other) noexcept
{
    if (this == &other)
        return *this;

    _destroy();
    _instance = other._instance;
    _ctx = other._ctx;
    _extent = other._extent;
    _pipelines = cc::move(other._pipelines);
    _layouts = cc::move(other._layouts);
    _permanent = cc::move(other._permanent);
    _transient = cc::move(other._transient);
    other._instance = nullptr;
    other._ctx = nullptr;
    return *this;
}

nrd_session::~nrd_session()
{
    _destroy();
}

void nrd_session::_destroy()
{
    if (_instance != nullptr)
        nrd::DestroyInstance(*static_cast<nrd::Instance*>(_instance));
    _instance = nullptr;

    // The textures and pipelines are sg handles: they release themselves, and the context outlives them.
    _pipelines.clear();
    _layouts.clear();
    _permanent.clear();
    _transient.clear();
}

bool nrd_session::create(sg::context& ctx, nrd_denoiser denoiser, tg::vec2i extent)
{
    _destroy();
    _ctx = &ctx;
    _extent = extent;

    // One denoiser per session, under a fixed identifier: the identifier only has to be unique within an instance,
    // and an instance is one stream's history.
    auto const kind = denoiser == nrd_denoiser::reblur_diffuse_specular ? nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR
                                                                        : nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
    nrd::DenoiserDesc const denoisers[] = {{.identifier = 0, .denoiser = kind}};

    auto const creation = nrd::InstanceCreationDesc{.denoisers = denoisers, .denoisersNum = 1};
    nrd::Instance* instance = nullptr;
    if (nrd::CreateInstance(creation, instance) != nrd::Result::SUCCESS)
    {
        CC_LOG_WARNING("nrd: could not create the denoiser instance");
        return false;
    }
    _instance = instance;

    auto const& desc = *nrd::GetInstanceDesc(*instance);
    if (desc.constantBufferMaxDataSize > sizeof(nrd_constants_block))
    {
        CC_LOG_WARNING("nrd: its constants need {} bytes and the block here holds {}", desc.constantBufferMaxDataSize,
                       sizeof(nrd_constants_block));
        _destroy();
        return false;
    }

    auto const statics = samplers_of(desc);

    // One pipeline per NRD pipeline, in its own order: `DispatchDesc::pipelineIndex` indexes exactly this.
    for (auto i = u32(0); i < desc.pipelinesNum; ++i)
    {
        auto const& p = desc.pipelines[i];
        if (p.computeShaderDXIL.bytecode == nullptr || p.computeShaderDXIL.size == 0)
        {
            CC_LOG_WARNING("nrd: pipeline {} embeds no DXIL — the SDK was built without it", i);
            _destroy();
            return false;
        }

        auto const bytes = cc::span<byte const>(static_cast<byte const*>(p.computeShaderDXIL.bytecode),
                                                isize(p.computeShaderDXIL.size));

        // A hand-built `compiled_shader`: NRD gives bytecode and a register layout rather than reflection, so the
        // bindings are written out here and the names invented to match what `execute` binds.
        auto shader = sg::compiled_shader{.stage = sg::shader_stage::compute,
                                          .format = sg::shader_format::dxil,
                                          .entry_point = desc.shaderEntryPoint,
                                          .bytecode = cc::make_pinned_data(bytes),
                                          .bindings = bindings_of(desc, p),

                                          // Deliberately 1x1x1, and not NRD's real `numthreads`.
                                          //
                                          // NRD reports each dispatch's grid in GROUPS, already divided by whatever
                                          // its own shader declared, and it does not report the divisor.
                                          // At 1x1x1 `dispatch_threads` and `dispatch_groups` agree, so NRD's numbers
                                          // are right through either call; a plausible-looking 16x16 here would make
                                          // one of the two silently wrong by a factor of 256.
                                          .workgroup_size = sg::compute_dimensions{1, 1, 1}};

        auto layout = ctx.cached.acquire_binding_group_layout(shader.bindings, statics);
        auto const pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {layout}});
        auto pipeline = ctx.cached.acquire_compute_pipeline({.shader = shader, .layout = pipeline_layout});

        _layouts.push_back(cc::move(layout));
        _pipelines.push_back(cc::move(pipeline));
    }

    // NRD's scratch, at the extent it was created for; a pool entry may be a fraction of it.
    auto const make_pool = [&](nrd::TextureDesc const* pool, u32 count, cc::vector<sg::texture_2d>& out) -> bool
    {
        for (auto i = u32(0); i < count; ++i)
        {
            auto const format = pixel_format_of(pool[i].format);
            if (!format.has_value())
            {
                CC_LOG_WARNING("nrd: pool texture {} wants format {} which this integration does not map", i,
                               u32(pool[i].format));
                return false;
            }

            auto const divisor = cc::max(u16(1), pool[i].downsampleFactor);
            out.push_back(ctx.persistent.create_texture_2d(
                {.format = format.value(),
                 .width = cc::max(1, extent[0] / divisor),
                 .height = cc::max(1, extent[1] / divisor),
                 .usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture}));
        }
        return true;
    };

    if (!make_pool(desc.permanentPool, desc.permanentPoolSize, _permanent)
        || !make_pool(desc.transientPool, desc.transientPoolSize, _transient))
    {
        _destroy();
        return false;
    }
    return true;
}

bool nrd_session::is_ready() const
{
    if (_instance == nullptr || _pipelines.empty())
        return false;
    for (auto const& p : _pipelines)
        if (p == nullptr || p->try_value() == nullptr)
            return false;
    return true;
}

namespace
{
/// `tg`'s column-major matrix written into NRD's row-major-with-row-vectors layout.
///
/// The two conventions are transposes of one another, and a matrix handed over untransposed denoises against a camera
/// that never existed — which shows up as ghosting rather than as an error, so it is worth being explicit.
void write_matrix(float (&out)[16], tg::mat4f const& m)
{
    for (auto r = 0; r < 4; ++r)
        for (auto c = 0; c < 4; ++c)
            out[r * 4 + c] = m[c, r];
}

/// The user resource one `ResourceDesc` names, or an empty texture for a pool entry.
[[nodiscard]] sg::texture_2d const* user_resource(nrd::ResourceType type, nrd_resources const& r)
{
    switch (type)
    {
    case nrd::ResourceType::IN_MV:
        return &r.motion;
    case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
        return &r.normal_roughness;
    case nrd::ResourceType::IN_VIEWZ:
        return &r.view_z;
    case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
        return &r.diffuse_radiance_hit_distance;
    case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
        return &r.specular_radiance_hit_distance;
    case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
        return &r.out_diffuse_radiance_hit_distance;
    case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
        return &r.out_specular_radiance_hit_distance;
    default:
        return nullptr;
    }
}
} // namespace

bool nrd_session::execute(sg::command_list& cmd, nrd_frame const& frame, nrd_resources const& resources)
{
    if (!is_ready())
        return false;

    auto& instance = *static_cast<nrd::Instance*>(_instance);
    auto& ctx = cmd.context();

    auto settings = nrd::CommonSettings{};
    write_matrix(settings.worldToViewMatrix, frame.world_to_view);
    write_matrix(settings.viewToClipMatrix, frame.view_to_clip);
    write_matrix(settings.worldToViewMatrixPrev, frame.previous_world_to_view);
    write_matrix(settings.viewToClipMatrixPrev, frame.previous_view_to_clip);

    settings.cameraJitter[0] = frame.jitter[0];
    settings.cameraJitter[1] = frame.jitter[1];
    settings.cameraJitterPrev[0] = frame.previous_jitter[0];
    settings.cameraJitterPrev[1] = frame.previous_jitter[1];

    settings.resourceSize[0] = u16(_extent[0]);
    settings.resourceSize[1] = u16(_extent[1]);
    settings.resourceSizePrev[0] = u16(_extent[0]);
    settings.resourceSizePrev[1] = u16(_extent[1]);
    settings.rectSize[0] = u16(_extent[0]);
    settings.rectSize[1] = u16(_extent[1]);
    settings.rectSizePrev[0] = u16(_extent[0]);
    settings.rectSizePrev[1] = u16(_extent[1]);

    settings.frameIndex = frame.frame_index;

    // A reset is what a camera cut and a first frame both are: the history describes something else, so NRD is told to
    // start over rather than to reproject into it.
    settings.accumulationMode = frame.reset ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;

    // Our motion guide is screen-space pixels; NRD wants the scale that takes it to its own units, and a 2D motion
    // vector in pixels is what `motionVectorScale` of 1 means once `isMotionVectorInWorldSpace` is false.
    settings.isMotionVectorInWorldSpace = false;

    if (nrd::SetCommonSettings(instance, settings) != nrd::Result::SUCCESS)
    {
        CC_LOG_WARNING("nrd: the common settings were refused");
        return false;
    }

    nrd::Identifier const identifiers[] = {0};
    nrd::DispatchDesc const* dispatches = nullptr;
    uint32_t dispatch_count = 0;
    if (nrd::GetComputeDispatches(instance, identifiers, 1, dispatches, dispatch_count) != nrd::Result::SUCCESS)
    {
        CC_LOG_WARNING("nrd: no dispatches were produced for this frame");
        return false;
    }

    auto const& desc = *nrd::GetInstanceDesc(instance);

    for (auto d = uint32_t(0); d < dispatch_count; ++d)
    {
        auto const& dispatch = dispatches[d];
        CC_ASSERT(dispatch.pipelineIndex < _pipelines.size(), "nrd named a pipeline it never declared");

        auto const& pipeline_desc = desc.pipelines[dispatch.pipelineIndex];

        // The views this dispatch binds, named the way `bindings_of` named them — the two walk the resource list in
        // the same order, which is what keeps an invented name matching.
        auto views = cc::vector<sg::named_view>();
        u32 next[2] = {desc.resourcesBaseRegisterIndex, desc.resourcesBaseRegisterIndex};

        for (auto i = uint32_t(0); i < dispatch.resourcesNum; ++i)
        {
            auto const& r = dispatch.resources[i];
            auto const storage = r.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
            auto const index = next[storage ? 1 : 0]++;

            sg::texture_2d const* texture = nullptr;
            if (r.type == nrd::ResourceType::TRANSIENT_POOL)
                texture = &_transient[r.indexInPool];
            else if (r.type == nrd::ResourceType::PERMANENT_POOL)
                texture = &_permanent[r.indexInPool];
            else
                texture = user_resource(r.type, resources);

            if (texture == nullptr || texture->raw() == nullptr)
            {
                // A resource NRD asked for that this integration does not supply would be read as garbage; refusing
                // is the only honest answer, and it names what is missing.
                CC_LOG_WARNING("nrd: dispatch '{}' wants resource type {} which is not bound",
                               dispatch.name != nullptr ? dispatch.name : "?", u32(r.type));
                return false;
            }

            // Built in a branch rather than with a conditional: a readonly and a readwrite view are different types,
            // and `named_view` takes whichever one this binding declared.
            if (storage)
                views.push_back({.name = binding_name(storage, index), .view = texture->as_readwrite_view()});
            else
                views.push_back({.name = binding_name(storage, index), .view = texture->as_readonly_view()});
        }

        // NRD's constants are raw bytes it laid out itself, so they travel as an opaque uniform block.
        if (pipeline_desc.hasConstantData && dispatch.constantBufferDataSize > 0)
        {
            auto const constants = ctx.transient.create_buffer<nrd_constants_block>(
                1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
            cmd.upload.bytes_to_buffer(
                constants.raw(),
                cc::span<byte const>(static_cast<byte const*>(static_cast<void const*>(dispatch.constantBufferData)),
                                     isize(dispatch.constantBufferDataSize)));
            views.push_back({.name = k_constants_name, .view = constants.as_uniform_buffer()});
        }

        auto const group = ctx.transient.create_binding_group(_layouts[dispatch.pipelineIndex], views);

        cmd.compute.bind_pipeline(**_pipelines[dispatch.pipelineIndex]->try_value());
        cmd.compute.bind_group(0, *group);

        // NRD reports GROUPS rather than threads — it has already divided by its own workgroup size.
        cmd.compute.dispatch_groups(int(dispatch.gridWidth), int(dispatch.gridHeight), 1);
    }
    return true;
}
} // namespace sr::impl
