// logic.h - pure logic for the Quick Shell plugin (ra9fael.shell).
//
// Everything here is OS-independent and side-effect free, so tests/test_logic.cpp
// can cover it on any C++17 compiler without starting processes. The Win32 side
// (shell detection, CreateProcess, clipboard, history file IO) lives in
// runner.h; main.cpp only glues the two onto the SDK.
//
// Comments are English per the project convention; user-visible strings are
// localized in main.cpp with tr(zh, en) - the functions here return stable
// codes, never display text.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace shellplug {

// ---- small text helpers --------------------------------------------------------

inline std::string lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline bool startsWith(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Case-insensitive substring on ASCII.
inline bool hasCi(const std::string& haystack, const std::string& needle)
{
    const std::string h = lower(haystack);
    const std::string n = lower(needle);
    return h.find(n) != std::string::npos;
}

// Replace every \n with the platform line ending the shell wants.
inline std::string normalizeLines(std::string s, const char* ending)
{
    // First make every ending \r\n or \n so the substitution below is uniform.
    std::string t;
    t.reserve(s.size() + 16);
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            if (i + 1 < s.size() && s[i + 1] == '\n')
                ++i;
            t += '\n';
        } else
            t += s[i];
    }
    if (std::string(ending) == "\n")
        return t;
    std::string out;
    out.reserve(t.size() + t.size() / 8);
    for (char c : t) {
        if (c == '\n')
            out += ending;
        else
            out += c;
    }
    return out;
}

// First line of a text, cut to `max` characters (UTF-8 aware, appends …).
inline std::string firstLineOf(const std::string& text, std::size_t max = 80)
{
    std::size_t end = text.find('\n');
    std::string line = text.substr(0, end == std::string::npos ? text.size() : end);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.pop_back();
    if (line.size() <= max)
        return line;
    // Cut on a UTF-8 boundary.
    std::size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
        --cut;
    return line.substr(0, cut) + "…";
}

// 3.2s / 1:05 style duration text is localized at the call site; this only
// gives the number formatting shared by both languages.
inline std::string formatSeconds(double seconds)
{
    char buffer[32];
    if (seconds < 60.0) {
        std::snprintf(buffer, sizeof buffer, "%.1fs", seconds < 0 ? 0.0 : seconds);
        return buffer;
    }
    const int total = static_cast<int>(seconds);
    std::snprintf(buffer, sizeof buffer, "%d:%02d", total / 60, total % 60);
    return buffer;
}

// ---- input parsing (design 5.2 / 10.1) -----------------------------------------

struct ParsedInput {
    enum class Kind { Empty, Command, ResultPage, ShellList, ShellRequest };
    Kind kind = Kind::Empty;
    std::string shellName; // ShellRequest: the name after '@'
    std::string command;   // Command / ShellRequest: the words to run
    long long runId = 0;   // ResultPage: the run id (0 = the latest run)
};

inline ParsedInput parseInput(const std::string& text)
{
    ParsedInput p;
    // A leading space means the '@' is part of the command, not a shell name
    // (design 10.1: " @x" is not a shell request).
    const bool atShell = !text.empty() && text[0] == '@';
    if (!atShell) {
        if (text.empty())
            return p; // Empty
        if (text == "#") {
            p.kind = ParsedInput::Kind::ResultPage;
            return p;
        }
        // Only a bare "#" or "#r<digits>" opens a result page; "# comment"
        // and "#rx" stay commands (in bash and pwsh a lone '#' is a comment).
        if (startsWith(text, "#r")) {
            bool digits = text.size() > 2;
            long long id = 0;
            for (std::size_t i = 2; i < text.size() && digits; ++i) {
                if (text[i] < '0' || text[i] > '9') {
                    digits = false;
                    break;
                }
                id = id * 10 + (text[i] - '0');
            }
            if (digits) {
                p.kind = ParsedInput::Kind::ResultPage;
                p.runId = id;
                return p;
            }
        }
        p.kind = ParsedInput::Kind::Command;
        p.command = text;
        return p;
    }
    // '@' request: "@" alone lists shells, "@name words…" selects a shell.
    const std::size_t space = text.find(' ');
    const std::string name = space == std::string::npos ? text.substr(1) : text.substr(1, space - 1);
    if (name.empty()) {
        p.kind = ParsedInput::Kind::ShellList;
        return p;
    }
    p.kind = ParsedInput::Kind::ShellRequest;
    p.shellName = name;
    p.command = space == std::string::npos ? std::string() : text.substr(space + 1);
    return p;
}

// ---- built-in shell names and resolution (design 4.1) ---------------------------

// Stable names used in settings, history and data keys.
constexpr const char* kCmd = "cmd";
constexpr const char* kPwsh = "pwsh";
constexpr const char* kPowerShell = "powershell"; // Windows PowerShell 5.1
constexpr const char* kBash = "bash";             // Git Bash
constexpr const char* kMsys = "msys";
constexpr const char* kWsl = "wsl";

inline std::vector<std::string> builtinShellNames()
{
    return {kCmd, kPwsh, kPowerShell, kBash, kMsys, kWsl};
}

// The default shell for '>' when defaultShell=auto: pwsh → powershell → cmd.
// `present` maps a built-in name to whether the machine has it.
inline std::string autoDefaultShell(const std::map<std::string, bool>& present)
{
    for (const char* name : {kPwsh, kPowerShell, kCmd}) {
        const auto it = present.find(name);
        if (it != present.end() && it->second)
            return name;
    }
    return {};
}

// Which shell a command id runs on. commandId: "default" (uses `defaultShell`,
// "auto" resolved through `present`), a built-in name, or empty (the command id
// is a custom shell's name). Returns an empty string when nothing fits.
inline std::string resolveShellName(const std::string& commandId,
                                    const std::string& defaultShell,
                                    const std::map<std::string, bool>& present,
                                    const std::vector<std::string>& customNames)
{
    const auto have = [&](const std::string& name) {
        const auto it = present.find(name);
        return it != present.end() && it->second;
    };
    if (commandId == "default") {
        if (defaultShell == "auto" || defaultShell.empty())
            return autoDefaultShell(present);
        // A custom shell named in the setting, or a built-in that exists.
        for (const std::string& n : customNames)
            if (n == defaultShell)
                return n;
        return have(defaultShell) ? defaultShell : std::string();
    }
    for (const std::string& n : customNames)
        if (n == commandId)
            return n;
    return have(commandId) ? commandId : std::string();
}

// ---- custom shells (design 4.4 / 10.1) ------------------------------------------

struct CustomShell {
    std::string name;
    std::string program;      // full path or exe name
    std::string args;         // may contain {script} / {wslscript}
    std::string ext = ".txt"; // script extension
    bool wsl = false;         // {wslscript} used: the path goes through /mnt
};

struct CustomShellProblem {
    int index = 0; // 1-based entry number in the setting value
    enum class Code { BadFormat, BadName, Duplicate, Reserved };
    Code code = Code::BadFormat;
};

struct CustomShellParse {
    std::vector<CustomShell> shells;
    std::vector<CustomShellProblem> problems;
};

// Entries are split by '|' (a '|' cannot appear in a Windows path):
//   name=program args…[;ext=.xx]
// The program may be quoted; {script} in the args is the script path ({wslscript}
// the /mnt/… form). Without a placeholder the path is appended last.
inline CustomShellParse parseCustomShells(const std::string& value,
                                          const std::vector<std::string>& reservedNames)
{
    CustomShellParse out;
    int index = 0;
    std::size_t start = 0;
    const auto validName = [](const std::string& n) {
        if (n.empty())
            return false;
        for (char c : n) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                            || c == '-' || c == '_';
            if (!ok)
                return false;
        }
        return true;
    };
    while (start <= value.size()) {
        const std::size_t bar = value.find('|', start);
        const std::string entry = value.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        ++index;
        start = bar == std::string::npos ? value.size() + 1 : bar + 1;
        std::size_t a = 0;
        std::size_t b = entry.size();
        while (a < b && (entry[a] == ' ' || entry[a] == '\t'))
            ++a;
        while (b > a && (entry[b - 1] == ' ' || entry[b - 1] == '\t'))
            --b;
        const std::string body = entry.substr(a, b - a);
        if (body.empty())
            continue; // tolerate "a | | b" style blanks
        const std::size_t eq = body.find('=');
        if (eq == std::string::npos) {
            out.problems.push_back({index, CustomShellProblem::Code::BadFormat});
            continue;
        }
        CustomShell sh;
        sh.name = body.substr(0, eq);
        std::size_t ra = eq + 1;
        std::size_t rb = body.size();
        while (ra < rb && (body[ra] == ' ' || body[ra] == '\t'))
            ++ra;
        while (rb > ra && (body[rb - 1] == ' ' || body[rb - 1] == '\t'))
            --rb;
        std::string rest = body.substr(ra, rb - ra);
        if (!validName(sh.name)) {
            out.problems.push_back({index, CustomShellProblem::Code::BadName});
            continue;
        }
        // Trailing ;ext=.xx (the last occurrence, so paths may contain ';').
        const std::string extMark = ";ext=";
        std::size_t extAt = rest.rfind(extMark);
        if (extAt != std::string::npos) {
            std::string ext = rest.substr(extAt + extMark.size());
            std::size_t ea = 0;
            std::size_t eb = ext.size();
            while (ea < eb && (ext[ea] == ' ' || ext[ea] == '\t'))
                ++ea;
            while (eb > ea && (ext[eb - 1] == ' ' || ext[eb - 1] == '\t'))
                --eb;
            ext = ext.substr(ea, eb - ea);
            if (!ext.empty() && ext[0] == '.' && ext.size() >= 2
                && ext.find_first_of(" \t|=\"") == std::string::npos)
                sh.ext = lower(ext);
            rest = rest.substr(0, extAt);
            while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t'))
                rest.pop_back();
        }
        // Program: quoted, or the first space-separated token.
        if (!rest.empty() && rest[0] == '"') {
            const std::size_t close = rest.find('"', 1);
            if (close == std::string::npos) {
                out.problems.push_back({index, CustomShellProblem::Code::BadFormat});
                continue;
            }
            sh.program = rest.substr(1, close - 1);
            std::string args = rest.substr(close + 1);
            std::size_t aa = 0;
            while (aa < args.size() && (args[aa] == ' ' || args[aa] == '\t'))
                ++aa;
            sh.args = args.substr(aa);
        } else {
            const std::size_t sp = rest.find(' ');
            sh.program = sp == std::string::npos ? rest : rest.substr(0, sp);
            sh.args = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
        }
        if (sh.program.empty()) {
            out.problems.push_back({index, CustomShellProblem::Code::BadFormat});
            continue;
        }
        bool reserved = false;
        for (const std::string& n : reservedNames)
            if (n == sh.name)
                reserved = true;
        if (reserved) {
            out.problems.push_back({index, CustomShellProblem::Code::Reserved});
            continue;
        }
        bool dup = false;
        for (const CustomShell& s : out.shells)
            if (s.name == sh.name)
                dup = true;
        if (dup) {
            out.problems.push_back({index, CustomShellProblem::Code::Duplicate});
            continue;
        }
        // Script extension from the program name when not given.
        if (extAt == std::string::npos) {
            const std::size_t slash = sh.program.find_last_of("/\\");
            const std::string exe = lower(sh.program.substr(slash == std::string::npos ? 0 : slash + 1));
            if (exe == "bash.exe" || exe == "sh.exe" || exe == "zsh.exe" || exe == "fish.exe"
                || exe == "bash" || exe == "sh" || exe == "zsh" || exe == "fish")
                sh.ext = ".sh";
            else if (exe == "pwsh.exe" || exe == "powershell.exe" || exe == "pwsh" || exe == "powershell")
                sh.ext = ".ps1";
            else if (exe == "cmd.exe" || exe == "cmd")
                sh.ext = ".cmd";
        }
        sh.wsl = sh.args.find("{wslscript}") != std::string::npos;
        out.shells.push_back(std::move(sh));
    }
    return out;
}

// Windows quoting: wrap only when the path itself needs it (spaces, tabs).
inline std::string quoteIfNeeded(const std::string& path)
{
    if (path.find_first_of(" \t\"") == std::string::npos)
        return path;
    std::string out = "\"";
    for (char c : path) {
        if (c == '"')
            out += "\\\"";
        else
            out += c;
    }
    out += '"';
    return out;
}

// Script paths are always quoted: they can contain spaces, & or parentheses
// and every consumer (cmd, bash, wt) takes quoted paths.
inline std::string quoteAlways(const std::string& path)
{
    return "\"" + path + "\"";
}

// ---- admin execution (shell-admin-design.md 4) --------------------------------------

// Whether the shell's program can be driven through the elevated wrapper: only
// the trusted system shells, matched by basename. bash/msys/wsl and any other
// program get no admin entry.
inline bool adminCapable(const std::string& program)
{
    const std::size_t slash = program.find_last_of("/\\");
    const std::string file = slash == std::string::npos ? program : program.substr(slash + 1);
    const std::string exe = lower(file);
    return exe == "cmd.exe" || exe == "pwsh.exe" || exe == "powershell.exe";
}

// FNV-1a 64. Not cryptographic: it detects the script being rewritten in the
// window between pressing Enter and clicking the UAC dialog
// (shell-admin-design.md 7 R7) and shrinks that window to milliseconds.
inline unsigned long long fnv1a64(const std::string& s)
{
    unsigned long long h = 0xcbf29ce484222325ULL;
    for (const char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

// The parameters of the elevated cmd.exe. keepOpen=/k leaves the console open
// after the command (the direct form: the window hosts the payload itself);
// /c is the two-stage form: the elevated cmd launches wt and exits right
// after, and the WT window owns the payload. The consent object is the system
// cmd either way. /S strips the outermost quote pair only, so the inner command
// line keeps its own quotes. No < NUL and no redirection: the window has real
// stdin (interactive commands work) and shows the output itself, so nothing
// depends on the plugin outliving the run.
inline std::string buildAdminWindowParams(const std::string& innerCommandLine, bool keepOpen)
{
    return keepOpen ? "/d /s /k \"" + innerCommandLine + "\""
                    : "/d /s /c \"" + innerCommandLine + "\"";
}

// wt splits its command line on `;` even inside quotes, so every semicolon
// (a `;` in a path, an `exec bash -l` handover) must be escaped as `\;` or wt
// launches the tail as a second command and fails with 0x80070002.
inline std::string wtEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == ';')
            out += "\\;";
        else
            out += c;
    }
    return out;
}

// The accepted values of the adminMode setting; anything else falls back to
// ask (the setting hint names only these two).
inline std::string adminModeOrDefault(const std::string& value)
{
    return value == "ask" || value == "off" ? value : "ask";
}

// The accepted values of the adminTerminal setting (which host the elevated
// window uses): auto follows `terminal` (wt → two-stage into WT), wt always
// tries the two-stage form, conhost always opens the elevated cmd's own
// console. Anything else falls back to auto.
inline std::string adminTerminalOrDefault(const std::string& value)
{
    return value == "auto" || value == "wt" || value == "conhost" ? value : "auto";
}

// The command line for a custom shell: {script} → scriptPath, {wslscript} →
// wslScriptPath; no placeholder → the script path appended last. Substituted
// paths are quoted when they contain spaces.
inline std::string customShellCommandLine(const CustomShell& sh, const std::string& scriptPath,
                                          const std::string& wslScriptPath)
{
    const std::string quotedScript = quoteAlways(scriptPath);
    const std::string quotedWsl = quoteAlways(wslScriptPath);
    std::string out;
    std::size_t from = 0;
    bool placeholder = false;
    const std::string& args = sh.args;
    for (;;) {
        const std::size_t s = args.find("{script}", from);
        const std::size_t w = args.find("{wslscript}", from);
        if (s == std::string::npos && w == std::string::npos) {
            out += args.substr(from);
            break;
        }
        const bool scriptFirst = s != std::string::npos && (w == std::string::npos || s < w);
        const std::size_t at = scriptFirst ? s : w;
        out += args.substr(from, at - from);
        out += scriptFirst ? quotedScript : quotedWsl;
        placeholder = true;
        from = at + (scriptFirst ? 8 : 11);
    }
    if (!placeholder)
        out += (out.empty() ? std::string() : " ") + (sh.wsl ? quotedWsl : quotedScript);
    return quoteIfNeeded(sh.program) + (out.empty() ? std::string() : " ") + out;
}

// ---- script generation (design 4.2) ---------------------------------------------

struct ScriptSpec {
    std::string ext;    // ".cmd" | ".ps1" | ".sh" | ".txt"…
    bool bom = false;   // UTF-8 BOM (Windows PowerShell 5.1 needs it)
    bool crlf = false;  // CRLF line endings (cmd and PowerShell want them)
    std::string content;
};

inline ScriptSpec scriptFor(const std::string& shellName, const std::string& command)
{
    ScriptSpec spec;
    const std::string name = lower(shellName);
    if (name == kCmd) {
        spec.ext = ".cmd";
        spec.crlf = true;
        // Keep the user's bytes untouched; @echo off just stops the echo of
        // every line, chcp 65001 makes cmd emit UTF-8.
        spec.content = "@echo off\r\nchcp 65001>nul\r\n" + normalizeLines(command, "\r\n") + "\r\n";
    } else if (name == kPwsh || name == kPowerShell) {
        spec.ext = ".ps1";
        spec.crlf = true;
        spec.bom = name == kPowerShell; // 5.1 reads a BOM-less file as ANSI
        const std::string header = "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)\n"
                                   "$ErrorActionPreference='Continue'\n";
        // The whole file is CRLF: the header's \n are folded by normalizeLines.
        spec.content = normalizeLines(header + command + "\n", "\r\n");
    } else {
        // bash, msys, wsl and custom shells default to LF, no BOM. Clipboard
        // content with CRLF is normalized too, or bash reports $'\r'.
        spec.ext = ".sh";
        spec.content = normalizeLines(command, "\n") + "\n";
    }
    return spec;
}

inline std::string scriptBytes(const ScriptSpec& spec)
{
    const std::string bom = spec.bom ? "\xEF\xBB\xBF" : "";
    return bom + spec.content;
}

// ---- WSL paths (design 4.1 / 10.1) ----------------------------------------------

// C:\a b\c.sh → /c/a b/c.sh (the form Git Bash/MSYS2 see their own files by).
// UNC paths (\\server\share\…) have no such form: nullopt.
inline std::optional<std::string> toMsysPath(const std::string& windowsPath)
{
    if (windowsPath.size() < 2 || windowsPath[0] == '\\' || windowsPath[0] == '/')
        return std::nullopt;
    const char drive = static_cast<char>(std::tolower(static_cast<unsigned char>(windowsPath[0])));
    if (drive < 'a' || drive > 'z' || windowsPath[1] != ':')
        return std::nullopt;
    std::string out = "/";
    out += drive;
    for (std::size_t i = 2; i < windowsPath.size(); ++i) {
        const char c = windowsPath[i];
        out += c == '\\' ? '/' : c;
    }
    return out;
}

// C:\a b\c.sh → /mnt/c/a b/c.sh. UNC paths: nullopt.
inline std::optional<std::string> toWslPath(const std::string& windowsPath)
{
    const std::optional<std::string> msys = toMsysPath(windowsPath);
    if (!msys)
        return std::nullopt;
    return "/mnt" + *msys;
}

// ---- output decoding (design 4.3 / 10.1) ----------------------------------------

// A well-formed UTF-8 string (no truncated sequence, no overlong, no surrogate).
inline bool wellFormedUtf8(const std::string& s)
{
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char lead = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;
        unsigned char low = 0x80, high = 0xBF;
        if (lead < 0x80) {
            ++i;
            continue;
        } else if (lead >= 0xC2 && lead <= 0xDF)
            n = 2;
        else if (lead >= 0xE0 && lead <= 0xEF) {
            n = 3;
            if (lead == 0xE0)
                low = 0xA0;
            else if (lead == 0xED)
                high = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            n = 4;
            if (lead == 0xF0)
                low = 0x90;
            else if (lead == 0xF4)
                high = 0x8F;
        } else
            return false;
        if (i + n > s.size())
            return false;
        if (static_cast<unsigned char>(s[i + 1]) < low || static_cast<unsigned char>(s[i + 1]) > high)
            return false;
        for (std::size_t k = 2; k < n; ++k) {
            const unsigned char c = static_cast<unsigned char>(s[i + k]);
            if (c < 0x80 || c > 0xBF)
                return false;
        }
        i += n;
    }
    return true;
}

inline void appendUtf8(std::string& out, unsigned cp)
{
    if (cp < 0x80)
        out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// UTF-16LE bytes → UTF-8 (surrogate pairs handled, lone surrogates → U+FFFD).
inline std::string utf16leToUtf8(const std::string& bytes)
{
    std::string out;
    out.reserve(bytes.size());
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const unsigned unit = static_cast<unsigned char>(bytes[i])
                              | (static_cast<unsigned char>(bytes[i + 1]) << 8);
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 3 < bytes.size()) {
                const unsigned low = static_cast<unsigned char>(bytes[i + 2])
                                     | (static_cast<unsigned char>(bytes[i + 3]) << 8);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    appendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                    i += 2;
                    continue;
                }
            }
            appendUtf8(out, 0xFFFD);
        } else if (unit >= 0xDC00 && unit <= 0xDFFF)
            appendUtf8(out, 0xFFFD);
        else
            appendUtf8(out, unit);
    }
    return out;
}

// wsl.exe reports its own errors in UTF-16LE ("xx 00" pairs, or a FF FE BOM).
inline bool looksUtf16le(const std::string& bytes)
{
    if (bytes.size() < 4)
        return false;
    if (static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE)
        return true;
    const std::size_t pairs = std::min<std::size_t>(bytes.size() / 2, 8);
    int ascii = 0;
    for (std::size_t i = 0; i < pairs; ++i) {
        const unsigned char lo = static_cast<unsigned char>(bytes[i * 2]);
        const unsigned char hi = static_cast<unsigned char>(bytes[i * 2 + 1]);
        if (hi == 0 && lo >= 0x20 && lo < 0x7F)
            ++ascii;
        else if (hi != 0)
            return false;
    }
    return pairs > 0 && ascii == static_cast<int>(pairs);
}

// Split a growing read buffer into complete lines: everything through the last
// '\n' is decodable now; the tail waits for the next read (design 10.1: "a line
// cut between two reads").
struct LineSplit {
    std::string complete; // through the last '\n' (inclusive), maybe empty
    std::string rest;     // after it, maybe empty
};
inline LineSplit splitCompleteLines(const std::string& buffer)
{
    const std::size_t nl = buffer.rfind('\n');
    if (nl == std::string::npos)
        return {{}, buffer};
    return {buffer.substr(0, nl + 1), buffer.substr(nl + 1)};
}

// Decode one complete chunk: per line, UTF-8 when well-formed, else the OEM
// code page (the `oem` callback, provided by the Win32 side). A UTF-16LE chunk
// (wsl.exe's own errors) decodes whole.
inline std::string decodeChunk(const std::string& bytes,
                               const std::function<std::string(const std::string&)>& oem)
{
    if (looksUtf16le(bytes))
        return utf16leToUtf8(bytes);
    std::string out;
    out.reserve(bytes.size());
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = bytes.find('\n', start);
        const std::size_t end = nl == std::string::npos ? bytes.size() : nl + 1;
        const std::string line = bytes.substr(start, end - start);
        if (wellFormedUtf8(line))
            out += line;
        else if (oem)
            out += oem(line);
        else
            out += line;
        if (nl == std::string::npos)
            break;
        start = end;
    }
    return out;
}

// ANSI escape sequences off (CSI … final byte, OSC … BEL/ST, and lone ESCs).
inline std::string stripAnsi(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] != '\x1B') {
            out += s[i];
            ++i;
            continue;
        }
        if (i + 1 < s.size() && s[i + 1] == '[') {
            // CSI: parameter bytes 0x30–0x3F, intermediates 0x20–0x2F, final 0x40–0x7E.
            std::size_t j = i + 2;
            while (j < s.size() && static_cast<unsigned char>(s[j]) >= 0x20
                   && static_cast<unsigned char>(s[j]) <= 0x3F)
                ++j;
            if (j < s.size() && static_cast<unsigned char>(s[j]) >= 0x40
                && static_cast<unsigned char>(s[j]) <= 0x7E) {
                i = j + 1;
                continue;
            }
            ++i; // malformed: drop the ESC only
            continue;
        }
        if (i + 1 < s.size() && s[i + 1] == ']') {
            // OSC: up to BEL or ESC \.
            std::size_t j = i + 2;
            while (j < s.size()) {
                if (s[j] == '\x07') {
                    j += 1;
                    break;
                }
                if (s[j] == '\x1B' && j + 1 < s.size() && s[j + 1] == '\\') {
                    j += 2;
                    break;
                }
                ++j;
            }
            i = j;
            continue;
        }
        ++i; // lone ESC or two-byte sequence we do not model: drop the ESC
    }
    return out;
}

// '\r'-overwritten progress: per line keep what the last carriage return left
// ("33%\r66%\r100%" → "100%"). One trailing '\r' (of a CRLF) is dropped first.
inline std::string collapseCarriageReturns(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = s.find('\n', start);
        const std::size_t end = nl == std::string::npos ? s.size() : nl + 1;
        std::string line = s.substr(start, end - start);
        if (!line.empty() && line.back() == '\n')
            line.pop_back();
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::size_t cr = line.rfind('\r');
        out += cr == std::string::npos ? line : line.substr(cr + 1);
        if (nl == std::string::npos)
            break;
        out += '\n';
        start = end;
    }
    return out;
}

// A display/storage cap for one output line. The full line still goes to the
// log; this bounds the ring buffer (the manifest's memoryMB) and any single
// JSON row (the host kills a plugin whose stdout line passes 16 MB). Cuts on a
// UTF-8 boundary so a CJK character is never split, and marks the cut so the
// reader knows the log has the rest.
inline std::string truncateLine(const std::string& s, std::size_t maxBytes)
{
    if (s.size() <= maxBytes)
        return s;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
        --cut; // back off continuation bytes to the boundary code point's lead
    return s.substr(0, cut) + "… [line truncated; full output in the log]";
}

// A UTF-8 code point at s[i]; false on malformed input.
inline bool codePointAt(const std::string& s, std::size_t i, unsigned& cp, std::size_t& len)
{
    const unsigned char lead = static_cast<unsigned char>(s[i]);
    if (lead < 0x80) {
        cp = lead;
        len = 1;
        return true;
    }
    std::size_t n = 0;
    unsigned char low = 0x80, high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF)
        n = 2;
    else if (lead >= 0xE0 && lead <= 0xEF) {
        n = 3;
        if (lead == 0xE0)
            low = 0xA0;
        else if (lead == 0xED)
            high = 0x9F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        n = 4;
        if (lead == 0xF0)
            low = 0x90;
        else if (lead == 0xF4)
            high = 0x8F;
    } else
        return false;
    if (i + n > s.size())
        return false;
    if (static_cast<unsigned char>(s[i + 1]) < low || static_cast<unsigned char>(s[i + 1]) > high)
        return false;
    for (std::size_t k = 2; k < n; ++k) {
        const unsigned char c = static_cast<unsigned char>(s[i + k]);
        if (c < 0x80 || c > 0xBF)
            return false;
    }
    cp = lead & (n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07);
    for (std::size_t k = 1; k < n; ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    len = n;
    return true;
}

inline bool isInvisibleChar(unsigned cp)
{
    // Zero-width and word-joiner, bidi controls, BOM.
    return (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || cp == 0x2060
           || cp == 0xFEFF || (cp < 0x20 && cp != '\t' && cp != '\n' && cp != '\r')
           || (cp >= 0x7F && cp < 0xA0);
}

// Invisible characters become <U+202E> so a spoofed command is visible in the
// confirmation page (design 5.3). Also escapes '\t' so rows stay one line.
inline std::string showInvisibles(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned cp = 0;
        std::size_t len = 0;
        if (!codePointAt(s, i, cp, len)) {
            out += s[i]; // keep broken bytes; decodeChunk already fixed encoding
            ++i;
            continue;
        }
        if (cp == '\t') {
            out += "    ";
            ++i;
            continue;
        }
        if (isInvisibleChar(cp)) {
            char buffer[16];
            std::snprintf(buffer, sizeof buffer, "<U+%04X>", cp);
            out += buffer;
        } else
            out.append(s, i, len);
        i += len;
    }
    return out;
}

inline bool hasInvisibleChars(const std::string& s)
{
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned cp = 0;
        std::size_t len = 0;
        if (!codePointAt(s, i, cp, len)) {
            ++i;
            continue;
        }
        if (isInvisibleChar(cp) && cp != '\t' && cp != '\n' && cp != '\r')
            return true;
        i += len;
    }
    return false;
}

// ---- danger rules (design 5.3) ---------------------------------------------------

// Stable rule ids, localized by the caller:
//   download  - piped into an interpreter after a download (iex / | sh / curl … |)
//   destructive - deletes trees, formats drives
//   system    - changes the boot or registry configuration
//   invisible - invisible or direction-changing characters
inline std::vector<std::string> dangerRules(const std::string& command)
{
    std::vector<std::string> hits;
    const std::string c = lower(command);
    const bool interpreterPipe = hasCi(command, "| sh") || hasCi(command, "| bash") || hasCi(command, "|sh")
                                 || hasCi(command, "|bash");
    const bool iex = hasCi(command, "iex ") || hasCi(command, "iex;") || hasCi(command, "iex\n")
                     || hasCi(command, "invoke-expression");
    const bool curlPipe = (hasCi(command, "curl ") || hasCi(command, "iwr ")
                           || hasCi(command, "invoke-webrequest"))
                          && command.find('|') != std::string::npos;
    if (interpreterPipe || iex || curlPipe)
        hits.push_back("download");
    if (hasCi(command, "rm -rf") || hasCi(command, "rm -fr") || hasCi(command, "remove-item -recurse")
        || hasCi(command, "del /s") || hasCi(command, "rd /s") || hasCi(command, "rmdir /s")
        || hasCi(command, "format ") || hasCi(command, "diskpart"))
        hits.push_back("destructive");
    if (hasCi(command, "reg delete") || hasCi(command, "bcdedit") || hasCi(command, "set-executionpolicy"))
        hits.push_back("system");
    if (hasInvisibleChars(command))
        hits.push_back("invisible");
    return hits;
}

// ---- clipboard confirmation (design 5.3) -----------------------------------------

// always: confirm everything; multiline: only multi-line or dangerous text;
// never: run right away. `multiline` means the content spans several lines.
inline bool shouldConfirm(const std::string& mode, bool multiline, bool dangerous)
{
    if (mode == "never")
        return false;
    if (mode == "multiline")
        return multiline || dangerous;
    return true; // "always" and anything unrecognized
}

// The confirmation page shows at most 20 content lines, then "还有 N 行".
struct ConfirmLines {
    std::vector<std::string> lines; // at most `maxLines` (invisible chars shown)
    std::size_t total = 0;
    std::size_t hidden = 0;
};
inline ConfirmLines confirmPageLines(const std::string& text, std::size_t maxLines = 20)
{
    ConfirmLines out;
    std::size_t start = 0;
    bool first = true;
    for (;;) {
        const std::size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        while (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (nl == std::string::npos && line.empty() && !first)
            break; // no content after the last newline
        first = false;
        ++out.total;
        if (out.lines.size() < maxLines)
            out.lines.push_back(showInvisibles(line));
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    out.hidden = out.total > out.lines.size() ? out.total - out.lines.size() : 0;
    return out;
}

// ---- history (design 7.1) --------------------------------------------------------

struct HistoryEntry {
    std::string shell;    // stable shell name
    std::string command;  // the whole command text
    long long lastRun = 0;   // Unix seconds of the latest run
    long long count = 1;
    long long lastExit = 0;
};

constexpr std::size_t kMaxHistoryEntries = 200;
constexpr std::size_t kMaxCommandChars = 8000;
// The history file is untrusted input: wrong types, oversized commands and
// non-object entries are dropped; a wholesale parse failure marks the whole
// file bad and is handled by the caller (rename to .bad). Each row is the
// already-unpacked fields of one history object: [shell, command, lastRun,
// count, lastExit].
struct HistoryBlob {
    bool isObject = false;
    bool isNumber = false;
    bool isString = false;
    long long number = 0;
    std::string text;
};
inline std::vector<HistoryEntry> historyFromBlobs(const std::vector<std::vector<HistoryBlob>>& rows)
{
    std::vector<HistoryEntry> out;
    for (const auto& row : rows) {
        if (row.size() != 5)
            continue;
        if (!row[0].isString || !row[1].isString || !row[2].isNumber || !row[3].isNumber
            || !row[4].isNumber)
            continue;
        HistoryEntry e;
        e.shell = row[0].text;
        e.command = row[1].text;
        e.lastRun = row[2].number;
        e.count = row[3].number;
        e.lastExit = row[4].number;
        if (e.shell.empty() || e.shell.size() > 64)
            continue;
        if (e.command.empty() || e.command.size() > kMaxCommandChars)
            continue;
        if (e.count <= 0)
            e.count = 1;
        out.push_back(std::move(e));
    }
    return out;
}

// Merge a run into the history: same shell + same command become one entry,
// newest first, capped at kMaxHistoryEntries.
inline std::vector<HistoryEntry> historyMerged(std::vector<HistoryEntry> history, const HistoryEntry& run)
{
    if (run.command.empty() || run.command.size() > kMaxCommandChars)
        return history;
    for (auto& e : history) {
        if (e.shell == run.shell && e.command == run.command) {
            ++e.count;
            e.lastRun = run.lastRun;
            e.lastExit = run.lastExit;
            std::stable_sort(history.begin(), history.end(),
                             [](const HistoryEntry& a, const HistoryEntry& b) { return a.lastRun > b.lastRun; });
            return history;
        }
    }
    history.push_back(run);
    std::stable_sort(history.begin(), history.end(),
                     [](const HistoryEntry& a, const HistoryEntry& b) { return a.lastRun > b.lastRun; });
    if (history.size() > kMaxHistoryEntries)
        history.resize(kMaxHistoryEntries);
    return history;
}

inline std::vector<HistoryEntry> historyWithout(const std::vector<HistoryEntry>& history,
                                                const std::string& shell, const std::string& command)
{
    std::vector<HistoryEntry> out;
    for (const HistoryEntry& e : history)
        if (!(e.shell == shell && e.command == command))
            out.push_back(e);
    return out;
}

// History rows matching the query text (substring, newest first already).
inline std::vector<HistoryEntry> historyMatching(const std::vector<HistoryEntry>& history,
                                                 const std::string& text, std::size_t limit = 8)
{
    std::vector<HistoryEntry> out;
    if (text.empty())
        return history.size() <= limit ? history : std::vector<HistoryEntry>(history.begin(), history.begin() + limit);
    for (const HistoryEntry& e : history) {
        if (e.command.find(text) != std::string::npos) {
            out.push_back(e);
            if (out.size() >= limit)
                break;
        }
    }
    return out;
}

// ---- runs folder cleanup (design 7.1) --------------------------------------------

// Keep the min(keep, withinDays) newest files; everything else goes. `now`
// and `mtime` are Unix seconds.
inline std::vector<std::string> expiredRunFiles(const std::vector<std::pair<std::string, long long>>& files,
                                                long long now, std::size_t keep = 50, long long days = 7)
{
    std::vector<std::pair<std::string, long long>> sorted = files;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    std::size_t within = 0;
    for (const auto& f : sorted)
        if (f.second >= now - days * 86400)
            ++within;
    const std::size_t keepCount = std::min(keep, within);
    std::vector<std::string> out;
    for (std::size_t i = keepCount; i < sorted.size(); ++i)
        out.push_back(sorted[i].first);
    return out;
}

// ---- run ids ---------------------------------------------------------------------

// A short, readable id for `#r<id>`: epoch seconds mod 10^8 times 100 plus a
// per-process counter; unique within a day per plugin instance.
inline long long newRunId(long long epochSeconds, long long counter)
{
    const long long base = (epochSeconds % 100000000LL) * 100LL;
    return base + (counter % 100LL);
}

} // namespace shellplug
