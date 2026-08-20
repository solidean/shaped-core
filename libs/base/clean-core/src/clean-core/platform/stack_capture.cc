#include "stack_capture.hh"

using namespace cc::primitive_defines;

isize cc::capture_stack(cc::span<void*> out, isize skip, void const* stop_frame)
{
    // Not implemented yet: a caller gets an empty capture rather than a wrong one.
    // The real walk is frame-pointer based for our own modules, addresses only, symbolized offline.
    (void)out;
    (void)skip;
    (void)stop_frame;
    return 0;
}
