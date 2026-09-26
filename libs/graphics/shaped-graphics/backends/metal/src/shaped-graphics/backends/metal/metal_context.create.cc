#include "metal_context.hh"

#include <Foundation/NSProcessInfo.hpp>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>

// Bring-up for the metal backend: picking a device, refusing below the floor, and assembling the context.
// Kept apart from metal_context.cc the way vulkan keeps its own create path apart — this is the one file that runs
// before a context exists, and its failure modes are environmental rather than programmer error.

namespace sg::backend::metal
{
namespace
{
/// Whether the OS is new enough for the Metal 4 API at all.
///
/// Checked before any MTL4 selector is touched, and that order is the point: this backend is compiled against the
/// macOS 26 SDK, so on an older OS the selectors simply do not exist and calling one is a crash rather than a
/// diagnosable failure.
[[nodiscard]] bool os_supports_metal4()
{
    auto const required
        = NS::OperatingSystemVersion{.majorVersion = k_required_macos_major, .minorVersion = 0, .patchVersion = 0};
    return NS::ProcessInfo::processInfo()->isOperatingSystemAtLeastVersion(required);
}

[[nodiscard]] sg::adapter_info describe(MTL::Device* device)
{
    auto info = sg::adapter_info{};
    info.name = to_string(device->name());

    // Metal reports no PCI ids and no driver version of its own — the OS version is the closest true thing, and the
    // field's contract is that it is opaque and compared for equality rather than parsed.
    info.driver_version = to_string(NS::ProcessInfo::processInfo()->operatingSystemVersionString());

    // On unified memory there is no memory "on the card" at all, and 0 is the honest answer rather than a guess at the
    // system RAM the GPU shares.
    info.dedicated_video_memory_bytes = device->hasUnifiedMemory() ? i64(0) : i64(device->recommendedMaxWorkingSetSize());

    // Metal has no software rasterizer; every device it hands out is real hardware.
    info.is_software = false;

    return info;
}
} // namespace
} // namespace sg::backend::metal

cc::result<sg::context_handle> sg::create_metal_context(backend::metal::metal_config const& config)
{
    using namespace sg::backend::metal;

    auto const scope = autorelease_scope();

    if (!os_supports_metal4())
    {
        return cc::error(cc::format(
            "the metal backend needs macOS {0} / iOS {0} or newer for the Metal 4 API; this system reports {1}",
            k_required_macos_major,
            backend::metal::to_string(NS::ProcessInfo::processInfo()->operatingSystemVersionString())));
    }

    auto* device = MTL::CreateSystemDefaultDevice();
    if (device == nullptr)
        return cc::error("no metal device on this system");

    if (!device->supportsFamily(MTL::GPUFamilyMetal4))
    {
        auto const name = backend::metal::to_string(device->name());
        device->release();
        return cc::error(cc::format("'{}' is not in the Metal 4 GPU family, which this backend requires — it needs "
                                    "Apple silicon, M1 or A14 and newer",
                                    name));
    }

    NS::Error* queue_error = nullptr;
    auto* const queue_descriptor = MTL4::CommandQueueDescriptor::alloc()->init();
    queue_descriptor->setLabel(ns_string("sg direct queue"));
    auto* const queue = device->newMTL4CommandQueue(queue_descriptor, &queue_error);
    queue_descriptor->release();

    if (queue == nullptr)
    {
        auto error = metal_error(queue_error, "could not create the metal command queue");
        device->release();
        return error;
    }

    // One compiler per context.
    // MTL4 makes compilation an explicit object where Metal 3 hid it behind the device, and it is what every pipeline
    // build goes through.
    auto* const compiler_descriptor = MTL4::CompilerDescriptor::alloc()->init();
    NS::Error* compiler_error = nullptr;
    auto* const compiler = device->newCompiler(compiler_descriptor, &compiler_error);
    compiler_descriptor->release();

    if (compiler == nullptr)
    {
        auto error = metal_error(compiler_error, "could not create the metal compiler");
        queue->release();
        device->release();
        return error;
    }

    auto* const epoch_event = device->newSharedEvent();
    auto* const submission_event = device->newSharedEvent();
    if (epoch_event == nullptr || submission_event == nullptr)
    {
        if (epoch_event != nullptr)
            epoch_event->release();
        if (submission_event != nullptr)
            submission_event->release();
        compiler->release();
        queue->release();
        device->release();
        return cc::error("could not create the metal epoch timelines");
    }

    // From here on the context owns every handle above, so nothing below may release one — the destructor would free it
    // a second time.
    // That is why the guard-style unwinds stop here rather than continuing past construction.
    auto ctx = std::make_shared<metal_context>(device, queue, compiler, epoch_event, submission_event);
    ctx->set_adapter_info(describe(device));

    // A refusal here is the device declining an allocation, not a broken contract — so it reaches the caller as an
    // error, and the half-built context unwinds through its own shutdown as this handle drops.
    CC_RETURN_IF_ERROR(ctx->create_systems(config.upload_ring_bytes, config.download_ring_bytes));

    CC_LOG_INFO("metal context on '{}'", ctx->adapter().name);

    return sg::context_handle(cc::move(ctx));
}
