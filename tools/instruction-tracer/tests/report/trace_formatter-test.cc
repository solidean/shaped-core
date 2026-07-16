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

/// The first line containing `needle`, or empty. A register diff lives on its own instruction's line,
/// and the entry dump names every register — so an assertion about a diff has to be line-scoped or it
/// would match the dump instead.
cc::string_view line_containing(cc::string_view text, cc::string_view needle)
{
    isize start = 0;
    for (isize i = 0; i <= text.size(); ++i)
    {
        if (i != text.size() && text[i] != '\n')
            continue;

        auto const line = text.subview({.start = start, .end = i});
        if (line.contains(needle))
            return line;

        start = i + 1;
    }
    return {};
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

    // Scoped to the instruction's own line: the entry dump lists rcx too, by design.
    auto const out = render(t, {.stack = false, .source = false, .register_diffs = true});
    auto const line = line_containing(out, "add rax, rcx"); // a view into `out` — it must outlive this

    CHECK(line.contains("rax=0x3"));
    CHECK(!line.contains("rcx=")); // "rcx" is in the mnemonic; "rcx=" would be a diff
}

TEST("formatter - the last instruction's effect is shown, not dropped")
{
    // Snapshots are sampled *before* each instruction, so the final one needs the trailing snapshot
    // the session records after its last step. Without it a `ret`'s rsp move is invisible.
    trace t;
    t.index = 1;
    t.entry_symbol = "foo";
    t.instructions.push_back(insn_at(0x1000, 1, "ret", insn_category::ret));

    register_snapshot before;
    before.gpr[4] = 0x1000; // rsp

    register_snapshot after = before;
    after.gpr[4] = 0x1008; // the ret popped

    t.registers.push_back(before);
    t.registers.push_back(after); // the trailing snapshot: one more than there are instructions

    auto const out = render(t, {.stack = false, .source = false, .register_diffs = true});
    CHECK(out.contains("rsp=0x1008"));
}

TEST("formatter - flag changes print by name and value")
{
    trace t;
    t.index = 1;
    t.entry_symbol = "foo";
    t.instructions.push_back(insn_at(0x1000, 3, "sub rax, rcx"));

    register_snapshot before;
    before.rflags = 0;

    register_snapshot after;
    after.rflags = (1u << 6) | (1u << 0); // ZF and CF set

    t.registers.push_back(before);
    t.registers.push_back(after);

    auto const out = render(t, {.stack = false, .source = false, .register_diffs = true});
    CHECK(out.contains("ZF=1"));
    CHECK(out.contains("CF=1"));
    CHECK(!out.contains("rflags=0x41")); // by name and value, never the raw word
}

TEST("formatter - a cleared flag reads as 0, not as absent")
{
    trace t;
    t.index = 1;
    t.entry_symbol = "foo";
    t.instructions.push_back(insn_at(0x1000, 3, "add rax, rcx"));

    register_snapshot before;
    before.rflags = 1u << 6; // ZF set

    register_snapshot after;
    after.rflags = 0; // ZF cleared

    t.registers.push_back(before);
    t.registers.push_back(after);

    auto const out = render(t, {.stack = false, .source = false, .register_diffs = true});
    CHECK(out.contains("ZF=0"));
}

TEST("formatter - the trap flag is the tracer's own, and is never reported")
{
    // We set TF to single-step. Reporting it would describe the debugger, not the debuggee. Same for
    // IF and RF, which the traced code does not author either.
    trace t;
    t.index = 1;
    t.entry_symbol = "foo";
    t.instructions.push_back(insn_at(0x1000, 1, "nop"));

    register_snapshot before;
    before.rflags = 0;

    register_snapshot after;
    after.rflags = (1u << 8) | (1u << 9) | (1u << 16); // TF, IF, RF

    t.registers.push_back(before);
    t.registers.push_back(after);

    auto const out = render(t, {.stack = false, .source = false, .register_diffs = true});
    CHECK(!out.contains("TF"));
    CHECK(!out.contains("IF"));
    CHECK(!out.contains("RF"));
}

TEST("formatter - the entry dump gives the diffs a baseline")
{
    trace t;
    t.index = 1;
    t.entry_symbol = "foo";
    t.instructions.push_back(insn_at(0x1000, 1, "nop"));

    register_snapshot entry;
    entry.gpr[0] = 0x64;                  // rax
    entry.gpr[15] = 0xdead;               // r15 — the far end of the array must render too
    entry.rflags = (1u << 6) | (1u << 2); // ZF, PF

    t.registers.push_back(entry);
    t.registers.push_back(entry);

    auto const out = render(t, {.stack = false, .source = false, .register_diffs = true});

    CHECK(out.contains("registers:"));
    CHECK(out.contains("rax=0x0000000000000064"));
    CHECK(out.contains("r15=0x000000000000dead"));
    CHECK(out.contains("rflags=0x00000044"));
    CHECK(out.contains("[PF ZF]")); // decoded, in bit order
}

TEST("formatter - no register dump without --register-diffs")
{
    trace t = one_instruction_trace();
    t.registers.push_back({});

    CHECK(!render(t, {.register_diffs = false}).contains("registers:"));
}

TEST("formatter - ambiguity report lists every candidate")
{
    symbol_error e;
    e.message = "symbol 'process' is ambiguous";
    e.candidates.push_back({.address = 0x1000, .name = "foo::process(item const&)", .module = "mymodule.exe"});
    e.candidates.push_back({.address = 0x2000, .name = "foo::process(batch const&)", .module = "mymodule.exe"});
    e.candidates.push_back({.address = 0x3000, .name = "detail::process", .module = "helper.dll"});

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
