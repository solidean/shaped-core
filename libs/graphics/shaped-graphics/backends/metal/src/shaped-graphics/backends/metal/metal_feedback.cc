#include "metal_feedback.hh"

#include <shaped-graphics/backends/metal/metal_context.hh>

namespace sg::backend::metal
{
void metal_feedback_sink::report(sg::device_error_kind kind, cc::string_view message)
{
    _context.lock(
        [&](metal_context*& ctx)
        {
            if (ctx != nullptr)
                ctx->report_feedback_error(kind, message);
        });
}

void metal_feedback_sink::notify_drained()
{
    _context.lock(
        [&](metal_context*& ctx)
        {
            if (ctx != nullptr)
                sg::impl::notify_transfer_drained(*ctx);
        });
}

void metal_feedback_sink::notify_stream_progress()
{
    _context.lock(
        [&](metal_context*& ctx)
        {
            if (ctx != nullptr)
                ctx->streams().wake_if_pending();
        });
}

void metal_feedback_sink::notify_completion_signal()
{
    _context.lock(
        [&](metal_context*& ctx)
        {
            if (ctx != nullptr)
                ctx->notify_completion_signal();
        });
}

void metal_feedback_sink::detach()
{
    _context.lock([](metal_context*& ctx) { ctx = nullptr; });
}

sg::device_error_kind device_error_kind_of(NS::Error const* error)
{
    if (error == nullptr)
        return sg::device_error_kind::validation;

    auto* const domain = error->domain();

    // Compared by value: metal-cpp declares neither domain constant, and the strings are what Metal's headers define.
    if (domain != nullptr && domain->isEqualToString(MTLSTR("MTL4CommandQueueErrorDomain")))
        return device_error_kind_of_queue_error(error->code());
    if (domain != nullptr && domain->isEqualToString(MTLSTR("MTLCommandBufferErrorDomain")))
        return device_error_kind_of_command_buffer_error(NS::UInteger(error->code()));
    return sg::device_error_kind::validation;
}

sg::device_error_kind device_error_kind_of_queue_error(NS::Integer queue_error_code)
{
    switch (queue_error_code)
    {
    case MTL4::CommandQueueErrorTimeout:
    case MTL4::CommandQueueErrorNotPermitted:
    case MTL4::CommandQueueErrorDeviceRemoved:
    case MTL4::CommandQueueErrorAccessRevoked:
        // The device is gone for this process: a hang or a fault the GPU restarted over, after which the system
        // refuses the process's submissions (NotPermitted), an eGPU unplugged, or access taken away.
        return sg::device_error_kind::device_lost;
    default:
        return sg::device_error_kind::validation;
    }
}

sg::device_error_kind device_error_kind_of_command_buffer_error(NS::UInteger command_buffer_error_code)
{
    switch (command_buffer_error_code)
    {
    case MTL::CommandBufferErrorTimeout:
    case MTL::CommandBufferErrorDeviceRemoved:
    case MTL::CommandBufferErrorAccessRevoked:
        // The device is gone: a hang the watchdog reset, an eGPU unplugged, or access taken away.
        return sg::device_error_kind::device_lost;
    default:
        return sg::device_error_kind::validation;
    }
}
} // namespace sg::backend::metal
