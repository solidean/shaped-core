#include <nexus/test.hh>
#include <sv_shaders.hh>

// sv's shader package, checked against the shaders it was generated from.
//
// The generator parses each source in Python and emits a constant binding table; slib's runtime pass parses
// the same bytes in C++. Nothing but this compares the two over what we actually ship — the shared corpus
// covers the cases somebody thought of, and the reflection cross-check lives in slib's own tests.
//
// Here rather than on the render path: self_check re-parses every embedded source, which is a build-time
// property to check once per run, not something a frame should pay for.
// No device either, so it runs everywhere the package is generated rather than only where dx12 and DXC are.
TEST("sv - the generated shader tables still describe the shaders they came from")
{
    auto const message = sv::shaders::self_check();
    CHECK(message.empty());
    if (!message.empty())
        FAIL(message);
}
