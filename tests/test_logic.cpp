// test_logic.cpp - the pure rules in logic.h, no process or file dependency.
// Follows the ra9fael.binarylab test pattern: a CHECK macro, one main.
#include "../logic.h"

#include <cstdio>
#include <string>

int g_total = 0;
int g_failed = 0;

#define CHECK(condition, label)                                                \
    do {                                                                       \
        ++g_total;                                                             \
        if (!(condition)) {                                                    \
            ++g_failed;                                                        \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, label);        \
        }                                                                      \
    } while (0)

using namespace shellplug;

// ---- groups ---------------------------------------------------------------------

static void testParseInput()
{
    const auto empty = parseInput("");
    CHECK(empty.kind == ParsedInput::Kind::Empty, "empty input");
    const auto cmd = parseInput("ls -la");
    CHECK(cmd.kind == ParsedInput::Kind::Command && cmd.command == "ls -la", "plain command");
    const auto req = parseInput("@nu ls -la");
    CHECK(req.kind == ParsedInput::Kind::ShellRequest && req.shellName == "nu"
              && req.command == "ls -la",
          "shell request with words");
    const auto reqBare = parseInput("@nu");
    CHECK(reqBare.kind == ParsedInput::Kind::ShellRequest && reqBare.command.empty(),
          "shell request without words");
    const auto list = parseInput("@");
    CHECK(list.kind == ParsedInput::Kind::ShellList, "@ alone lists shells");
    const auto notShell = parseInput(" @x");
    CHECK(notShell.kind == ParsedInput::Kind::Command && notShell.command == " @x",
          "leading space: @x is part of the command");
    const auto latest = parseInput("#");
    CHECK(latest.kind == ParsedInput::Kind::ResultPage && latest.runId == 0, "# is the latest run");
    const auto page = parseInput("#r42");
    CHECK(page.kind == ParsedInput::Kind::ResultPage && page.runId == 42, "#r42 opens run 42");
    CHECK(parseInput("#r").kind == ParsedInput::Kind::Command, "#r alone is a command");
    CHECK(parseInput("#rx").kind == ParsedInput::Kind::Command, "#rx is a command");
    CHECK(parseInput("# comment").kind == ParsedInput::Kind::Command,
          "# comment stays a command (bash)");
    CHECK(parseInput("#r42x").kind == ParsedInput::Kind::Command, "#r42x is a command");
}

static void testCustomShells()
{
    const std::vector<std::string> reserved = builtinShellNames();
    const auto two = parseCustomShells(
        "ucrt=C:\\msys64\\usr\\bin\\bash.exe -l {script}|py=C:\\Python\\python.exe -X utf8 {script}",
        reserved);
    CHECK(two.problems.empty() && two.shells.size() == 2, "two valid entries");
    CHECK(two.shells[0].name == "ucrt" && two.shells[0].args == "-l {script}", "first entry fields");
    CHECK(two.shells[1].ext == ".txt", "default extension");

    const auto noEq = parseCustomShells("ucrt bash.exe", reserved);
    CHECK(noEq.problems.size() == 1 && noEq.problems[0].code == CustomShellProblem::Code::BadFormat
              && noEq.problems[0].index == 1,
          "missing = is a format problem");

    const auto dup = parseCustomShells("a=C:\\x.exe|a=C:\\y.exe", reserved);
    CHECK(dup.problems.size() == 1 && dup.problems[0].code == CustomShellProblem::Code::Duplicate
              && dup.problems[0].index == 2,
          "duplicate name reported at the second entry");

    const auto reservedName = parseCustomShells("cmd=C:\\x.exe", reserved);
    CHECK(reservedName.problems.size() == 1
              && reservedName.problems[0].code == CustomShellProblem::Code::Reserved,
          "built-in names are reserved");

    const auto badName = parseCustomShells("a b=C:\\x.exe", reserved);
    CHECK(badName.problems.size() == 1 && badName.problems[0].code == CustomShellProblem::Code::BadName,
          "a name with a space is invalid");

    const auto noScript = parseCustomShells("a=C:\\x.exe", reserved);
    CHECK(noScript.problems.empty() && noScript.shells.size() == 1,
          "no {script}: the path is appended");
    CHECK(customShellCommandLine(noScript.shells[0], "C:\\r\\1.sh", "/mnt/c/r/1.sh")
              == "C:\\x.exe \"C:\\r\\1.sh\"",
          "appended script path, quoted");

    const auto quoted = parseCustomShells("a=\"C:\\My Tools\\nu.exe\" -x {script}", reserved);
    CHECK(quoted.problems.empty() && quoted.shells[0].program == "C:\\My Tools\\nu.exe"
              && quoted.shells[0].args == "-x {script}",
          "quoted program with spaces");
    CHECK(customShellCommandLine(quoted.shells[0], "C:\\r\\1.sh", "")
              == "\"C:\\My Tools\\nu.exe\" -x \"C:\\r\\1.sh\"",
          "quoted substitution");

    const auto spaced = parseCustomShells("a=C:\\r 1\\nu.exe {script}", reserved);
    CHECK(spaced.problems.empty() && spaced.shells[0].program == "C:\\r", 
          "unquoted path: the first token is the program");

    const auto ext = parseCustomShells("a=C:\\x.exe {script};ext=.ps1", reserved);
    CHECK(ext.shells[0].ext == ".ps1", ";ext= sets the extension");
    const auto extBad = parseCustomShells("a=C:\\x.exe {script};ext=noDot", reserved);
    CHECK(extBad.shells[0].ext == ".txt", "an extension without a dot is ignored");

    const auto wsl = parseCustomShells("w=C:\\wsl\\bash.exe {wslscript}", reserved);
    CHECK(wsl.shells[0].wsl, "{wslscript} marks a WSL shell");
    CHECK(customShellCommandLine(wsl.shells[0], "C:\\r\\1.sh", "/mnt/c/r/1.sh")
              == "C:\\wsl\\bash.exe \"/mnt/c/r/1.sh\"",
          "wsl path substituted, quoted");

    const auto extFromExe = parseCustomShells("z=C:\\msys64\\usr\\bin\\bash.exe", reserved);
    CHECK(extFromExe.shells[0].ext == ".sh", "bash.exe implies .sh");
}

static void testScriptGeneration()
{
    const ScriptSpec cmd = scriptFor(kCmd, "echo hi");
    CHECK(cmd.ext == ".cmd" && cmd.crlf && !cmd.bom, "cmd script spec");
    CHECK(startsWith(cmd.content, "@echo off\r\nchcp 65001>nul\r\n"), "cmd script header");
    CHECK(cmd.content.find("echo hi\r\n") != std::string::npos, "cmd body with CRLF");

    const ScriptSpec pwsh = scriptFor(kPwsh, "Get-ChildItem");
    CHECK(pwsh.ext == ".ps1" && pwsh.crlf && !pwsh.bom, "pwsh script spec");
    CHECK(pwsh.content.find("[Console]::OutputEncoding") != std::string::npos, "pwsh header");

    const ScriptSpec ps51 = scriptFor(kPowerShell, "dir");
    CHECK(ps51.bom, "Windows PowerShell 5.1 wants a BOM");
    const std::string bytes = scriptBytes(ps51);
    CHECK(bytes.size() > 3 && static_cast<unsigned char>(bytes[0]) == 0xEF
              && static_cast<unsigned char>(bytes[1]) == 0xBB
              && static_cast<unsigned char>(bytes[2]) == 0xBF,
          "the BOM bytes are there");

    const ScriptSpec bash = scriptFor(kBash, "ls\r\npwd");
    CHECK(bash.ext == ".sh" && !bash.crlf && !bash.bom, "bash script spec");
    CHECK(bash.content == "ls\npwd\n", "bash script: CRLF normalized to LF, one trailing \\n");
    const ScriptSpec msys = scriptFor(kMsys, "uname");
    CHECK(msys.ext == ".sh", "msys is a bash script too");
}

static void testWslPaths()
{
    CHECK(toWslPath("C:\\a b\\c.sh").value_or("") == "/mnt/c/a b/c.sh", "drive path to /mnt");
    CHECK(toMsysPath("C:\\a b\\c.sh").value_or("") == "/c/a b/c.sh", "drive path to /c");
    CHECK(!toWslPath("\\\\server\\share\\x").has_value(), "UNC has no /mnt form");
    CHECK(!toMsysPath("relative/x").has_value(), "a relative path is not a drive path");
    CHECK(toWslPath("D:").value_or("") == "/mnt/d", "bare drive letter");
}

static void testDecode()
{
    CHECK(wellFormedUtf8("abc"), "ascii is utf-8");
    CHECK(wellFormedUtf8("中文"), "utf-8 chinese");
    CHECK(!wellFormedUtf8("\xE4\xB8"), "a truncated sequence is not utf-8");
    CHECK(!wellFormedUtf8("\xFF"), "0xFF is not utf-8");
    CHECK(!wellFormedUtf8("\xC0\x80"), "an overlong encoding is not utf-8");
    CHECK(!wellFormedUtf8("\xED\xA0\x80"), "a surrogate is not utf-8");

    const std::string utf16 = std::string("a\0b\0c\0", 6);
    CHECK(looksUtf16le(utf16), "ascii utf-16le pattern");
    CHECK(!looksUtf16le("abc\n"), "utf-8 is not utf-16");
    CHECK(utf16leToUtf8(utf16) == "abc", "utf-16le decodes");
    // 中 = U+4E2D → LE bytes 2D 4E; a surrogate pair (U+1F600 → 3D D8 00 DE).
    CHECK(utf16leToUtf8(std::string("\x2D\x4E", 2)) == "中", "utf-16le chinese");
    CHECK(utf16leToUtf8(std::string("\x3D\xD8\x00\xDE", 4)) == "\xF0\x9F\x98\x80",
          "surrogate pair to utf-8");

    const LineSplit split = splitCompleteLines("one\ntwo");
    CHECK(split.complete == "one\n" && split.rest == "two", "a line cut between reads waits");
    const LineSplit all = splitCompleteLines("one\ntwo\n");
    CHECK(all.complete == "one\ntwo\n" && all.rest.empty(), "complete buffer");

    const auto oem = [](const std::string& s) { return "<oem>" + s; };
    CHECK(decodeChunk("ok\n", oem) == "ok\n", "utf-8 lines pass through");
    CHECK(decodeChunk("\xE4\xB8\n", oem) == "<oem>\xE4\xB8\n", "a bad line goes to the OEM page");

    CHECK(stripAnsi("\x1B[31mred\x1B[0m") == "red", "CSI colors stripped");
    CHECK(stripAnsi("\x1B]0;title\x07x") == "x", "OSC title stripped");
    CHECK(stripAnsi("a\x1B" "b") == "ab", "a lone ESC goes away");

    CHECK(collapseCarriageReturns("33%\r66%\r100%\n") == "100%\n", "progress keeps the last");
    CHECK(collapseCarriageReturns("text\r\n") == "text\n", "CRLF survives collapse");
    CHECK(collapseCarriageReturns("a\r\nb\rc\n") == "a\nc\n",
          "an overwriting \\r keeps the tail, a line-ending \\r drops");

    CHECK(showInvisibles("a\u202Eb") == "a<U+202E>b", "bidi char shown");
    CHECK(showInvisibles("a\tb") == "a    b", "tab widened");
    CHECK(hasInvisibleChars("a\u200Bb"), "zero width detected");
    CHECK(!hasInvisibleChars("普通文本"), "plain text clean");
}

static void testDangerRules()
{
    auto has = [](const std::string& cmd, const std::string& rule) {
        const std::vector<std::string> hits = dangerRules(cmd);
        for (const std::string& h : hits)
            if (h == rule)
                return true;
        return false;
    };
    CHECK(has("iex (irm http://x)", "download"), "iex flagged");
    CHECK(has("Invoke-Expression $x", "download"), "Invoke-Expression flagged");
    CHECK(has("curl http://x | sh", "download"), "curl piped to sh flagged");
    CHECK(has("cat x |bash", "download"), "piped to bash flagged");
    CHECK(!has("curl -O http://x/f.zip", "download"), "plain curl not flagged");
    CHECK(!has("ls -la", "download"), "ls clean");
    CHECK(has("rm -rf /tmp/x", "destructive"), "rm -rf flagged");
    CHECK(has("Remove-Item -Recurse C:\\x", "destructive"), "Remove-Item -Recurse flagged");
    CHECK(has("reg delete HKLM\\x", "system"), "reg delete flagged");
    CHECK(has("bcdedit /set x", "system"), "bcdedit flagged");
    CHECK(has("Set-ExecutionPolicy RemoteSigned", "system"), "execution policy flagged");
    CHECK(!has("reg query HKLM", "system"), "reg query clean");
    CHECK(has("echo a\u202Eb", "invisible"), "invisible chars flagged");
    CHECK(dangerRules("echo hi").empty(), "plain echo clean");
}

static void testConfirm()
{
    CHECK(shouldConfirm("always", false, false), "always confirms everything");
    CHECK(!shouldConfirm("never", true, true), "never confirms nothing");
    CHECK(shouldConfirm("multiline", true, false), "multiline mode wants multiline");
    CHECK(shouldConfirm("multiline", false, true), "multiline mode wants danger");
    CHECK(!shouldConfirm("multiline", false, false), "multiline mode skips a short clean text");

    std::string text;
    for (int i = 0; i < 25; ++i)
        text += "line " + std::to_string(i) + "\n";
    const ConfirmLines page = confirmPageLines(text);
    CHECK(page.lines.size() == 20 && page.total == 25 && page.hidden == 5, "25 lines: 20 shown, 5 hidden");
    const ConfirmLines small = confirmPageLines("one\ntwo");
    CHECK(small.lines.size() == 2 && small.hidden == 0, "short text fully shown");
    const ConfirmLines single = confirmPageLines("only");
    CHECK(single.lines.size() == 1 && single.total == 1, "a single line without \\n");
}

static shellplug::HistoryBlob str(const std::string& s)
{
    shellplug::HistoryBlob b;
    b.isObject = true;
    b.isString = true;
    b.text = s;
    return b;
}
static shellplug::HistoryBlob num(long long v)
{
    shellplug::HistoryBlob b;
    b.isObject = true;
    b.isNumber = true;
    b.number = v;
    return b;
}

static void testHistory()
{
    const std::vector<std::vector<HistoryBlob>> rows = {
        {str("bash"), str("ls -la"), num(100), num(3), num(0)},
        {str("pwsh"), str("bad"), num(0), num(1), num(0), num(1)},  // 6 fields: dropped
        {str(""), str("x"), num(1), num(1), num(0)},                // empty shell: dropped
        {num(1), str("x"), num(1), num(1), num(0)},                 // shell not a string: dropped
        {str("cmd"), str(""), num(1), num(1), num(0)},              // empty command: dropped
        {str("cmd"), str(std::string(kMaxCommandChars + 1, 'x')), num(1), num(1), num(0)}, // oversized: dropped
    };
    const std::vector<HistoryEntry> entries = historyFromBlobs(rows);
    CHECK(entries.size() == 1 && entries[0].shell == "bash" && entries[0].count == 3,
          "bad rows dropped, good kept");

    std::vector<HistoryEntry> history;
    history = historyMerged(history, {"bash", "ls", 100, 1, 0});
    history = historyMerged(history, {"pwsh", "dir", 200, 1, 0});
    history = historyMerged(history, {"bash", "ls", 300, 1, 2});
    CHECK(history.size() == 2, "same shell + command merge");
    CHECK(history[0].command == "ls" && history[0].count == 2 && history[0].lastRun == 300
              && history[0].lastExit == 2,
          "the newest run updated the entry, sorted first");
    history = historyMerged(history, {"bash", "small", 50, 1, 0});
    CHECK(history[0].command == "ls" && history[2].command == "small", "sorted by lastRun");

    std::vector<HistoryEntry> big;
    for (std::size_t i = 0; i < kMaxHistoryEntries + 10; ++i)
        big = historyMerged(big, {"bash", "c" + std::to_string(i),
                                  static_cast<long long>(i), 1, 0});
    CHECK(big.size() == kMaxHistoryEntries, "history capped");
    CHECK(big.front().command == "c" + std::to_string(kMaxHistoryEntries + 9), "the oldest went");

    const std::vector<HistoryEntry> gone = historyWithout(history, "bash", "ls");
    CHECK(gone.size() == history.size() - 1, "delete removes one entry");

    CHECK(historyMatching(history, "small", 8).size() == 1, "substring match");
    CHECK(historyMatching(history, "", 8).size() == history.size(),
          "empty text: newest first, limited");
}

static void testCleanup()
{
    const long long now = 1000000000LL;
    std::vector<std::pair<std::string, long long>> files;
    for (long long i = 0; i < 60; ++i)
        files.push_back({"r" + std::to_string(i) + ".log", now - i * 86400}); // one per day
    const std::vector<std::string> expired = expiredRunFiles(files, now, 50, 7);
    CHECK(expired.size() == 52, "within 7 days are 8, keep min(50, 8) → 52 expire");
    bool hasRecent = false;
    for (const std::string& name : expired)
        if (name == "r5.log")
            hasRecent = true;
    CHECK(!hasRecent, "a 5-day-old file stays");

    std::vector<std::pair<std::string, long long>> fresh;
    for (long long i = 0; i < 60; ++i)
        fresh.push_back({"r" + std::to_string(i) + ".log", now - i}); // all fresh
    CHECK(expiredRunFiles(fresh, now, 50, 7).size() == 10, "all within 7 days: cap at 50");
}

static void testMisc()
{
    CHECK(formatSeconds(3.24) == "3.2s", "seconds format");
    CHECK(formatSeconds(65.0) == "1:05", "minutes format");
    CHECK(firstLineOf("a\nb") == "a", "first line only");
    CHECK(firstLineOf("中文中文中文中文中文", 7).size() == 6 + 3,
          "utf-8 boundary cut back to 2 chars + …");
    CHECK(normalizeLines("a\r\nb\nc", "\n") == "a\nb\nc", "normalize to LF");
    CHECK(normalizeLines("a\nb", "\r\n") == "a\r\nb", "normalize to CRLF");

    const std::map<std::string, bool> present {{"pwsh", true}, {"powershell", true}, {"cmd", true}};
    CHECK(autoDefaultShell(present) == "pwsh", "auto prefers pwsh");
    const std::map<std::string, bool> noPwsh {{"powershell", true}, {"cmd", true}};
    CHECK(autoDefaultShell(noPwsh) == "powershell", "auto falls to powershell");
    const std::map<std::string, bool> onlyCmd {{"cmd", true}};
    CHECK(autoDefaultShell(onlyCmd) == "cmd", "auto falls to cmd");

    const std::map<std::string, bool> all {{"pwsh", true}, {"cmd", true}, {"bash", true}};
    CHECK(resolveShellName("default", "auto", all, {}) == "pwsh", "default: auto");
    CHECK(resolveShellName("default", "bash", all, {}) == "bash", "default: named builtin");
    CHECK(resolveShellName("default", "nope", all, {}) == "", "default: unknown name");
    CHECK(resolveShellName("bash", "", all, {}) == "bash", "command id picks its shell");
    CHECK(resolveShellName("pwsh", "", {{"cmd", true}}, {}) == "", "missing shell resolves empty");
    CHECK(resolveShellName("default", "mine", {{"cmd", true}}, {"mine"}) == "mine",
          "default: a custom shell name");

    const long long id = newRunId(1730000000LL, 5);
    CHECK(id > 0 && id < 10000000000LL, "run id fits the #r<id> form");
    CHECK(newRunId(1730000000LL, 5) == newRunId(1730000000LL, 5), "run id deterministic");
    CHECK(quoteIfNeeded("plain") == "plain", "no quoting when clean");
    CHECK(quoteIfNeeded("a b") == "\"a b\"", "spaces quoted");
}

int main()
{
    testParseInput();
    testCustomShells();
    testScriptGeneration();
    testWslPaths();
    testDecode();
    testDangerRules();
    testConfirm();
    testHistory();
    testCleanup();
    testMisc();
    std::printf("shell logic tests: %d/%d passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
