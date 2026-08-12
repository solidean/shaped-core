#include <nexus/test.hh>
#include <shaped-linter/lex/directive.hh>

using namespace scl;

TEST("directive - the word names the directive, however it is spaced")
{
    CHECK(directive_word("#include <atomic>") == "include");
    CHECK(directive_word("#  endif") == "endif");
    CHECK(directive_word("#pragma once") == "pragma");
    CHECK(directive_word("#") == "");
}

TEST("directive - an include target keeps its delimiters")
{
    CHECK(include_target("#include <atomic>") == "<atomic>");
    CHECK(include_target("#include \"local.hh\"") == "\"local.hh\"");
    CHECK(include_target("#  include   <clean-core/fwd.hh>") == "<clean-core/fwd.hh>");
}

TEST("directive - only an include has a target")
{
    CHECK(include_target("#pragma once").empty());
    CHECK(include_target("#if CC_HAS_THREADS").empty());
}

TEST("directive - a header a macro spells is left alone")
{
    // Which header CONFIG_HEADER names cannot be answered from this file, so nothing is guessed at.
    CHECK(include_target("#include CONFIG_HEADER").empty());
}

TEST("directive - an unterminated target yields nothing")
{
    CHECK(include_target("#include <atomic").empty());
    CHECK(include_target("#include").empty());
}
