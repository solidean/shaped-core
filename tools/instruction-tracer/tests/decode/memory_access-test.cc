#include <clean-core/container/vector.hh>
#include <instruction-tracer/decode/memory_access.hh>
#include <nexus/test.hh>

using namespace itrace;

namespace
{
recorded_instruction at(u64 rip, cc::span<u8 const> bytes)
{
    recorded_instruction insn;
    insn.rip = rip;
    insn.byte_count = u8(bytes.size());
    for (isize i = 0; i < bytes.size(); ++i)
        insn.bytes[i] = bytes[i];
    return insn;
}

/// A snapshot with GPRs set by encoding-order index (rax=0, rcx=1, rdx=2, rbx=3, rsp=4, rbp=5,
/// rsi=6, rdi=7).
register_snapshot regs_with(int i, u64 v, int j = -1, u64 w = 0)
{
    register_snapshot s;
    s.gpr[i] = v;
    if (j >= 0)
        s.gpr[j] = w;
    return s;
}
} // namespace

TEST("memory - mov reg, [base+disp] resolves a read")
{
    // 48 8B 43 08 = mov rax, [rbx+0x08]
    u8 const bytes[] = {0x48, 0x8B, 0x43, 0x08};
    auto const insn = at(0x140001000, bytes);
    auto const ops = decode_memory_operands(insn, regs_with(3, 0x1000));

    REQUIRE(ops.size() == 1);
    CHECK(ops[0].address == 0x1008);
    CHECK(ops[0].size == 8);
    CHECK(ops[0].is_read);
    CHECK(!ops[0].is_write);
}

TEST("memory - mov [base], reg resolves a write")
{
    // 48 89 0E = mov [rsi], rcx
    u8 const bytes[] = {0x48, 0x89, 0x0E};
    auto const insn = at(0x140001000, bytes);
    auto const ops = decode_memory_operands(insn, regs_with(6, 0x2000));

    REQUIRE(ops.size() == 1);
    CHECK(ops[0].address == 0x2000);
    CHECK(ops[0].size == 8);
    CHECK(!ops[0].is_read);
    CHECK(ops[0].is_write);
}

TEST("memory - base + index*scale + disp")
{
    // 8B 44 8B 10 = mov eax, [rbx + rcx*4 + 0x10]
    u8 const bytes[] = {0x8B, 0x44, 0x8B, 0x10};
    auto const insn = at(0x140001000, bytes);
    auto const ops = decode_memory_operands(insn, regs_with(3, 0x1000, 1, 0x10));

    REQUIRE(ops.size() == 1);
    CHECK(ops[0].address == 0x1000 + 0x10 * 4 + 0x10);
    CHECK(ops[0].size == 4); // 32-bit eax
    CHECK(ops[0].is_read);
}

TEST("memory - rip-relative uses the next instruction's address")
{
    // 48 8B 05 00 00 00 00 = mov rax, [rip+0x0]; the base is the address after the 7-byte insn.
    u8 const bytes[] = {0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00};
    auto const insn = at(0x140001000, bytes);
    auto const ops = decode_memory_operands(insn, {});

    REQUIRE(ops.size() == 1);
    CHECK(ops[0].address == 0x140001007);
    CHECK(ops[0].is_read);
}

TEST("memory - lea computes an address but touches no memory")
{
    // 48 8D 43 08 = lea rax, [rbx+0x08]
    u8 const bytes[] = {0x48, 0x8D, 0x43, 0x08};
    auto const insn = at(0x140001000, bytes);
    CHECK(decode_memory_operands(insn, regs_with(3, 0x1000)).empty());
}

TEST("memory - push writes the stack")
{
    // 50 = push rax. The exact slot (rsp vs rsp-8) is Zydis's to model; the write and size are ours.
    u8 const bytes[] = {0x50};
    auto const insn = at(0x140001000, bytes);
    auto const ops = decode_memory_operands(insn, regs_with(4, 0x9000));

    REQUIRE(ops.size() == 1);
    CHECK(ops[0].is_write);
    CHECK(ops[0].size == 8);
    CHECK((ops[0].address == 0x9000 || ops[0].address == 0x8FF8));
}

TEST("memory - undecodable bytes yield nothing")
{
    u8 const bytes[] = {0xFF, 0xFF, 0xFF};
    CHECK(decode_memory_operands(at(0x140001000, bytes), {}).empty());
}

TEST("classify_region - stack frame vs caller vs heap")
{
    // Two frames on a [0x1000, 0x3000) stack: outer at 0x2800, current at 0x2400.
    u64 const bases[] = {0x2800, 0x2400};
    cc::span<u64 const> const frames = bases;
    isize owner = -1;

    // Below the current base: the current function's own frame.
    CHECK(classify_region(0x2300, 0x1000, 0x3000, frames, owner) == access_region::frame);
    CHECK(owner == 1);

    // Between the current and outer base: a caller's frame — a span reaching up the stack.
    CHECK(classify_region(0x2500, 0x1000, 0x3000, frames, owner) == access_region::stack);
    CHECK(owner == 0);

    // Above every tracked frame: still stack, but no owner we recorded.
    CHECK(classify_region(0x2900, 0x1000, 0x3000, frames, owner) == access_region::stack);
    CHECK(owner == -1);

    // Off the stack entirely: heap (or a global).
    CHECK(classify_region(0x8000, 0x1000, 0x3000, frames, owner) == access_region::heap);
    CHECK(owner == -1);
    CHECK(classify_region(0x0500, 0x1000, 0x3000, frames, owner) == access_region::heap);
}

TEST("classify_region - no stack bounds means heap")
{
    u64 const bases[] = {0x2400};
    isize owner = -1;
    CHECK(classify_region(0x2300, 0, 0, bases, owner) == access_region::heap);
}

TEST("region_name")
{
    CHECK(region_name(access_region::heap) == "heap");
    CHECK(region_name(access_region::frame) == "frame");
    CHECK(region_name(access_region::stack) == "stack");
    CHECK(region_name(access_region::instructions) == "instructions");
}
