#include "completion.hh"

#include <clean-core/string/format.hh>
#include <nexus/args/builder.hh>
#include <nexus/args/impl/describe.hh>

// Each emitter writes the simplest script that shell will accept.
//
// Deliberately not clever: these complete option names, command names and the value sets a type publishes,
// and defer to the shell's own file completion for anything path-shaped.
// A completion script that tries to be a second parser goes stale the moment the real one changes.
//
// What an option's VALUE completes to comes from one place, `value_source` below, so the four shells cannot
// disagree about it.

namespace
{
using nx::isize;
using nx::impl::described_command;
using nx::impl::described_option;

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

/// Quotes and the characters that end a shell word are dropped rather than escaped four different ways.
/// A value carrying one is not something to complete anyway.
cc::string shell_safe(cc::string_view text)
{
    auto out = cc::string();
    for (auto const c : text)
        out += (c == '\'' || c == '"' || c == '`' || c == '$' || c == '\\' || c == '\n') ? ' ' : c;

    return out;
}

/// What an option's value should complete to, as one of three answers every shell can express.
enum class value_source
{
    nothing,    // no hint and no published set, or `complete_hint::none`
    enumerated, // the closed set the type published
    files,
    directories,
};

value_source source_of(described_option const& option)
{
    if (!option.takes_value)
        return value_source::nothing;

    switch (option.complete)
    {
    case nx::complete_hint::none:
        return value_source::nothing;
    case nx::complete_hint::files:
        return value_source::files;
    case nx::complete_hint::directories:
        return value_source::directories;
    case nx::complete_hint::automatic:
        return option.values.empty() ? value_source::nothing : value_source::enumerated;
    }

    return value_source::nothing;
}

cc::string value_words(described_option const& option)
{
    auto out = cc::string();
    for (auto const& value : option.values)
    {
        if (!out.empty())
            out += " ";

        out += shell_safe(value);
    }

    return out;
}

/// Every spelling of `option`, joined by `separator` — the case-label shape three of the four shells want.
cc::string spellings_joined(described_option const& option, cc::string_view separator)
{
    auto out = cc::string();
    for (auto const& spelling : option.spellings)
    {
        if (!out.empty())
            out += separator;

        out += spelling;
    }

    return out;
}

/// Whether any option at this level completes its value to something.
bool has_value_rules(described_command const& command)
{
    for (auto const& option : command.options)
        if (source_of(option) != value_source::nothing)
            return true;

    return false;
}

// --- bash ---------------------------------------------------------------------------------------------

cc::string bash_function_name(cc::string_view path)
{
    return cc::format("_{}_complete", sanitize(path));
}

/// One function per command, so `app build <TAB>` reaches the build level rather than the root's option list.
/// Emitted depth-first; a delegate contributes only its name, since there is nothing here to introspect.
void emit_bash(described_command const& command, cc::string_view path, cc::string& out)
{
    for (auto const& sub : command.commands)
        if (!sub.opaque)
            emit_bash(sub, cc::format("{} {}", path, sub.name), out);

    cc::format_append(out, "{}()\n{{\n", bash_function_name(path));
    out += "    local cur=\"${COMP_WORDS[COMP_CWORD]}\"\n";
    out += "    local prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n";

    if (!command.commands.empty())
    {
        // `path` is the words already consumed to reach this level, so the next one is where a subcommand's
        // name sits: COMP_WORDS[1] at the root, [2] one level down, and so on.
        auto name_index = isize(1);
        for (auto const c : cc::string_view(path))
            if (c == ' ')
                ++name_index;

        cc::format_append(out, "    if [[ $COMP_CWORD -gt {} ]]; then\n", name_index);
        cc::format_append(out, "        case \"${{COMP_WORDS[{}]}}\" in\n", name_index);

        for (auto const& sub : command.commands)
        {
            if (sub.opaque)
                continue;

            cc::format_append(out, "            {}) {}; return;;\n", sub.name,
                              bash_function_name(cc::format("{} {}", path, sub.name)));
        }

        out += "        esac\n";
        out += "    fi\n";
    }

    if (has_value_rules(command))
    {
        out += "    case \"$prev\" in\n";
        for (auto const& option : command.options)
        {
            switch (source_of(option))
            {
            case value_source::enumerated:
                cc::format_append(out, "        {}) COMPREPLY=($(compgen -W \"{}\" -- \"$cur\")); return;;\n",
                                  spellings_joined(option, "|"), value_words(option));
                break;
            case value_source::files:
                cc::format_append(out, "        {}) COMPREPLY=($(compgen -f -- \"$cur\")); return;;\n",
                                  spellings_joined(option, "|"));
                break;
            case value_source::directories:
                cc::format_append(out, "        {}) COMPREPLY=($(compgen -d -- \"$cur\")); return;;\n",
                                  spellings_joined(option, "|"));
                break;
            case value_source::nothing:
                break;
            }
        }

        out += "    esac\n";
    }

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

/// zsh reads a spec as `name[desc]:metavar:action`, so a colon or a bracket inside either half would split
/// the spec itself.
cc::string zsh_safe(cc::string_view text)
{
    auto const stripped = shell_safe(text);

    auto out = cc::string();
    for (auto const c : cc::string_view(stripped))
        out += (c == ':' || c == '[' || c == ']') ? ' ' : c;

    return out;
}

void emit_zsh(described_command const& command, cc::string& out)
{
    out += "  _arguments -s \\\n";

    for (auto const& option : command.options)
        for (auto const& spelling : option.spellings)
        {
            auto const desc = zsh_safe(option.desc);

            switch (source_of(option))
            {
            case value_source::enumerated:
                cc::format_append(out, "    '{}[{}]:{}:({})' \\\n", spelling, desc, zsh_safe(option.metavar),
                                  zsh_safe(value_words(option)));
                break;
            case value_source::files:
                cc::format_append(out, "    '{}[{}]:{}:_files' \\\n", spelling, desc, zsh_safe(option.metavar));
                break;
            case value_source::directories:
                cc::format_append(out, "    '{}[{}]:{}:_directories' \\\n", spelling, desc, zsh_safe(option.metavar));
                break;
            case value_source::nothing:
                if (option.takes_value)
                    cc::format_append(out, "    '{}[{}]:{}:' \\\n", spelling, desc, zsh_safe(option.metavar));
                else
                    cc::format_append(out, "    '{}[{}]' \\\n", spelling, desc);
                break;
            }
        }

    if (!command.commands.empty())
        out += "    '1:command:(" + command_words(command) + ")' \\\n";

    out += "    && return 0\n";
}

// --- fish ---------------------------------------------------------------------------------------------

void emit_fish(described_command const& command, cc::string_view program, cc::string& out)
{
    for (auto const& sub : command.commands)
        cc::format_append(out, "complete -c {} -n __fish_use_subcommand -a {} -d '{}'\n", program, sub.name,
                          shell_safe(sub.desc));

    for (auto const& option : command.options)
        for (auto const& spelling : option.spellings)
        {
            auto const is_long = spelling.starts_with("--");
            auto const bare = is_long ? cc::string_view(spelling).subview(2) : cc::string_view(spelling).subview(1);

            cc::format_append(out, "complete -c {} {} {}", program, is_long ? "-l" : "-s", bare);
            if (option.takes_value)
                out += " -r";

            switch (source_of(option))
            {
            case value_source::enumerated:
                cc::format_append(out, " -a '{}'", value_words(option));
                break;
            case value_source::files:
            case value_source::directories:
                out += " -F"; // fish's own file completion, which -r would otherwise have suppressed
                break;
            case value_source::nothing:
                break;
            }

            if (!option.desc.empty())
                cc::format_append(out, " -d '{}'", shell_safe(option.desc));

            out += "\n";
        }
}

// --- powershell ---------------------------------------------------------------------------------------

void emit_powershell(described_command const& command, cc::string_view program, cc::string& out)
{
    cc::format_append(out, "Register-ArgumentCompleter -Native -CommandName {} -ScriptBlock {{\n", program);
    out += "    param($wordToComplete, $commandAst, $cursorPosition)\n";

    // The value sets, keyed by every spelling that leads to them.
    out += "    $valuesFor = @{\n";
    for (auto const& option : command.options)
    {
        if (source_of(option) != value_source::enumerated)
            continue;

        for (auto const& spelling : option.spellings)
        {
            cc::format_append(out, "        '{}' = @(", spelling);
            for (auto i = isize(0); i < option.values.size(); ++i)
            {
                if (i > 0)
                    out += ", ";

                cc::format_append(out, "'{}'", shell_safe(option.values[i]));
            }

            out += ")\n";
        }
    }
    out += "    }\n";

    // The word before the one being completed, which is what says whether a value set applies.
    out += "    $elements = @($commandAst.CommandElements | ForEach-Object { $_.ToString() })\n";
    out += "    $previous = ''\n";
    out += "    if ($wordToComplete) { if ($elements.Count -ge 2) { $previous = $elements[$elements.Count - 2] } }\n";
    out += "    elseif ($elements.Count -ge 1) { $previous = $elements[$elements.Count - 1] }\n";
    out += "    if ($valuesFor.ContainsKey($previous)) {\n";
    out += "        return $valuesFor[$previous] |\n";
    out += "            Where-Object { $_ -like \"$wordToComplete*\" } |\n";
    out += "            ForEach-Object { [System.Management.Automation.CompletionResult]::new($_, $_, "
           "'ParameterValue', $_) }\n";
    out += "    }\n";

    out += "    $candidates = @(\n";

    auto first = true;
    auto const append = [&](cc::string_view value, cc::string_view desc)
    {
        if (!first)
            out += ",\n";

        first = false;
        cc::format_append(out, "        @{{ Text = '{}'; Tip = '{}' }}", value, shell_safe(desc));
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
        cc::format_append(out, "complete -F {} {}\n", bash_function_name(program), program);
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
