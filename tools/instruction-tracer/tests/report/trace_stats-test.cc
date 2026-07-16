#include <instruction-tracer/report/trace_stats.hh>
#include <nexus/test.hh>

using namespace itrace;

namespace
{
/// One instruction owned by `owner`, falling through by default. rip is arbitrary but distinct —
/// stats charge by owner_symbol, never by address.
recorded_instruction insn_in(cc::string_view owner, u64 rip = 0x1000, insn_category category = insn_category::other)
{
    recorded_instruction insn;
    insn.rip = rip;
    insn.length = 4;
    insn.byte_count = 4;
    insn.next_rip = rip + 4;
    insn.category = category;
    insn.owner_symbol = owner;
    return insn;
}

trace trace_of(cc::vector<recorded_instruction> instructions, step_reason reason = step_reason::returned)
{
    trace t;
    t.index = 1;
    t.instructions = cc::move(instructions);
    t.reason = reason;
    return t;
}

/// The row for `symbol`, or a default-constructed one so a missing row fails on its counts.
symbol_stats row_for(stats_summary const& s, cc::string_view symbol)
{
    for (auto const& r : s.rows)
        if (r.symbol == symbol)
            return r;

    return {};
}
} // namespace

TEST("stats - instructions are charged to their owning symbol")
{
    auto const t = trace_of({insn_in("foo"), insn_in("bar"), insn_in("foo"), insn_in("foo")});
    auto const s = collect_stats(cc::span<trace const>(&t, 1));

    CHECK(s.rows.size() == 2);
    CHECK(row_for(s, "foo").instructions == 3);
    CHECK(row_for(s, "bar").instructions == 1);
}

TEST("stats - rows are sorted by instructions descending")
{
    auto const t = trace_of({insn_in("cheap"), insn_in("hot"), insn_in("hot"), insn_in("hot")});
    auto const s = collect_stats(cc::span<trace const>(&t, 1));

    CHECK(s.rows[0].symbol == "hot");
    CHECK(s.rows[1].symbol == "cheap");
}

TEST("stats - ties break by symbol so the table is stable")
{
    auto const t = trace_of({insn_in("zeta"), insn_in("alpha")});
    auto const s = collect_stats(cc::span<trace const>(&t, 1));

    CHECK(s.rows[0].symbol == "alpha");
    CHECK(s.rows[1].symbol == "zeta");
}

TEST("stats - a tail call is charged to where the code is, not to who jumped")
{
    // The trap a text-parsing stack walk has to hand-code around: a `jmp` to another symbol replaces
    // the frame rather than pushing one, and missing that desynchronizes every later attribution.
    // Charging by owner_symbol cannot get this wrong — there is no stack to keep in sync.
    auto jump = insn_in("teardown_payload", 0x1000, insn_category::unconditional_branch);
    jump.next_rip = 0x9000; // diverges into another function, and never returns here

    auto const t = trace_of({insn_in("teardown_payload"), jump, insn_in("~poly_node_allocation", 0x9000),
                             insn_in("~poly_node_allocation", 0x9004)});
    auto const s = collect_stats(cc::span<trace const>(&t, 1));

    CHECK(row_for(s, "teardown_payload").instructions == 2);
    CHECK(row_for(s, "~poly_node_allocation").instructions == 2);
}

TEST("stats - atomics, memory and calls are counted per symbol")
{
    auto atomic = insn_in("poll");
    atomic.is_atomic = true;
    atomic.reads_memory = true;
    atomic.writes_memory = true;

    auto direct = insn_in("poll", 0x1000, insn_category::call);
    auto indirect = insn_in("poll", 0x1000, insn_category::call);
    indirect.is_indirect = true;

    auto load = insn_in("poll");
    load.reads_memory = true;

    auto const t = trace_of({atomic, direct, indirect, load});
    auto const row = row_for(collect_stats(cc::span<trace const>(&t, 1)), "poll");

    CHECK(row.instructions == 4);
    CHECK(row.atomics == 1);
    CHECK(row.direct_calls == 1);
    CHECK(row.indirect_calls == 1);
    CHECK(row.memory_reads == 2);
    CHECK(row.memory_writes == 1);
}

TEST("stats - expensive instructions are counted and named")
{
    auto idiv = insn_in("cc::map::bucket_of");
    idiv.slow_mnemonic = "idiv";

    auto tsc = insn_in("prof::now");
    tsc.slow_mnemonic = "rdtsc";

    auto const t = trace_of({insn_in("cc::map::bucket_of"), idiv, tsc});
    auto const s = collect_stats(cc::span<trace const>(&t, 1));

    CHECK(row_for(s, "cc::map::bucket_of").slow == 1);
    CHECK(row_for(s, "prof::now").slow == 1);

    REQUIRE(s.slow_ops.size() == 2);
    // The identity is the finding, so the mnemonic and its symbol both survive to the report.
    CHECK(s.slow_ops[0].mnemonic == "idiv");
    CHECK(s.slow_ops[0].symbol == "cc::map::bucket_of");
    CHECK(s.slow_ops[0].count == 1);
}

TEST("stats - the same op in one symbol accumulates, in another does not merge")
{
    auto a = insn_in("hash_bucket");
    a.slow_mnemonic = "idiv";
    auto b = insn_in("hash_bucket");
    b.slow_mnemonic = "idiv";
    auto elsewhere = insn_in("other");
    elsewhere.slow_mnemonic = "idiv";

    auto const t = trace_of({a, b, elsewhere});
    auto const s = collect_stats(cc::span<trace const>(&t, 1));

    REQUIRE(s.slow_ops.size() == 2); // (idiv, hash_bucket) and (idiv, other) are distinct findings
    CHECK(s.slow_ops[0].count == 2); // sorted by count, so hash_bucket leads
    CHECK(s.slow_ops[0].symbol == "hash_bucket");
    CHECK(s.slow_ops[1].count == 1);
    CHECK(row_for(s, "hash_bucket").slow == 2);
}

TEST("stats - no expensive instructions is the normal case and says nothing")
{
    auto const t = trace_of({insn_in("foo"), insn_in("bar")});
    auto const s = collect_stats(cc::span<trace const>(&t, 1));

    CHECK(s.slow_ops.empty());
    CHECK(row_for(s, "foo").slow == 0);

    // An all-zero column is itself the result: instruction count is a fair proxy here.
    auto const out = format_stats(s);
    CHECK(out.contains("slow"));      // the column stays, so the format is stable
    CHECK(!out.contains("slow ops")); // but the footer costs nothing when there is nothing to say
}

TEST("format_stats - the slow-op footer names the instruction and where it ran")
{
    auto idiv = insn_in("cc::map::bucket_of");
    idiv.slow_mnemonic = "idiv";

    auto const t = trace_of({idiv, insn_in("cc::map::bucket_of")});
    auto const out = format_stats(collect_stats(cc::span<trace const>(&t, 1)));

    CHECK(out.contains("slow ops"));
    CHECK(out.contains("idiv"));
    CHECK(out.contains("x1"));
    CHECK(out.contains("cc::map::bucket_of"));
}

TEST("stats - only taken conditional branches count as taken")
{
    auto taken = insn_in("f", 0x1000, insn_category::conditional_branch);
    taken.next_rip = 0x2000; // went somewhere else

    auto const not_taken = insn_in("f", 0x1000, insn_category::conditional_branch); // falls through

    // An unconditional jump had no choice to make, so it is not a branch for this column.
    auto const jump = insn_in("f", 0x1000, insn_category::unconditional_branch);

    auto const t = trace_of({taken, not_taken, jump});
    auto const row = row_for(collect_stats(cc::span<trace const>(&t, 1)), "f");

    CHECK(row.branches == 2);
    CHECK(row.branches_taken == 1);
}

TEST("stats - the last instruction's branch is not guessed taken")
{
    // next_rip is 0 for the final record: we never saw a successor, so "taken" is unknowable and
    // must not be invented.
    auto last = insn_in("f", 0x1000, insn_category::conditional_branch);
    last.next_rip = 0;

    auto const t = trace_of({last});
    auto const row = row_for(collect_stats(cc::span<trace const>(&t, 1)), "f");

    CHECK(row.branches == 1);
    CHECK(row.branches_taken == 0);
}

TEST("stats - traces aggregate into one table")
{
    cc::vector<trace> traces;
    traces.push_back(trace_of({insn_in("foo"), insn_in("foo")}));
    traces.push_back(trace_of({insn_in("foo"), insn_in("bar")}));

    auto const s = collect_stats(traces);

    CHECK(s.traces == 2);
    CHECK(s.rows.size() == 2);
    CHECK(row_for(s, "foo").instructions == 3);
    CHECK(row_for(s, "bar").instructions == 1);
}

TEST("stats - a budget-truncated trace is flagged")
{
    auto const complete = trace_of({insn_in("foo")}, step_reason::returned);
    CHECK(!collect_stats(cc::span<trace const>(&complete, 1)).truncated);

    auto const cut = trace_of({insn_in("foo")}, step_reason::instruction_budget);
    CHECK(collect_stats(cc::span<trace const>(&cut, 1)).truncated);
}

TEST("stats - an unsymbolized instruction still gets a row")
{
    auto const t = trace_of({insn_in("")});
    auto const s = collect_stats(cc::span<trace const>(&t, 1));

    CHECK(s.rows.size() == 1);
    CHECK(s.rows[0].symbol == "(unknown)");
}

TEST("stats - no traces at all is empty, not a crash")
{
    auto const s = collect_stats({});
    CHECK(s.rows.empty());
    CHECK(s.traces == 0);
    CHECK(format_stats(s).contains("no instructions recorded"));
}

TEST("format_stats - table carries the counts, the header and a total")
{
    auto atomic = insn_in("poll");
    atomic.is_atomic = true;

    auto const t = trace_of({insn_in("probe"), atomic, insn_in("poll")});
    auto const out = format_stats(collect_stats(cc::span<trace const>(&t, 1)));

    CHECK(out.contains("self"));
    CHECK(out.contains("atomics"));
    CHECK(out.contains("calls d/i"));
    CHECK(out.contains("mem r/w"));
    CHECK(out.contains("symbol"));

    CHECK(out.contains("poll"));
    CHECK(out.contains("probe"));
    CHECK(out.contains("total (1 trace)"));
}

TEST("format_stats - the totals row sums every column")
{
    auto atomic = insn_in("a");
    atomic.is_atomic = true;
    auto other = insn_in("b");
    other.is_atomic = true;

    cc::vector<trace> traces;
    traces.push_back(trace_of({atomic}));
    traces.push_back(trace_of({other, insn_in("b")}));

    auto const s = collect_stats(traces);
    auto const out = format_stats(s);

    CHECK(s.rows.size() == 2);
    CHECK(out.contains("total (2 traces)"));
    // 3 instructions and 2 atomics across both traces.
    CHECK(out.contains("3"));
    CHECK(out.contains("2"));
}

TEST("format_stats - truncation is called out, not left to the reader")
{
    auto const cut = trace_of({insn_in("foo")}, step_reason::instruction_budget);
    CHECK(format_stats(collect_stats(cc::span<trace const>(&cut, 1))).contains("TRUNCATED"));

    auto const complete = trace_of({insn_in("foo")}, step_reason::returned);
    CHECK(!format_stats(collect_stats(cc::span<trace const>(&complete, 1))).contains("TRUNCATED"));
}

TEST("strip_template_args - drops arguments, keeps the name")
{
    CHECK(strip_template_args("foo") == "foo");
    CHECK(strip_template_args("foo<int>") == "foo");
    CHECK(strip_template_args("cc::vector<int>::push_back") == "cc::vector::push_back");
    CHECK(strip_template_args("foo<int>::bar<T>") == "foo::bar");
}

TEST("strip_template_args - nested arguments go whole")
{
    CHECK(strip_template_args("cc::map<cc::string, cc::vector<int>>::get") == "cc::map::get");
    CHECK(strip_template_args("a<b<c<d>>>") == "a");
}

TEST("strip_template_args - operator angle brackets are not template brackets")
{
    // The failure this prevents: reading `operator<<`'s brackets as template depth swallows the rest
    // of the name.
    CHECK(strip_template_args("cc::string::operator<") == "cc::string::operator<");
    CHECK(strip_template_args("stream::operator<<") == "stream::operator<<");
    CHECK(strip_template_args("foo::operator<=>") == "foo::operator<=>");
    CHECK(strip_template_args("cc::vector<int>::operator<<") == "cc::vector::operator<<");
}

TEST("strip_template_args - a component that was all template collapses away")
{
    // A lambda or instantiation tag is a whole component: stripping it would otherwise leave the
    // empty "function_ref::::__invoke".
    CHECK(strip_template_args("cc::function_ref<void()>::<lambda_1>::__invoke") == "cc::function_ref::__invoke");
    CHECK(strip_template_args("a::<x>::<y>::b") == "a::b");
}

TEST("strip_template_args - an identifier merely ending in operator is not one")
{
    CHECK(strip_template_args("myoperator<int>") == "myoperator");
}

TEST("strip_template_args - unbalanced brackets return the name whole")
{
    // Better to print a long honest name than a silently mangled short one.
    CHECK(strip_template_args("foo<int") == "foo<int");
    CHECK(strip_template_args("foo>bar") == "foo>bar");
}

TEST("stats - rows are bucketed after template stripping")
{
    // Two instantiations of one function are one row: the table is about where time went, and the
    // argument list is not what you are reading it for.
    auto const t = trace_of({insn_in("poll<int>"), insn_in("poll<float>")});
    auto const s = collect_stats(cc::span<trace const>(&t, 1));

    CHECK(s.rows.size() == 1);
    CHECK(s.rows[0].symbol == "poll");
    CHECK(s.rows[0].instructions == 2);
}
