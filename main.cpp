// main.cpp - the protocol glue for Quick Shell (ra9fael.shell).
//
// The pure rules live in logic.h, the Win32 side in runner.h; this file only
// maps between Win顺's JSON-RPC lines and those two: rows for the search box,
// effects for the actions, and the notifications the run manager asks for.
// English comments per the project convention; user-visible strings through
// tr(zh, en).
#include "runner.h"

using namespace winshun;
using namespace shellplug;

namespace {

std::string tr(bool zh, const std::string& zhText, const std::string& enText)
{
    return zh ? zhText : enText;
}

// The keyword a command id goes by (for setQuery back into this plugin).
std::string keywordOf(const std::string& commandId)
{
    return commandId == "default" ? ">" : commandId;
}

// What a shell is called in rows.
std::string shellTitle(bool zh, const std::string& name)
{
    if (name == kCmd)
        return "cmd";
    if (name == kPwsh)
        return zh ? "PowerShell 7" : "PowerShell 7";
    if (name == kPowerShell)
        return zh ? "Windows PowerShell" : "Windows PowerShell";
    if (name == kBash)
        return "Git Bash";
    if (name == kMsys)
        return "MSYS2";
    if (name == kWsl)
        return "WSL";
    return name;
}

std::string dangerTitle(bool zh, const std::string& rule)
{
    if (rule == "download")
        return tr(zh, "疑似下载后执行", "Looks like download-and-run");
    if (rule == "destructive")
        return tr(zh, "删除性操作", "Destructive operation");
    if (rule == "system")
        return tr(zh, "系统设置更改", "System configuration change");
    if (rule == "invisible")
        return tr(zh, "命令里有隐形字符", "Invisible characters in the command");
    return rule;
}

// The script for a custom shell: the generating rules follow the program, the
// extension follows the setting.
ScriptSpec scriptForCustom(const CustomShell& c, const std::string& command)
{
    const std::string exe = lower(c.program);
    const std::size_t slash = exe.find_last_of("/\\");
    const std::string file = slash == std::string::npos ? exe : exe.substr(slash + 1);
    ScriptSpec spec;
    if (file == "cmd.exe" || file == "cmd")
        spec = scriptFor(kCmd, command);
    else if (file == "powershell.exe" || file == "powershell")
        spec = scriptFor(kPowerShell, command);
    else if (file == "pwsh.exe" || file == "pwsh")
        spec = scriptFor(kPwsh, command);
    else
        spec = scriptFor(kBash, command);
    if (!c.ext.empty())
        spec.ext = c.ext;
    return spec;
}

Item settingsRow(bool zh, const std::string& id)
{
    return Item(id, tr(zh, "这个插件要在 Win顺 的设置里配置", "Configure this plugin in WinShun's settings"),
                tr(zh, "设置 → 插件 → 命令执行", "Settings → Plugins → Quick Shell"))
        .invalid();
}

// A run's one-line summary for a title.
std::string runStatusTitle(bool zh, const RunState& s, bool full = true)
{
    const double secs = s.running ? (tickMs() - s.startedTick) / 1000.0
                                  : (s.endedTick - s.startedTick) / 1000.0;
    if (s.running)
        return tr(zh, "运行中 · ", "Running · ") + formatSeconds(secs);
    if (s.cancelled)
        return tr(zh, "已取消 · ", "Cancelled · ") + formatSeconds(secs);
    if (!full)
        return tr(zh, "退出码 ", "Exit ") + std::to_string(s.exitCode);
    return tr(zh, "退出码 ", "Exit code ") + std::to_string(s.exitCode) + " · " + formatSeconds(secs);
}

const char* runIcon(const RunState& s)
{
    if (s.running)
        return "color:#d97706"; // amber while it works
    if (s.cancelled)
        return "color:#6b7280";
    return s.exitCode == 0 ? "color:#16a34a" : "color:#dc2626";
}

class ShellPlugin final : public Plugin {
public:
    std::string name() const override { return "ra9fael.shell"; }

    void initialized() override
    {
        m_runs.init(dataDir(), [this] { refresh(); },
                    [this](const RunState& s) { runFinished(s); });
        reload();
    }

    void settingsChanged(const Json&) override { reload(); }

    Json queryResult(const Query& q) override
    {
        const Items items = query(q);
        Json::Object result {{"items", itemsJson(items)}};
        if (m_runs.runningCount() > 0)
            result["rerun"] = 0.5; // the result page redraws while a run works
        return Json(std::move(result));
    }

    Items query(const Query& q) override { return buildItems(q); }

    Effects action(const ActionRequest& a) override
    {
        const Json& item = a.item;
        const Json& data = item["data"];
        const std::string kind = data["kind"].toString();
        const std::string shell = data["shell"].toString();
        const std::string command = data["command"].toString();
        const bool zh = chinese();

        Settings st;
        Detection det;
        {
            const std::lock_guard lock(m_stateMutex);
            st = m_st;
            det = m_det;
        }

        if (a.action == "cancel") {
            m_runs.cancel(data["runId"].toInt());
            return {effect::keep()};
        }
        if (a.action == "showRun") {
            return {effect::keep(),
                    effect::setQuery(keywordOf(a.command) + " #r"
                                     + std::to_string(data["runId"].toInt()))};
        }
        if (a.action == "copyLine")
            return {effect::copy(data["text"].toString()), effect::notify(tr(zh, "已复制", "Copied")),
                    effect::keep()};
        if (a.action == "copyAll") {
            std::shared_ptr<RunState> run = m_runs.find(data["runId"].toInt());
            if (!run)
                return {effect::keep()};
            std::string all;
            {
                const std::lock_guard lock(run->mutex);
                for (const OutputLine& line : run->lines)
                    all += line.text + "\n";
            }
            return {effect::copy(all), effect::notify(tr(zh, "已复制", "Copied")), effect::keep()};
        }
        if (a.action == "pickShell")
            return {effect::keep(),
                    effect::setQuery(keywordOf(a.command) + " @" + data["name"].toString() + " ")};
        if (a.action == "clearHistory") {
            Items confirm;
            confirm.push_back(Item(
                "clearHistoryConfirm", tr(zh, "确认清空全部历史？", "Clear all history?"),
                tr(zh, "再按一次 Enter", "Press Enter again"))
                .withActions({{"clearHistoryConfirm", tr(zh, "确认清空", "Clear"), "Enter"}})
                .withData(Json::Object {{"kind", "clearHistoryConfirm"}}));
            return {effect::replace(confirm), effect::keep()};
        }
        if (a.action == "clearHistoryConfirm") {
            {
                const std::lock_guard lock(m_stateMutex);
                m_history.clear();
                saveHistory(dataDir(), m_history);
            }
            return {effect::keep(), effect::setQuery(keywordOf(a.command) + " ")};
        }
        if (a.action == "addHistory") {
            {
                const std::lock_guard lock(m_stateMutex);
                m_history = historyMerged(m_history,
                                          {shell, command, epochSeconds(), 1, 0});
                saveHistory(dataDir(), m_history);
            }
            return {effect::notify(tr(zh, "已加入历史", "Added to history")), effect::keep()};
        }
        if (a.action == "deleteHistory") {
            {
                const std::lock_guard lock(m_stateMutex);
                m_history = historyWithout(m_history, shell, command);
                saveHistory(dataDir(), m_history);
            }
            return {effect::keep(), effect::setQuery(keywordOf(a.command) + " ")};
        }
        if (a.action == "copyCommand")
            return {effect::copy(command), effect::notify(tr(zh, "已复制", "Copied")),
                    effect::hide()};
        if (a.action == "edit")
            return {effect::keep(), effect::setQuery(keywordOf(a.command) + " " + command)};
        if (a.action == "openTerminal") {
            const std::string workDir = st.workDir.empty() ? userHome() : st.workDir;
            const std::string error = openInTerminal(st, det, shell, command, dataDir() + "\\runs", workDir);
            if (!error.empty()) {
                notifyUser(tr(zh, "打不开终端：", "Cannot open the terminal: ") + error);
                return {effect::keep()};
            }
            return {effect::notify(tr(zh, "已在终端打开", "Opened in a terminal")), effect::hide()};
        }
        if (a.action == "execute") {
            if (kind == "clipboard") {
                // First Enter: the confirmation page replaces the list (design 5.3).
                return {effect::replace(confirmItems(shell, command, zh, st)), effect::keep()};
            }
            if (kind == "clipboardConfirm" || kind == "command" || kind == "history" || kind == "rerun")
                return startRun(a.command, shell, command, kind == "clipboardConfirm", st, det, zh);
        }
        return Plugin::action(a);
    }

private:
    void reload()
    {
        {
            const std::lock_guard lock(m_stateMutex);
            m_st = readSettings(settings());
            m_det = detectShells(m_st);
        }
        bool bad = false;
        const std::vector<HistoryEntry> history = loadHistory(dataDir(), &bad);
        if (bad) {
            // The file is not valid JSON: move it aside and start empty.
            ::MoveFileExW(winshun::wide(dataDir() + "\\history.json").c_str(),
                          winshun::wide(dataDir() + "\\history.json.bad").c_str(),
                          MOVEFILE_REPLACE_EXISTING);
            m_history.clear();
        } else {
            const std::lock_guard lock(m_stateMutex);
            m_history = history;
        }
        cleanupRuns(dataDir());
    }

    std::vector<std::string> customNames(const Detection& det) const
    {
        std::vector<std::string> names;
        for (const CustomShell& c : det.customs)
            names.push_back(c.name);
        return names;
    }

    bool nameKnown(const Detection& det, const std::string& name) const
    {
        for (const std::string& n : builtinShellNames())
            if (n == name)
                return true;
        for (const CustomShell& c : det.customs)
            if (c.name == name)
                return true;
        return false;
    }

    std::optional<CustomShell> customByName(const Detection& det, const std::string& name) const
    {
        for (const CustomShell& c : det.customs)
            if (c.name == name)
                return c;
        return std::nullopt;
    }

    // ---- the lists --------------------------------------------------------------

    Items buildItems(const Query& q)
    {
        const bool zh = chinese();
        Settings st;
        Detection det;
        {
            const std::lock_guard lock(m_stateMutex);
            st = m_st;
            det = m_det;
        }
        const std::vector<std::string> names = customNames(det);
        const std::string shell = resolveShellName(q.command, st.defaultShell, det.present, names);
        const ParsedInput p = parseInput(q.text);

        if (p.kind == ParsedInput::Kind::ResultPage)
            return resultPage(p.runId, st, zh);
        if (p.kind == ParsedInput::Kind::ShellList
            || (p.kind == ParsedInput::Kind::ShellRequest && trim(p.command).empty()))
            return shellList(q, det, zh);
        if (p.kind == ParsedInput::Kind::Empty || trim(p.command).empty())
            return emptyState(q, shell, st, zh);

        // Command or ShellRequest: the rows to run what is written.
        const std::string runShell = p.kind == ParsedInput::Kind::ShellRequest ? p.shellName : shell;
        if (p.kind == ParsedInput::Kind::ShellRequest && !nameKnown(det, p.shellName)) {
            Items items;
            items.push_back(Item(
                "unknownShell", tr(zh, "没有叫 " + p.shellName + " 的 shell", "No shell named " + p.shellName),
                tr(zh, "@ 后面写自定义 shell 的名字", "A custom shell's name after @"))
                .invalid());
            for (Item& row : shellList(q, det, zh))
                items.push_back(std::move(row));
            return items;
        }
        Items items;
        appendCommandRows(items, runShell, trim(p.command), st, zh);
        return items;
    }

    // The rows for a command about to run: warnings, the run row, history.
    void appendCommandRows(Items& items, const std::string& shell, const std::string& command,
                           const Settings& st, const bool zh)
    {
        if (shell.empty()) {
            items.push_back(Item("noShell", tr(zh, "没有检测到可用的 shell", "No usable shell detected"),
                                 tr(zh, "检查路径设置", "Check the path settings"))
                                .invalid());
            items.push_back(settingsRow(zh, "openSettings"));
            return;
        }
        for (const std::string& rule : dangerRules(command))
            items.push_back(Item("danger." + rule, dangerTitle(zh, rule),
                                 tr(zh, "确认后再执行", "Check before running"))
                                .invalid());
        std::string detail = shellTitle(zh, shell);
        if (!st.workDir.empty())
            detail += " · " + firstLineOf(st.workDir, 40);
        items.push_back(
            Item("run", tr(zh, "执行：", "Run: ") + firstLineOf(command, 80), detail)
                .withActions({{"execute", tr(zh, "执行", "Run"), "Enter"},
                              {"openTerminal", tr(zh, "在终端里执行", "Run in a terminal"), "Ctrl+Enter"},
                              {"edit", tr(zh, "改写命令", "Edit the command"), "Shift+Enter"},
                              {"copyCommand", tr(zh, "复制命令", "Copy command"), ""}})
                .withData(Json::Object {{"kind", "command"}, {"shell", shell}, {"command", command}}));
        {
            const std::lock_guard lock(m_stateMutex);
            std::size_t shown = 0;
            for (const HistoryEntry& e : historyMatching(m_history, command, 5)) {
                if (e.command == command)
                    continue;
                ++shown;
                items.push_back(
                    Item("history." + std::to_string(shown), firstLineOf(e.command, 80),
                         shellTitle(zh, e.shell) + " · " + tr(zh, "用过 ", "used ")
                             + std::to_string(e.count) + tr(zh, " 次", "×"))
                        .withActions({{"execute", tr(zh, "执行", "Run"), "Enter"},
                                      {"openTerminal", tr(zh, "在终端里执行", "Run in a terminal"), "Ctrl+Enter"},
                                      {"edit", tr(zh, "改写命令", "Edit the command"), "Shift+Enter"},
                                      {"deleteHistory", tr(zh, "从历史中删除", "Delete from history"), ""}})
                        .withData(Json::Object {{"kind", "history"},
                                                {"shell", e.shell},
                                                {"command", e.command}}));
            }
        }
    }

    // `bash ` / `> ` with nothing after it (design 5.2).
    Items emptyState(const Query& q, const std::string& shell, const Settings& st, const bool zh)
    {
        Items items;
        if (shell.empty()) {
            items.push_back(Item("noShell",
                                 tr(zh, "没有检测到可用的 shell", "No usable shell detected"))
                                .invalid());
            items.push_back(settingsRow(zh, "openSettings"));
            return items;
        }
        // ① The terminal itself.
        items.push_back(
            Item("openShellTerminal", tr(zh, "打开 ", "Open the ") + shellTitle(zh, shell)
                                          + tr(zh, " 终端", " terminal"))
                .withActions({{"openTerminal", tr(zh, "打开终端", "Open the terminal"), "Enter"}})
                .withData(Json::Object {{"kind", "openShellTerminal"},
                                        {"shell", shell},
                                        {"command", ""}}));
        // ② The clipboard, if it holds something runnable.
        appendClipboardRow(items, shell, st, zh);
        // ③ The runs, newest first (design 5.2: running ones surface here too).
        const std::vector<std::shared_ptr<RunState>> runs = m_runs.all();
        for (auto it = runs.rbegin(); it != runs.rend() && items.size() <= 12; ++it) {
            const RunState& run = **it;
            items.push_back(
                Item("run." + std::to_string(run.id), runStatusTitle(zh, run),
                     shellTitle(zh, run.shellName) + " · " + firstLineOf(run.command, 60))
                    .withIcon(runIcon(run))
                    .withActions({{"showRun", tr(zh, "查看", "Show"), "Enter"}})
                    .withData(Json::Object {{"kind", "showRun"}, {"runId", run.id}}));
        }
        // ④ The history (all shells under `>`).
        const bool allShells = q.command == "default";
        {
            const std::lock_guard lock(m_stateMutex);
            std::size_t shown = 0;
            for (const HistoryEntry& e : m_history) {
                if (!allShells && e.shell != shell)
                    continue;
                if (shown >= 6)
                    break;
                ++shown;
                items.push_back(
                    Item("history." + std::to_string(shown), firstLineOf(e.command, 80),
                         shellTitle(zh, e.shell) + " · " + tr(zh, "用过 ", "used ")
                             + std::to_string(e.count) + tr(zh, " 次", "×"))
                        .withActions({{"execute", tr(zh, "执行", "Run"), "Enter"},
                                      {"openTerminal", tr(zh, "在终端里执行", "Run in a terminal"), "Ctrl+Enter"},
                                      {"edit", tr(zh, "改写命令", "Edit the command"), "Shift+Enter"},
                                      {"deleteHistory", tr(zh, "从历史中删除", "Delete from history"), ""}})
                        .withData(Json::Object {{"kind", "history"},
                                                {"shell", e.shell},
                                                {"command", e.command}}));
            }
            if (allShells && !m_history.empty())
                items.push_back(
                    Item("clearHistory", tr(zh, "清空历史", "Clear history"))
                        .withActions({{"clearHistory", tr(zh, "清空历史", "Clear history"), "Enter"}})
                        .withData(Json::Object {{"kind", "clearHistory"}}));
        }
        return items;
    }

    void appendClipboardRow(Items& items, const std::string& shell, const Settings& st, const bool zh)
    {
        const std::optional<std::string> clip = clipboardText();
        if (!clip) {
            items.push_back(
                Item("clipboard", tr(zh, "剪贴板里没有文本", "No text on the clipboard")).invalid());
            return;
        }
        const std::string& text = *clip;
        const bool multiline = text.find('\n') != std::string::npos;
        const bool dangerous = !dangerRules(text).empty();
        if (!shouldConfirm(st.clipboardConfirm, multiline, dangerous)) {
            // No confirmation wanted: run it straight away.
            items.push_back(
                Item("clipboard", tr(zh, "执行剪贴板内容", "Run the clipboard content"),
                     firstLineOf(text, 80))
                    .withActions({{"execute", tr(zh, "执行", "Run"), "Enter"},
                                  {"copyCommand", tr(zh, "复制", "Copy"), "Ctrl+Enter"}})
                    .withData(Json::Object {{"kind", "clipboard"},
                                            {"shell", shell},
                                            {"command", text}}));
            return;
        }
        const std::string lines = std::to_string(static_cast<long long>(std::count(text.begin(), text.end(), '\n')) + 1);
        items.push_back(
            Item("clipboard", tr(zh, "执行剪贴板内容（", "Run the clipboard content (")
                                  + lines + tr(zh, " 行）", " lines)"),
                 firstLineOf(text, 80) + (multiline || dangerous
                                              ? " · " + tr(zh, "Enter 后确认", "Enter to confirm")
                                              : std::string()))
                .withActions({{"execute", tr(zh, "执行", "Run"), "Enter"}})
                .withData(Json::Object {{"kind", "clipboard"},
                                        {"shell", shell},
                                        {"command", text}}));
    }

    // The confirmation page for clipboard content (replace effect).
    Items confirmItems(const std::string& shell, const std::string& text, const bool zh,
                       const Settings& st)
    {
        Items items;
        for (const std::string& rule : dangerRules(text))
            items.push_back(Item("danger." + rule, dangerTitle(zh, rule),
                                 tr(zh, "确认后再执行", "Check before running"))
                                .invalid());
        const ConfirmLines page = confirmPageLines(text);
        for (std::size_t i = 0; i < page.lines.size(); ++i)
            items.push_back(Item("clip." + std::to_string(i), page.lines[i]).invalid());
        if (page.hidden > 0)
            items.push_back(
                Item("clipMore",
                     tr(zh, "还有 " + std::to_string(page.hidden) + " 行，确认执行后可在结果页看全部输出",
                        "…and " + std::to_string(page.hidden) + " more lines"))
                    .invalid());
        items.push_back(
            Item("confirm", tr(zh, "确认执行（", "Confirm (") + shellTitle(zh, shell) + "）",
                 st.workDir.empty() ? userHome() : st.workDir)
                .withActions({{"execute", tr(zh, "执行", "Run"), "Enter"},
                              {"copyCommand", tr(zh, "复制命令", "Copy command"), "Shift+Enter"}})
                .withData(Json::Object {{"kind", "clipboardConfirm"},
                                        {"shell", shell},
                                        {"command", text}}));
        return items;
    }

    // `> @` and `@name` with nothing after it.
    Items shellList(const Query& q, const Detection& det, const bool zh)
    {
        Items items;
        for (const CustomShellProblem& problem : det.customProblems) {
            const std::string why = problem.code == CustomShellProblem::Code::BadFormat
                                        ? tr(zh, "格式不对（应为 名字=程序 参数）",
                                             "bad format (want name=program args)")
                                        : problem.code == CustomShellProblem::Code::BadName
                                              ? tr(zh, "名字只能用字母、数字、- 和 _",
                                                   "the name may use letters, digits, - and _ only")
                                              : problem.code == CustomShellProblem::Code::Reserved
                                                    ? tr(zh, "和内置 shell 重名", "a built-in shell has this name")
                                                    : tr(zh, "重名", "duplicate name");
            items.push_back(
                Item("customProblem." + std::to_string(problem.index),
                     tr(zh, "自定义 shell 第 " + std::to_string(problem.index) + " 项：",
                        "Custom shell entry " + std::to_string(problem.index) + ": ")
                         + why)
                    .invalid());
        }
        for (const std::string& name : builtinShellNames()) {
            const auto it = det.present.find(name);
            if (it == det.present.end() || !it->second)
                continue;
            const auto program = det.programs.find(name);
            items.push_back(
                Item("shell." + name, "@" + name + "  " + shellTitle(zh, name),
                     program == det.programs.end() ? std::string() : program->second)
                    .withAutocomplete(keywordOf(q.command) + " @" + name + " ")
                    .withActions({{"pickShell", tr(zh, "选用", "Use"), "Enter"}})
                    .withData(Json::Object {{"kind", "pickShell"}, {"name", name}}));
        }
        for (const CustomShell& c : det.customs)
            items.push_back(
                Item("shell." + c.name, "@" + c.name, c.program)
                    .withAutocomplete(keywordOf(q.command) + " @" + c.name + " ")
                    .withActions({{"pickShell", tr(zh, "选用", "Use"), "Enter"}})
                    .withData(Json::Object {{"kind", "pickShell"}, {"name", c.name}}));
        if (items.empty())
            items.push_back(Item("noShell", tr(zh, "没有可用的 shell", "No usable shell")).invalid());
        return items;
    }

    // `bash #` / `bash #r42` (design 6).
    Items resultPage(long long runId, const Settings& st, const bool zh)
    {
        Items items;
        std::shared_ptr<RunState> run = runId == 0 ? m_runs.latest() : m_runs.find(runId);
        if (!run) {
            items.push_back(
                Item("noRun", tr(zh, "没有这次运行", "No such run"),
                     tr(zh, "# 是最近一次，#r42 是第 42 次", "# is the latest, #r42 the one with id 42"))
                    .invalid());
            return items;
        }
        const long long id = run->id;
        const std::string base = "run:" + std::to_string(id);
        std::string statusSubtitle = shellTitle(zh, run->shellName);
        {
            const std::lock_guard lock(run->mutex);
            if (!run->command.empty())
                statusSubtitle += " · " + firstLineOf(run->command, 60);
        }
        Item status(base + ":status", runStatusTitle(zh, *run), statusSubtitle);
        status.withIcon(runIcon(*run));
        if (run->running)
            status.withActions({{"cancel", tr(zh, "取消", "Cancel"), "Enter"}})
                .withData(Json::Object {{"kind", "cancelRun"}, {"runId", id}});
        else
            status.withActions({{"execute", tr(zh, "再执行一次", "Run again"), "Enter"},
                                {"openTerminal", tr(zh, "在终端里执行", "Run in a terminal"), "Ctrl+Enter"}})
                .withData(Json::Object {{"kind", "rerun"},
                                        {"shell", run->shellName},
                                        {"command", run->command}});
        items.push_back(std::move(status));

        std::vector<OutputLine> lines;
        {
            const std::lock_guard lock(run->mutex);
            lines.assign(run->lines.begin(), run->lines.end());
        }
        const std::size_t shown = lines.size() > static_cast<std::size_t>(st.outputLines)
                                      ? static_cast<std::size_t>(st.outputLines)
                                      : lines.size();
        const std::size_t firstShown = lines.size() - shown;
        for (std::size_t i = firstShown; i < lines.size(); ++i) {
            Item line(base + ":line:" + std::to_string(lines[i].seq), showInvisibles(lines[i].text));
            if (lines[i].isErr)
                line.withIcon("color:#dc2626");
            line.withActions({{"copyLine", tr(zh, "复制此行", "Copy this line"), "Enter"}})
                .withData(Json::Object {{"kind", "line"},
                                        {"runId", id},
                                        {"text", lines[i].text}});
            items.push_back(std::move(line));
        }
        if (lines.size() > shown)
            items.push_back(
                Item(base + ":truncated",
                     tr(zh, "只显示最后 " + std::to_string(shown) + " 行，完整输出在日志里",
                        "Only the last " + std::to_string(shown) + " lines; the log has it all"))
                    .withPath(run->logPath)
                    .invalid());

        if (!lines.empty())
            items.push_back(
                Item(base + ":copyAll", tr(zh, "复制全部输出", "Copy all output"))
                    .withActions({{"copyAll", tr(zh, "复制", "Copy"), "Enter"}})
                    .withData(Json::Object {{"kind", "copyAll"}, {"runId", id}}));
        items.push_back(
            Item(base + ":log", tr(zh, "在编辑器里打开日志", "Open the log in the editor"))
                .withPath(run->logPath));
        items.push_back(
            Item(base + ":terminal", tr(zh, "在终端里执行这条命令", "Run this command in a terminal"))
                .withActions({{"openTerminal", tr(zh, "打开终端", "Open a terminal"), "Enter"}})
                .withData(Json::Object {{"kind", "rerunTerminal"},
                                        {"shell", run->shellName},
                                        {"command", run->command}}));
        if (run->fromClipboard) {
            bool already = false;
            {
                const std::lock_guard lock(m_stateMutex);
                for (const HistoryEntry& e : m_history)
                    if (e.shell == run->shellName && e.command == run->command)
                        already = true;
            }
            if (!already)
                items.push_back(
                    Item(base + ":addHistory", tr(zh, "把这条命令加入历史", "Add this command to history"))
                        .withActions({{"addHistory", tr(zh, "加入历史", "Add to history"), "Enter"}})
                        .withData(Json::Object {{"kind", "addHistory"},
                                                {"shell", run->shellName},
                                                {"command", run->command}}));
        }
        return items;
    }

    // ---- starting a run ----------------------------------------------------------

    Effects startRun(const std::string& queryCommand, const std::string& shell,
                     const std::string& command, bool fromClipboard, const Settings& st,
                     const Detection& det, const bool zh)
    {
        if (command.empty())
            return {effect::keep()};
        const std::string workDir = st.workDir.empty() ? userHome() : st.workDir;
        const ShellSpec spec = shellSpec(st, det, shell, workDir);
        if (!spec.valid) {
            notifyUser(tr(zh, "没有找到 shell：", "Shell not found: ") + shell);
            return {effect::keep()};
        }
        ScriptSpec script;
        if (const std::optional<CustomShell> custom = customByName(det, shell))
            script = scriptForCustom(*custom, command);
        else
            script = scriptFor(shell, command);

        long long id = 0;
        const StartError err = m_runs.start(shell, spec.program, spec.args, command, script,
                                            fromClipboard, spec.env, workDir, st.saveHistory,
                                            st.notifyAfterSec, &id);
        if (err == StartError::Busy) {
            notifyUser(tr(zh, "已有 4 个命令在后台运行，等它们结束再试", "4 runs are active; try again when one ends"));
            return {effect::keep()};
        }
        if (err != StartError::Ok) {
            notifyUser(tr(zh, "启动失败（", "Failed to start (") + shell + "）");
            return {effect::keep()};
        }
        // The result page becomes the box's state: `bash #r42` reopens it (design 6.1).
        return {effect::keep(),
                effect::setQuery(keywordOf(queryCommand) + " #r" + std::to_string(id))};
    }

    // From the run manager's worker threads.
    void runFinished(const RunState& s)
    {
        const bool zh = chinese();
        const double secs = (s.endedTick - s.startedTick) / 1000.0;
        // Only runs long enough surface as a system notification (design 6.3);
        // the text stays free of the command and the output.
        if (secs >= s.notifyAfterSec && !s.cancelled) {
            const std::string title = tr(zh, "完成 · 退出码 " + std::to_string(s.exitCode) + " · "
                                                + formatSeconds(secs),
                                          "Finished · exit " + std::to_string(s.exitCode) + " · "
                                                + formatSeconds(secs));
            toast(title, "", {effect::setQuery("> #r" + std::to_string(s.id))});
        }
        if (s.saveHistory && !s.fromClipboard && !s.command.empty()) {
            const std::lock_guard lock(m_stateMutex);
            m_history = historyMerged(m_history,
                                      {s.shellName, s.command, s.startedEpoch, 1, s.exitCode});
            saveHistory(dataDir(), m_history);
        }
    }

    Settings m_st;
    Detection m_det;
    std::vector<HistoryEntry> m_history;
    std::mutex m_stateMutex; // guards m_st / m_det / m_history
    RunManager m_runs;
};

} // namespace

int main()
{
    ShellPlugin plugin;
    return plugin.serve();
}
