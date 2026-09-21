#include <nexus/run.hh>

// `sgl`, the command line of SGL's toolchain: every job is a COMMAND of this one nexus binary.
// `sgl <command> …` runs the command named first with the rest of the line, and `sgl` alone lists what it holds.
// A new tool of the toolchain - a formatter, a linter, the language server - is one more file with one more COMMAND.
int main(int argc, char** argv)
{
    return nx::run(argc, argv);
}
