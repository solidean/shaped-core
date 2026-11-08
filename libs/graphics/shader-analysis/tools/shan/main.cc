#include <nexus/run.hh>

// `shan`, shader-analysis's command line: `shan analyze <module>` and `shan diff <a> <b>`, each a COMMAND.
// `shan` alone lists what it holds.
int main(int argc, char** argv)
{
    return nx::run(argc, argv);
}
