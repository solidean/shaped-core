#include <instruction-tracer/report/trace_formatter.hh>
#include <nexus/test.hh>

using namespace itrace;

namespace
{
recorded_instruction insn_at(u64 rip, u8 length, cc::string_view text, insn_category category = insn_category::other)
{
    recorded_instruction insn;
    insn.rip = rip;
    insn.next_rip = rip + length; // fallthrough by default
    insn.length = length;
    insn.byte_count = length;
    insn.text = text;
    insn.category = category;
    return insn;
}

cc::string render(trace const& t, format_options opts = {})
{
    source_cache sources; // no real files; source text is simply absent
    return format_trace(t, 1, opts, sources);
}

trace one_instruction_trace()
{
    trace t;
    t.index = 1;
    t.hit_index = 101;
    t.thread_id = 18472;
    t.entry_symbol = "mymodule.exe!foo::bar";
    t.return_symbol = "mymodule.exe!caller+0x8f";
    t.instructions.push_back(insn_at(0x7ff611203410, 1, "ret", insn_category::ret));
    t.reason = step_reason::returned;
    return t;
}
} // namespace

TEST("format_address - grouped like a debugger prints it")
{
    CHECK(format_address(0x7ff611203410ull) == "00007ff6`11203410");
    CHECK(format_address(0) == "00000000`00000000");
}

TEST("formatter - header carries thread, hit, entry and return")
{
    auto const out = render(one_instruction_trace());

    CHECK(out.contains("=== trace 1/1: mymodule.exe!foo::bar ==="));
    CHECK(out.contains("thread: 18472"));
    CHECK(out.contains("hit:    101"));
    CHECK(out.contains("return: mymodule.exe!caller+0x8f"));
    CHECK(out.contains("00007ff6`11203410"));
    CHECK(out.contains("ret"));
    CHECK(out.contains("trace ended: original stack frame returned"));
    CHECK(out.contains("instructions: 1"));
}

TEST("formatter - every stop reason renders")
{
    for (auto const reason : {step_reason::instruction_budget, step_reason::returned, step_reason::syscall,
                              step_reason::exception, step_reason::process_exited})
    {
        auto t = one_instruction_trace();
        t.reason = reason;

        auto const out = render(t);
        CHECK(out.contains("trace ended:"));
        CHECK(!out.contains("unknown"));
    }
}

TEST("formatter - conditional branch is annotated taken / not taken")
{
    // Taken: next_rip is not the fallthrough.
    {
        auto insn = insn_at(0x1000, 2, "je 0x1020", insn_category::conditional_branch);
        insn.next_rip = 0x1020;
        insn.target_symbol = "mod.exe!zero_path";

        auto const out = format_instruction(insn);
        CHECK(out.contains("; taken"));
        CHECK(out.contains("-> mod.exe!zero_path"));
        CHECK(!out.contains("not taken"));
    }

    // Not taken: next_rip is rip + length.
    {
        auto const insn = insn_at(0x1000, 2, "je 0x1020", insn_category::conditional_branch);
        auto const out = format_instruction(insn);
        CHECK(out.contains("; not taken"));
    }
}

TEST("formatter - indirect call shows where it actually landed")
{
    // The point of the tool: static disassembly cannot know this target.
    auto insn = insn_at(0x1000, 2, "call rax", insn_category::call);
    insn.next_rip = 0x9000;
    insn.target_symbol = "allocator.dll!allocate+0x20";

    auto const out = format_instruction(insn);
    CHECK(out.contains("call rax"));
    CHECK(out.contains("-> allocator.dll!allocate+0x20"));
    CHECK(!out.contains("taken")); // "taken" is only meaningful for a conditional
}

TEST("formatter - a plain instruction gets no branch note")
{
    auto const out = format_instruction(insn_at(0x1000, 3, "mov rbp, rsp"));
    CHECK(out.contains("mov rbp, rsp"));
    CHECK(!out.contains(";"));
}

TEST("formatter - undecoded bytes fall back to hex")
{
    recorded_instruction insn;
    insn.rip = 0x1000;
    insn.byte_count = 2;
    insn.bytes[0] = 0x0F;
    insn.bytes[1] = 0x0B;
    // no text, no length: the decoder could not read it

    auto const out = format_instruction(insn);
    CHECK(out.contains("0f 0b"));
}

TEST("formatter - source headings appear only where the location changes")
{
    trace t;
    t.index = 1;
    t.entry_symbol = "foo";

    auto a = insn_at(0x1000, 2, "test edx, edx");
    a.file = "foo.cpp";
    a.line = 144;

    auto b = insn_at(0x1002, 2, "je 0x1010");
    b.file = "foo.cpp";
    b.line = 144; // same line: must not repeat the heading

    auto c = insn_at(0x1004, 3, "mov rax, [rcx+0x10]");
    c.file = "foo.cpp";
    c.line = 149;

    t.instructions.push_back(cc::move(a));
    t.instructions.push_back(cc::move(b));
    t.instructions.push_back(cc::move(c));

    auto const out = render(t);

    CHECK(out.contains("foo.cpp:144"));
    CHECK(out.contains("foo.cpp:149"));

    // 144 heads its group exactly once, despite two instructions.
    isize count = 0;
    for (isize i = out.find("foo.cpp:144"); i >= 0; i = out.find("foo.cpp:144", i + 1))
        ++count;
    CHECK(count == 1);
}

TEST("formatter - --no-source drops the headings")
{
    trace t;
    t.index = 1;
    t.entry_symbol = "foo";

    auto a = insn_at(0x1000, 2, "test edx, edx");
    a.file = "foo.cpp";
    a.line = 144;
    t.instructions.push_back(cc::move(a));

    auto const out = render(t, {.stack = true, .source = false, .register_diffs = false});
    CHECK(!out.contains("foo.cpp:144"));
    CHECK(out.contains("test edx, edx"));
}

TEST("formatter - stack renders and --no-stack drops it")
{
    trace t = one_instruction_trace();

    stack_frame f;
    f.rip = 0x7ff611203410;
    f.symbol = "foo::bar";
    f.file = "foo.cpp";
    f.line = 142;
    t.entry_stack.push_back(cc::move(f));

    auto const with = render(t, {.stack = true, .source = true, .register_diffs = false});
    CHECK(with.contains("stack:"));
    CHECK(with.contains("foo::bar"));
    CHECK(with.contains("foo.cpp:142"));

    auto const without = render(t, {.stack = false, .source = true, .register_diffs = false});
    CHECK(!without.contains("stack:"));
}

TEST("formatter - a frame without source still renders")
{
    trace t = one_instruction_trace();

    stack_frame f;
    f.rip = 0x7ff611203410;
    f.symbol = "stripped_frame";
    t.entry_stack.push_back(cc::move(f));

    auto const out = render(t);
    CHECK(out.contains("stripped_frame"));
}

TEST("formatter - register diffs show only what changed")
{
    trace t;
    t.index = 1;
    t.entry_symbol = "foo";
    t.instructions.push_back(insn_at(0x1000, 3, "add rax, rcx"));
    t.instructions.push_back(insn_at(0x1003, 1, "ret", insn_category::ret));

    register_snapshot before;
    before.gpr[0] = 1; // rax
    before.gpr[1] = 2; // rcx

    register_snapshot after = before;
    after.gpr[0] = 3; // rax changed, rcx did not

    t.registers.push_back(before);
    t.registers.push_back(after);

    auto const out = render(t, {.stack = false, .source = false, .register_diffs = true});
    CHECK(out.contains("rax=0x3"));
    CHECK(!out.contains("rcx="));
}

TEST("formatter - ambiguity report lists every candidate")
{
    symbol_error e;
    e.message = "symbol 'process' is ambiguous";
    e.candidates.push_back({0x1000, "foo::process(item const&)", "mymodule.exe"});
    e.candidates.push_back({0x2000, "foo::process(batch const&)", "mymodule.exe"});
    e.candidates.push_back({0x3000, "detail::process", "helper.dll"});

    auto const out = format_symbol_error(e);
    CHECK(out.contains("is ambiguous"));
    CHECK(out.contains("mymodule.exe!foo::process(item const&)"));
    CHECK(out.contains("mymodule.exe!foo::process(batch const&)"));
    CHECK(out.contains("helper.dll!detail::process"));
    CHECK(out.contains("--target module!symbol"));
}

TEST("formatter - a plain error prints no candidate section")
{
    symbol_error e;
    e.message = "no symbol matching 'nope'";

    auto const out = format_symbol_error(e);
    CHECK(out == "no symbol matching 'nope'");
}
