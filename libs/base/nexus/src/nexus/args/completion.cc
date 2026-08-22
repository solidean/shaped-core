#include "completion.hh"

#include <clean-core/string/format.hh>
#include <nexus/args/builder.hh>
#include <nexus/args/impl/describe.hh>

// Each emitter writes the simplest script that shell will accept.
//
// Deliberately not clever: these complete option names, command names and enumerated values, and defer to
// the shell's own file completion for anything path-shaped.
// A completion script that tries to be a second parser goes stale the moment the real one changes.

namespace
{
using nx::impl::described_command;

/// Every option spelling at one level, space-separated.
cc::string option_words(described_command const& command)
{
    auto out = cc::string();
    for (auto const& option : command.options)
        for (auto const& spelling : option.spellings)
        {
            if (!out.empty())
                out += " ";

            out += spelling;
        }

    return out;
}

cc::string command_words(described_command const& command)
{
    auto out = cc::string();
    for (auto const& sub : command.commands)
    {
        if (!out.empty())
            out += " ";

        out += sub.name;
    }

    return out;
}

/// A name usable as a shell identifier, so nested commands get one function each.
cc::string sanitize(cc::string_view text)
{
    auto out = cc::string();
    for (auto const c : text)
        out += (cc::is_alphanumeric(c) ? c : '_');

    return out;
}

// --- bash ---------------------------------------------------------------------------------------------

void emit_bash(described_command const& command, cc::string_view path, cc::string& out)
{
    auto const fn = cc::format("_{}_complete", sanitize(path));

    cc::format_append(out, "{}()\n{{\n", fn);
    cc::format_append(out, "    local cur=\"${{COMP_WORDS[COMP_CWORD]}}\"\n");
    cc::format_append(out, "    local opts=\"{}\"\n", option_words(command));
    cc::format_append(out, "    local cmds=\"{}\"\n", command_words(command));
    out += "    if [[ \"$cur\" == -* ]]; then\n";
    out += "        COMPREPLY=($(compgen -W \"$opts\" -- \"$cur\"))\n";
    out += "    else\n";
    out += "        COMPREPLY=($(compgen -W \"$cmds\" -- \"$cur\"))\n";
    out += "    fi\n";
    out += "}\n\n";
}

// --- zsh ----------------------------------------------------------------------------------------------

void emit_zsh(described_command const& command, cc::string& out)
{
    out += "  _arguments -s \\\n";

    for (auto const& option : command.options)
        for (auto const& spelling : option.spellings)
        {
            // The description is quoted into zsh's own [desc] form, so a colon in it would split the spec.
            auto desc = cc::string();
            for (auto const c : cc::string_view(option.desc))
                desc += (c == ':' || c == '\'' || c == '[' || c == ']') ? ' ' : c;

            if (option.takes_value)
                cc::format_append(out, "    '{}[{}]:{}:' \\\n", spelling, desc, option.metavar);
            else
                cc::format_append(out, "    '{}[{}]' \\\n", spelling, desc);
        }

    if (!command.commands.empty())
        out += "    '1:command:(" + command_words(command) + ")' \\\n";

    out += "    && return 0\n";
}

// --- fish ---------------------------------------------------------------------------------------------

void emit_fish(described_command const& command, cc::string_view program, cc::string& out)
{
    for (auto const& sub : command.commands)
        cc::format_append(out, "complete -c {} -n __fish_use_subcommand -a {} -d '{}'\n", program, sub.name, sub.desc);

    for (auto const& option : command.options)
        for (auto const& spelling : option.spellings)
        {
            auto const is_long = spelling.starts_with("--");
            auto const bare = is_long ? cc::string_view(spelling).subview(2) : cc::string_view(spelling).subview(1);

            cc::format_append(out, "complete -c {} {} {}", program, is_long ? "-l" : "-s", bare);
            if (option.takes_value)
                out += " -r";

            if (!option.desc.empty())
                cc::format_append(out, " -d '{}'", option.desc);

            out += "\n";
        }
}

// --- powershell ---------------------------------------------------------------------------------------

void emit_powershell(described_command const& command, cc::string_view program, cc::string& out)
{
    cc::format_append(out, "Register-ArgumentCompleter -Native -CommandName {} -ScriptBlock {{\n", program);
    out += "    param($wordToComplete, $commandAst, $cursorPosition)\n";
    out += "    $candidates = @(\n";

    auto first = true;
    auto const append = [&](cc::string_view value, cc::string_view desc)
    {
        if (!first)
            out += ",\n";

        first = false;
        auto clean = cc::string();
        for (auto const c : cc::string_view(desc))
            clean += (c == '\'') ? ' ' : c;

        cc::format_append(out, "        @{{ Text = '{}'; Tip = '{}' }}", value, clean);
    };

    for (auto const& sub : command.commands)
        append(sub.name, sub.desc);

    for (auto const& option : command.options)
        for (auto const& spelling : option.spellings)
            append(spelling, option.desc);

    out += "\n    )\n";
    out += "    $candidates |\n";
    out += "        Where-Object { $_.Text -like \"$wordToComplete*\" } |\n";
    out += "        ForEach-Object { [System.Management.Automation.CompletionResult]::new($_.Text, $_.Text, "
           "'ParameterValue', $_.Tip) }\n";
    out += "}\n";
}
} // namespace

cc::optional<nx::completion_shell> nx::completion_shell_from_name(cc::string_view name)
{
    if (name == "bash")
        return completion_shell::bash;
    if (name == "zsh")
        return completion_shell::zsh;
    if (name == "fish")
        return completion_shell::fish;
    if (name == "powershell" || name == "pwsh")
        return completion_shell::powershell;

    return cc::nullopt;
}

cc::string nx::generate_completion(args_builder& builder, completion_shell shell)
{
    auto const described = impl::describer::run(builder);
    auto const program = described.name.empty() ? cc::string("program") : described.name;

    auto out = cc::string();

    switch (shell)
    {
    case completion_shell::bash:
        emit_bash(described, program, out);
        cc::format_append(out, "complete -F _{}_complete {}\n", sanitize(program), program);
        break;

    case completion_shell::zsh:
        cc::format_append(out, "#compdef {}\n\n_{}()\n{{\n", program, sanitize(program));
        emit_zsh(described, out);
        cc::format_append(out, "}}\n\ncompdef _{} {}\n", sanitize(program), program);
        break;

    case completion_shell::fish:
        emit_fish(described, program, out);
        break;

    case completion_shell::powershell:
        emit_powershell(described, program, out);
        break;
    }

    return out;
}
