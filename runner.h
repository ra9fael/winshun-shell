// runner.h - the Win32 side of the Quick Shell plugin: shell detection,
// process management, clipboard, and the history/runs files.
//
// Everything OS-dependent lives here; the pure rules are in logic.h and
// main.cpp only glues both onto the SDK. Threads: the protocol thread never
// waits on a child process - action() starts a run and returns at once
// (design 3/6.1); each run owns a worker thread with a stdout and a stderr
// reader, updates shared state under a mutex, and calls the plugin's
// refresh() so an open result page redraws.
#pragma once

#include "logic.h"
#include "winshun.h"

#include <windows.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>

namespace shellplug {

// ---- misc Win32 helpers ---------------------------------------------------------

// A file (or folder) that exists; false otherwise.
inline bool fileExists(const std::string& path)
{
    if (path.empty())
        return false;
    const DWORD attrs = ::GetFileAttributesW(winshun::wide(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// The full path of an exe on PATH (or the system dirs); empty when absent.
inline std::string findOnPath(const wchar_t* exe)
{
    wchar_t buffer[MAX_PATH];
    wchar_t* filePart = nullptr;
    const DWORD n = ::SearchPathW(nullptr, exe, nullptr, MAX_PATH, buffer, &filePart);
    if (n == 0 || n >= MAX_PATH || !filePart)
        return {};
    return winshun::utf8(std::wstring(buffer, n));
}

// A registry string value (HKLM then HKCU); empty when absent.
inline std::string registryString(const wchar_t* subkey, const wchar_t* value)
{
    wchar_t buffer[MAX_PATH * 2];
    DWORD size = sizeof(buffer);
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        if (::RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, nullptr, buffer, &size)
                == ERROR_SUCCESS
            && size > sizeof(wchar_t)) {
            const std::wstring w(buffer, size / sizeof(wchar_t) - 1);
            return winshun::utf8(w);
        }
        size = sizeof(buffer);
    }
    return {};
}

inline std::string envWide(const wchar_t* name)
{
    wchar_t buffer[MAX_PATH * 4];
    const DWORD n = ::GetEnvironmentVariableW(name, buffer, MAX_PATH * 4);
    return n > 0 && n < MAX_PATH * 4 ? winshun::utf8(std::wstring(buffer, n)) : std::string();
}

inline std::string userHome()
{
    return envWide(L"USERPROFILE");
}

// The OEM code page decode used as the fallback for non-UTF-8 output.
inline std::string oemToUtf8(const std::string& bytes)
{
    const UINT codePage = ::GetOEMCP();
    const int wide = ::MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()),
                                           nullptr, 0);
    if (wide <= 0)
        return {};
    std::wstring w(static_cast<std::size_t>(wide), L'\0');
    ::MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()), w.data(), wide);
    return winshun::utf8(w);
}

inline long long epochSeconds()
{
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
}

inline long long tickMs()
{
    return static_cast<long long>(::GetTickCount64());
}

// An environment block: the caller's variables plus the shell-specific ones.
// Windows requires a Unicode block sorted case-insensitively by name (upper
// case before lower case on equal names) and CreateProcessW refuses an
// unsorted one with ERROR_INVALID_PARAMETER, so sort before joining.
inline std::wstring buildEnvironmentBlock(const std::map<std::string, std::string>& extra)
{
    std::vector<std::wstring> entries;
    wchar_t* block = ::GetEnvironmentStringsW();
    if (block) {
        for (wchar_t* p = block; *p; p += std::wcslen(p) + 1)
            entries.push_back(p);
        ::FreeEnvironmentStringsW(block);
    }
    for (const auto& [name, value] : extra)
        entries.push_back(winshun::wide(name) + L"=" + winshun::wide(value));
    const auto nameOf = [](const std::wstring& e) {
        const std::size_t at = e.find(L'=');
        return at == std::wstring::npos ? e : e.substr(0, at);
    };
    std::sort(entries.begin(), entries.end(), [&](const std::wstring& a, const std::wstring& b) {
        const std::wstring na = nameOf(a), nb = nameOf(b);
        for (std::size_t i = 0; i < na.size() && i < nb.size(); ++i) {
            const wchar_t ca = static_cast<wchar_t>(towlower(na[i])), cb = static_cast<wchar_t>(towlower(nb[i]));
            if (ca != cb)
                return ca < cb;
            if (na[i] != nb[i])
                return na[i] > nb[i]; // upper case first on equal names
        }
        return na.size() < nb.size();
    });
    std::wstring out;
    for (const std::wstring& e : entries) {
        out.append(e);
        out.push_back(L'\0');
    }
    if (out.empty())
        out.push_back(L'\0');
    out.push_back(L'\0');
    return out;
}

inline bool writeScriptFile(const std::string& path, const std::string& bytes)
{
    HANDLE f = ::CreateFileW(winshun::wide(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = ::WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
                    && written == bytes.size();
    ::CloseHandle(f);
    return ok;
}

// Runs a program and captures its output (short probes only, e.g. wsl -l -q).
// The process is killed after `timeoutMs` so detection cannot hang.
inline std::string runCapture(const std::string& commandLine, DWORD timeoutMs)
{
    SECURITY_ATTRIBUTES inherit {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!::CreatePipe(&readEnd, &writeEnd, &inherit, 0))
        return {};
    ::SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    STARTUPINFOW si {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &inherit, OPEN_EXISTING, 0, nullptr);
    PROCESS_INFORMATION pi {};
    std::wstring cmd = winshun::wide(commandLine);
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    BOOL ok = ::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (si.hStdInput != INVALID_HANDLE_VALUE)
        ::CloseHandle(si.hStdInput);
    ::CloseHandle(writeEnd);
    if (!ok) {
        ::CloseHandle(readEnd);
        return {};
    }
    const DWORD wait = ::WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wait == WAIT_TIMEOUT)
        ::TerminateProcess(pi.hProcess, 1); // a hung probe must not stall startup
    // The write end closed with the process, so the reads run to EOF.
    std::string out;
    char chunk[4096];
    DWORD got = 0;
    while (::ReadFile(readEnd, chunk, sizeof chunk, &got, nullptr) && got > 0)
        out.append(chunk, got);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(readEnd);
    return out;
}

// ---- clipboard (design 5.3) -------------------------------------------------------

// CF_UNICODETEXT up to 64 KiB; nullopt when the clipboard holds no text, is
// empty or is too long.
inline std::optional<std::string> clipboardText()
{
    if (!::OpenClipboard(nullptr))
        return std::nullopt;
    std::optional<std::string> out;
    const HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
    if (data) {
        const wchar_t* text = static_cast<const wchar_t*>(::GlobalLock(data));
        if (text) {
            std::wstring w(text);
            ::GlobalUnlock(data);
            if (w.size() <= 64 * 1024)
                out = winshun::utf8(w);
        }
    }
    ::CloseClipboard();
    return out;
}

// ---- shell detection (design 4.1) -------------------------------------------------

struct Settings {
    std::string defaultShell = "auto";
    std::string workDir;
    std::string pwshPath;
    std::string gitBashPath;
    std::string msysRoot = "C:\\msys64";
    std::string msysEnv = "UCRT64";
    std::string customShells;
    std::string terminal = "wt";
    std::string clipboardConfirm = "always";
    int outputLines = 50;
    int notifyAfterSec = 10;
    bool saveHistory = true;
};

inline Settings readSettings(const winshun::Json& s)
{
    Settings st;
    const auto str = [&](const char* key, const std::string& fallback) {
        const winshun::Json& v = s[key];
        return v.isString() ? v.toString() : fallback;
    };
    const auto num = [&](const char* key, int fallback, int lo, int hi) {
        const winshun::Json& v = s[key];
        const long long n = v.toInt(fallback);
        return static_cast<int>(n < lo ? lo : n > hi ? hi : n);
    };
    st.defaultShell = str("defaultShell", "auto");
    st.workDir = str("workDir", "");
    st.pwshPath = str("pwshPath", "");
    st.gitBashPath = str("gitBashPath", "");
    st.msysRoot = str("msysRoot", "C:\\msys64");
    st.msysEnv = str("msysEnv", "UCRT64");
    st.customShells = str("customShells", "");
    st.terminal = str("terminal", "wt");
    st.clipboardConfirm = str("clipboardConfirm", "always");
    st.outputLines = num("outputLines", 50, 10, 190);
    st.notifyAfterSec = num("notifyAfterSec", 10, 0, 3600);
    st.saveHistory = s["saveHistory"].toBool(true);
    return st;
}

struct Detection {
    std::map<std::string, bool> present;
    std::map<std::string, std::string> programs; // shell name → exe path
    std::vector<CustomShell> customs;
    std::vector<CustomShellProblem> customProblems;
};

// Detection runs once at startup and again when settings change; results stay
// in memory. wsl costs one short process probe; the rest are file/registry
// reads.
inline Detection detectShells(const Settings& st)
{
    Detection d;
    const auto mark = [&](const std::string& name, const std::string& program) {
        d.present[name] = !program.empty() && fileExists(program);
        if (d.present[name])
            d.programs[name] = program;
    };

    const std::string comspec = envWide(L"ComSpec");
    mark(kCmd, comspec.empty() ? findOnPath(L"cmd.exe") : comspec);

    std::string pwsh = st.pwshPath;
    if (pwsh.empty())
        pwsh = registryString(
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\pwsh.exe", nullptr);
    if (pwsh.empty())
        pwsh = findOnPath(L"pwsh.exe");
    mark(kPwsh, pwsh);

    mark(kPowerShell,
         envWide(L"SystemRoot") + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");

    std::string bash = st.gitBashPath;
    if (bash.empty()) {
        const std::string gitRoot = registryString(L"SOFTWARE\\GitForWindows", L"InstallPath");
        if (!gitRoot.empty())
            bash = gitRoot + "\\bin\\bash.exe";
    }
    if (bash.empty())
        bash = findOnPath(L"bash.exe");
    mark(kBash, bash);

    std::string msysRoot = st.msysRoot.empty() ? "C:\\msys64" : st.msysRoot;
    mark(kMsys, msysRoot + "\\usr\\bin\\bash.exe");

    // wsl.exe exists and has at least one distribution (`wsl -l -q`).
    const std::string wslExe = envWide(L"SystemRoot") + "\\System32\\wsl.exe";
    if (fileExists(wslExe)) {
        const std::string out = runCapture("\"" + wslExe + "\" -l -q", 4000);
        // The listing comes back as UTF-16LE; non-empty bytes mean at least
        // one distribution.
        mark(kWsl, out.size() >= 4 ? wslExe : std::string());
    } else
        d.present[kWsl] = false;

    const CustomShellParse parsed = parseCustomShells(st.customShells, builtinShellNames());
    d.customs = parsed.shells;
    d.customProblems = parsed.problems;
    return d;
}

// ---- run manager (design 3 / 6) ----------------------------------------------------

constexpr std::size_t kMaxKeptLines = 200;
constexpr long long kMaxLogBytes = 10 * 1024 * 1024;
constexpr std::size_t kMaxConcurrentRuns = 4;

struct OutputLine {
    std::string text;
    bool isErr = false;
    long long seq = 0; // stable id for run:<id>:line:<seq>
};

struct RunState {
    long long id = 0;
    std::string shellName; // display name: builtin or custom
    std::string command;   // the command text
    bool fromClipboard = false;
    long long startedEpoch = 0; // Unix seconds
    long long startedTick = 0;  // for the duration text
    long long endedTick = 0;
    std::atomic<bool> running {true};
    std::atomic<bool> cancelled {false};
    long long exitCode = 0;
    std::string logPath;
    std::string scriptPath; // deleted after a background run ends
    bool keepScript = false;
    // Settings captured when the run started, so the worker does not read
    // shared settings state.
    bool saveHistory = true;
    int notifyAfterSec = 10;
    std::mutex mutex; // guards lines + exit code
    std::deque<OutputLine> lines;
    long long nextSeq = 0;
    long long logBytes = 0;
    bool logTruncated = false;
};

enum class StartError { Ok, ShellMissing, Busy, SpawnFailed };

class RunManager {
public:
    // dataDir: WS_DATA_DIR; onLineArrived/onFinished fire from worker threads.
    void init(const std::string& dataDir, std::function<void()> onUpdate, std::function<void(const RunState&)> onFinish)
    {
        m_dataDir = dataDir;
        m_onUpdate = std::move(onUpdate);
        m_onFinish = std::move(onFinish);
    }

    const std::string& dataDir() const { return m_dataDir; }

    // A shared pointer keeps the state alive even if the map entry is pruned.
    std::shared_ptr<RunState> find(long long id) const
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_runs.find(id);
        return it == m_runs.end() ? nullptr : it->second;
    }

    // The most recent run, running or finished.
    std::shared_ptr<RunState> latest() const
    {
        std::lock_guard lock(m_mutex);
        if (m_order.empty())
            return nullptr;
        return m_runs.at(m_order.back());
    }

    std::vector<std::shared_ptr<RunState>> all() const
    {
        std::lock_guard lock(m_mutex);
        std::vector<std::shared_ptr<RunState>> out;
        for (long long id : m_order)
            out.push_back(m_runs.at(id));
        return out;
    }

    StartError start(const std::string& shellName, const std::string& program,
                     const std::string& backgroundArgs, const std::string& command,
                     const ScriptSpec& script, bool fromClipboard,
                     const std::map<std::string, std::string>& extraEnv,
                     const std::string& workDir, bool saveHistory, int notifyAfterSec,
                     long long* outId = nullptr)
    {
        std::lock_guard lock(m_mutex);
        std::size_t runningCount = 0;
        for (const auto& [id, state] : m_runs)
            if (state->running)
                ++runningCount;
        if (runningCount >= kMaxConcurrentRuns)
            return StartError::Busy;

        const long long id = newRunId(epochSeconds(), m_counter++);
        auto state = std::make_shared<RunState>();
        state->id = id;
        state->shellName = shellName;
        state->command = command;
        state->fromClipboard = fromClipboard;
        state->startedEpoch = epochSeconds();
        state->startedTick = tickMs();
        state->saveHistory = saveHistory;
        state->notifyAfterSec = notifyAfterSec;

        const std::string runsDir = m_dataDir + "\\runs";
        ::CreateDirectoryW(winshun::wide(runsDir).c_str(), nullptr);
        state->scriptPath = runsDir + "\\" + std::to_string(id) + script.ext;
        state->logPath = runsDir + "\\" + std::to_string(id) + ".log";
        HANDLE logHandle = openAppend(state->logPath);
        if (!writeScriptFile(state->scriptPath, scriptBytes(script))) {
            if (logHandle)
                ::CloseHandle(logHandle);
            return StartError::SpawnFailed;
        }

        // cmd line: the shell's arguments with the script path last, unless a
        // custom shell placed {script} itself (then backgroundArgs holds the
        // ready line).
        std::string cmdLine;
        if (backgroundArgs.find("{script}") != std::string::npos
            || backgroundArgs.find("{wslscript}") != std::string::npos)
            cmdLine = customShellCommandLine({"", program, backgroundArgs, script.ext,
                                              backgroundArgs.find("{wslscript}") != std::string::npos},
                                             state->scriptPath, toWslPath(state->scriptPath).value_or(state->scriptPath));
        else
            cmdLine = "\"" + program + "\" " + backgroundArgs + " \"" + state->scriptPath + "\"";

        // spawn must not lock m_mutex itself: start() already holds it here,
        // and std::mutex is not recursive (a nested lock self-deadlocks).
        HANDLE job = nullptr;
        spawn(id, state, cmdLine, extraEnv, workDir, std::move(logHandle), &job);
        if (job)
            m_jobs[id] = job;
        m_runs[id] = state;
        m_order.push_back(id);
        if (outId)
            *outId = id;
        return StartError::Ok;
    }

    // Terminate the run's whole process tree (design 6.2: the status row's
    // Enter while running).
    bool cancel(long long id)
    {
        std::shared_ptr<RunState> state = find(id);
        if (!state || !state->running)
            return false;
        state->cancelled = true;
        HANDLE job = nullptr;
        {
            std::lock_guard lock(m_mutex);
            const auto it = m_jobs.find(id);
            job = it == m_jobs.end() ? nullptr : it->second;
        }
        if (job)
            ::TerminateJobObject(job, 0xC000013AUL); // STATUS_CONTROL_C_EXIT
        return true;
    }

    void shutdown()
    {
        // The host ends the process about 2 s after the shutdown answer; the
        // job object (KILL_ON_JOB_CLOSE) kills the children when our handles
        // close at process exit. Join what finishes quickly.
        std::vector<std::shared_ptr<RunState>> running;
        for (const auto& state : all())
            if (state->running)
                running.push_back(state);
        for (const auto& state : running)
            state->running = false; // readers see EOF or the process dying
    }

    std::size_t runningCount() const
    {
        std::lock_guard lock(m_mutex);
        std::size_t n = 0;
        for (const auto& [id, state] : m_runs)
            if (state->running)
                ++n;
        return n;
    }

private:
    static HANDLE openAppend(const std::string& path)
    {
        return ::CreateFileW(winshun::wide(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    void spawn(long long id, const std::shared_ptr<RunState>& state, const std::string& cmdLine,
               const std::map<std::string, std::string>& extraEnv, const std::string& workDir,
               HANDLE logHandle, HANDLE* outJob)
    {
        *outJob = nullptr;
        SECURITY_ATTRIBUTES inherit {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
        ::CreatePipe(&outRead, &outWrite, &inherit, 0);
        ::CreatePipe(&errRead, &errWrite, &inherit, 0);
        ::SetHandleInformation(outWrite, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        ::SetHandleInformation(errWrite, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        HANDLE stdinNull = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         &inherit, OPEN_EXISTING, 0, nullptr); // design 6.1

        STARTUPINFOW si {};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = outWrite;
        si.hStdError = errWrite;
        si.hStdInput = stdinNull;
        PROCESS_INFORMATION pi {};
        std::wstring cmd = winshun::wide(cmdLine);
        std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
        mutableCmd.push_back(L'\0');
        const std::wstring envBlock = buildEnvironmentBlock(extraEnv);
        const std::wstring cwdWide = winshun::wide(workDir);
        // CREATE_UNICODE_ENVIRONMENT: the block is UTF-16. Windows also
        // requires the entries sorted case-insensitively by name and refuses
        // an unsorted block with ERROR_INVALID_PARAMETER (design 6.1).
        const BOOL spawned = ::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                              CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                              envBlock.empty() ? nullptr : (LPVOID)envBlock.c_str(),
                                              cwdWide.empty() ? nullptr : cwdWide.c_str(), &si, &pi);
        ::CloseHandle(outWrite);
        ::CloseHandle(errWrite);
        ::CloseHandle(stdinNull);
        if (!spawned) {
            ::CloseHandle(outRead);
            ::CloseHandle(errRead);
            finishSpawnless(state, logHandle, ::GetLastError());
            return;
        }

        // The child joins a nested job of our own (design 3 / §9 #1): the host
        // ends its plugin job on disable, and we can end the tree on cancel.
        HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
            if (!::AssignProcessToJobObject(job, pi.hProcess)) {
                ::CloseHandle(job);
                job = nullptr; // fall back: the tree is still the plugin's own job
            }
        }
        // The job handle goes back to start(), which holds m_mutex already -
        // spawn must not lock it again (std::mutex is not recursive).
        if (outJob)
            *outJob = job;
        ::ResumeThread(pi.hThread);
        ::CloseHandle(pi.hThread);

        std::thread([this, id, state, pi = pi.hProcess, outRead, errRead, logHandle]() {
            // stdout and stderr get their own reader so a child blocked on a
            // full stderr pipe cannot deadlock behind stdout.
            std::thread errReader([this, state, errRead, logHandle]() {
                reader(state, errRead, true, logHandle);
            });
            reader(state, outRead, false, logHandle);
            errReader.join();
            ::WaitForSingleObject(pi, INFINITE);
            DWORD code = 0;
            ::GetExitCodeProcess(pi, &code);
            ::CloseHandle(pi);
            {
                std::lock_guard lock(m_mutex);
                const auto it = m_jobs.find(id);
                if (it != m_jobs.end()) {
                    if (it->second)
                        ::CloseHandle(it->second);
                    m_jobs.erase(it);
                }
            }
            finish(state, logHandle, static_cast<long long>(code));
        }).detach();
    }

    // Reads one pipe to EOF, appending decoded lines to the state and the log.
    void reader(const std::shared_ptr<RunState>& state, HANDLE pipe, bool isErr, HANDLE logHandle)
    {
        std::string buffer;
        char chunk[16 * 1024];
        DWORD got = 0;
        for (;;) {
            if (!::ReadFile(pipe, chunk, sizeof chunk, &got, nullptr) || got == 0)
                break;
            buffer.append(chunk, got);
            const LineSplit split = splitCompleteLines(buffer);
            buffer = split.rest;
            if (!split.complete.empty())
                emit(state, split.complete, isErr, logHandle);
        }
        if (!buffer.empty())
            emit(state, buffer + "\n", isErr, logHandle); // a final line without \n
        ::CloseHandle(pipe);
    }

    void emit(const std::shared_ptr<RunState>& state, const std::string& completeLines, bool isErr,
              HANDLE logHandle)
    {
        const std::string decoded = decodeChunk(completeLines, oemToUtf8);
        const std::string cleaned = collapseCarriageReturns(stripAnsi(decoded));
        std::vector<OutputLine> fresh;
        std::size_t start = 0;
        for (;;) {
            const std::size_t nl = cleaned.find('\n', start);
            std::string line = cleaned.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            while (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (nl == std::string::npos && line.empty())
                break;
            fresh.push_back({line, isErr});
            if (nl == std::string::npos)
                break;
            start = nl + 1;
        }
        if (fresh.empty())
            return;
        {
            std::lock_guard lock(state->mutex);
            // Log the lines first: the second loop moves them into the ring
            // buffer, which leaves the texts empty.
            for (const OutputLine& line : fresh) {
                std::string row = line.text + "\n";
                if (state->logBytes + row.size() > kMaxLogBytes) {
                    if (!state->logTruncated) {
                        state->logTruncated = true;
                        const std::string note = "... log truncated ...\n";
                        writeLog(logHandle, note, state->logBytes);
                    }
                    continue;
                }
                writeLog(logHandle, row, state->logBytes);
            }
            for (OutputLine& line : fresh) {
                line.seq = state->nextSeq++;
                state->lines.push_back(std::move(line));
                if (state->lines.size() > kMaxKeptLines)
                    state->lines.pop_front();
            }
        }
        if (m_onUpdate)
            m_onUpdate();
    }

    static void writeLog(HANDLE logHandle, const std::string& row, long long& logBytes)
    {
        if (!logHandle)
            return;
        DWORD written = 0;
        ::WriteFile(logHandle, row.data(), static_cast<DWORD>(row.size()), &written, nullptr);
        logBytes += row.size();
    }

    void finish(const std::shared_ptr<RunState>& state, HANDLE logHandle, long long exitCode)
    {
        {
            std::lock_guard lock(state->mutex);
            state->exitCode = exitCode;
            state->endedTick = tickMs();
            state->running = false;
        }
        if (logHandle)
            ::CloseHandle(logHandle);
        // A background run's script goes away; a terminal run's stays for cleanup.
        if (!state->keepScript && !state->scriptPath.empty())
            ::DeleteFileW(winshun::wide(state->scriptPath).c_str());
        if (m_onUpdate)
            m_onUpdate();
        if (m_onFinish)
            m_onFinish(*state);
    }

    void finishSpawnless(const std::shared_ptr<RunState>& state, HANDLE logHandle, DWORD error)
    {
        {
            std::lock_guard lock(state->mutex);
            state->exitCode = static_cast<long long>(error);
            state->endedTick = tickMs();
            state->running = false;
            state->lines.push_back({"CreateProcess failed with error " + std::to_string(error), true});
        }
        if (logHandle)
            ::CloseHandle(logHandle);
        ::DeleteFileW(winshun::wide(state->scriptPath).c_str());
        if (m_onUpdate)
            m_onUpdate();
        if (m_onFinish)
            m_onFinish(*state);
    }

    std::string m_dataDir;
    std::function<void()> m_onUpdate;
    std::function<void(const RunState&)> m_onFinish;
    mutable std::mutex m_mutex;
    std::map<long long, std::shared_ptr<RunState>> m_runs;
    std::vector<long long> m_order;
    std::map<long long, HANDLE> m_jobs;
    long long m_counter = 0;
};

// ---- files in the runs folder (design 7.1) -----------------------------------------

inline void cleanupRuns(const std::string& dataDir)
{
    const std::string runsDir = dataDir + "\\runs";
    WIN32_FIND_DATAW found;
    HANDLE h = ::FindFirstFileW(winshun::wide(runsDir + "\\*").c_str(), &found);
    if (h == INVALID_HANDLE_VALUE)
        return;
    std::vector<std::pair<std::string, long long>> files;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        ULARGE_INTEGER t {found.ftLastWriteTime.dwLowDateTime, found.ftLastWriteTime.dwHighDateTime};
        // Windows FILETIME → Unix seconds.
        const long long unixTime = static_cast<long long>(t.QuadPart / 10000000ULL) - 11644473600LL;
        files.push_back({winshun::utf8(found.cFileName), unixTime});
    } while (::FindNextFileW(h, &found));
    ::FindClose(h);
    for (const std::string& name : expiredRunFiles(files, epochSeconds()))
        ::DeleteFileW(winshun::wide(runsDir + "\\" + name).c_str());
}

// ---- history file (design 7.1) -----------------------------------------------------

inline std::vector<HistoryEntry> loadHistory(const std::string& dataDir, bool* fileBad)
{
    *fileBad = false;
    const std::string path = dataDir + "\\history.json";
    HANDLE f = ::CreateFileW(winshun::wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return {};
    std::string bytes;
    char chunk[16 * 1024];
    DWORD got = 0;
    while (::ReadFile(f, chunk, sizeof chunk, &got, nullptr) && got > 0)
        bytes.append(chunk, got);
    ::CloseHandle(f);
    const auto parsed = winshun::Json::parse(bytes);
    if (!parsed || !parsed->isArray()) {
        if (!bytes.empty())
            *fileBad = true;
        return {};
    }
    std::vector<std::vector<HistoryBlob>> rows;
    for (const winshun::Json& entry : parsed->toArray()) {
        if (!entry.isObject())
            continue;
        const HistoryBlob shell {true, false, entry["shell"].isString(), 0, entry["shell"].toString()};
        const HistoryBlob command {true, false, entry["command"].isString(), 0, entry["command"].toString()};
        const HistoryBlob lastRun {true, entry["lastRun"].isNumber(), false, entry["lastRun"].toInt(), {}};
        const HistoryBlob count {true, entry["count"].isNumber(), false, entry["count"].toInt(), {}};
        const HistoryBlob lastExit {true, entry["lastExit"].isNumber(), false, entry["lastExit"].toInt(), {}};
        rows.push_back({shell, command, lastRun, count, lastExit});
    }
    return historyFromBlobs(rows);
}

inline bool saveHistory(const std::string& dataDir, const std::vector<HistoryEntry>& history)
{
    std::string out = "[\n";
    for (std::size_t i = 0; i < history.size(); ++i) {
        const HistoryEntry& e = history[i];
        out += "  {\"shell\":" + winshun::Json(e.shell).dump()
               + ",\"command\":" + winshun::Json(e.command).dump()
               + ",\"lastRun\":" + std::to_string(e.lastRun)
               + ",\"count\":" + std::to_string(e.count)
               + ",\"lastExit\":" + std::to_string(e.lastExit) + "}";
        if (i + 1 < history.size())
            out += ",";
        out += "\n";
    }
    out += "]\n";
    const std::string tmp = dataDir + "\\history.json.tmp";
    HANDLE f = ::CreateFileW(winshun::wide(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = ::WriteFile(f, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    ::CloseHandle(f);
    if (!ok)
        return false;
    ::MoveFileExW(winshun::wide(tmp).c_str(), winshun::wide(dataDir + "\\history.json").c_str(),
                  MOVEFILE_REPLACE_EXISTING);
    return true;
}

// ---- shell specs (design 4.1) -------------------------------------------------------

// The program and background arguments for one shell name. The script path
// reaches the command line through {script}/{wslscript} or as the last quoted
// argument (RunManager.start).
struct ShellSpec {
    std::string name;
    std::string program;
    std::string args; // may contain {script} or {wslscript}
    std::map<std::string, std::string> env;
    bool valid = false;
};

inline ShellSpec shellSpec(const Settings& st, const Detection& det, const std::string& shellName,
                           const std::string& workDir)
{
    ShellSpec s;
    s.name = shellName;
    const std::string name = lower(shellName);
    const auto programOf = [&](const std::string& n) {
        const auto it = det.programs.find(n);
        return it == det.programs.end() ? std::string() : it->second;
    };
    if (name == kCmd) {
        s.program = programOf(kCmd);
        s.args = "/d /c {script}";
    } else if (name == kPwsh || name == kPowerShell) {
        s.program = programOf(name);
        s.args = "-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File {script}";
    } else if (name == kBash || name == kMsys) {
        s.program = programOf(name);
        s.args = "-l {script}";
        s.env["CHERE_INVOKING"] = "1"; // a login shell keeps the starting folder
        if (name == kMsys)
            s.env["MSYSTEM"] = st.msysEnv;
    } else if (name == kWsl) {
        s.program = programOf(kWsl);
        s.args = "--cd \"" + workDir + "\" -e bash -l {wslscript}";
    } else {
        for (const CustomShell& c : det.customs) {
            if (c.name == shellName) {
                s.program = c.program;
                s.args = c.args;
            }
        }
    }
    s.valid = !s.program.empty() && fileExists(s.program);
    return s;
}

// ---- terminal opening (design 5.4) ---------------------------------------------------

// The mintty.exe next to a bash.exe (Git: <root>\usr\bin\mintty.exe).
inline std::string minttyFor(const std::string& bashExe)
{
    const std::size_t slash = bashExe.find_last_of("\\/");
    if (slash == std::string::npos)
        return {};
    const std::string binDir = bashExe.substr(0, slash); // ...\bin or ...\usr\bin
    const std::size_t up = binDir.find_last_of("\\/");
    const std::string root = up == std::string::npos ? binDir : binDir.substr(0, up);
    for (const std::string& candidate : {root + "\\usr\\bin\\mintty.exe", binDir + "\\mintty.exe"})
        if (fileExists(candidate))
            return candidate;
    return {};
}

// Ctrl+Enter: run the command in a visible terminal, window stays open.
// `empty` opens the shell's own prompt. Returns an error text, empty on success.
inline std::string openInTerminal(const Settings& st, const Detection& det,
                                  const std::string& shellName, const std::string& command,
                                  const std::string& runsDir, const std::string& workDir)
{
    const ShellSpec spec = shellSpec(st, det, shellName, workDir);
    if (!spec.valid)
        return "shell not found";

    const std::string name = lower(shellName);
    const bool shLike = name == kBash || name == kMsys || name == kWsl
                        || (name != kCmd && name != kPwsh && name != kPowerShell);
    std::string scriptPath, wslScriptPath;
    if (!command.empty()) {
        ScriptSpec spec2 = scriptFor(name, command);
        if (name != kCmd && name != kPwsh && name != kPowerShell)
            spec2.ext = ".sh";
        scriptPath = runsDir + "\\t" + std::to_string(tickMs()) + spec2.ext;
        writeScriptFile(scriptPath, scriptBytes(spec2));
        wslScriptPath = toWslPath(scriptPath).value_or(scriptPath);
    }

    std::string shellArgs; // after the terminal's front part
    if (command.empty()) {
        if (name == kCmd)
            shellArgs = "\"" + spec.program + "\" /d /k";
        else if (name == kPwsh || name == kPowerShell)
            shellArgs = "\"" + spec.program + "\" -NoLogo -NoExit";
        else if (name == kWsl)
            shellArgs = "\"" + spec.program + "\"";
        else if (name == kBash || name == kMsys)
            shellArgs = "\"" + spec.program + "\" -l";
        else
            shellArgs = customShellCommandLine({{}, spec.program, spec.args, ".sh", false}, "", "");
    } else if (name == kCmd) {
        shellArgs = "\"" + spec.program + "\" /d /k \"" + scriptPath + "\"";
    } else if (name == kPwsh || name == kPowerShell) {
        shellArgs = "\"" + spec.program + "\" -NoLogo -NoExit -ExecutionPolicy Bypass -File \""
                     + scriptPath + "\"";
    } else if (name == kWsl) {
        shellArgs = "\"" + spec.program + "\" --cd \"" + workDir + "\" -e bash -lc 'source \""
                     + wslScriptPath + "\"; exec bash -l'";
    } else {
        // bash / msys / custom sh-like: source the script, then hand over to an
        // interactive shell so the window stays (design 5.4).
        const std::string p = toMsysPath(scriptPath).value_or(scriptPath);
        shellArgs = "\"" + spec.program + "\" -l -c 'source \"" + p + "\"; exec bash -l'";
    }

    // The terminal itself.
    std::string cmdLine;
    const std::string wt = st.terminal == "wt" ? findOnPath(L"wt.exe") : std::string();
    const std::string mintty = st.terminal == "mintty" && shLike ? minttyFor(spec.program)
                                                                 : std::string();
    if (!wt.empty()) {
        // wt splits its command line on `;` even inside quotes, so every
        // semicolon (the `exec bash -l` handover, a `;` in a path) must be
        // escaped as `\;` or wt launches the tail as a second command and
        // fails with 0x80070002.
        const auto wtEscape = [](const std::string& s) {
            std::string out;
            out.reserve(s.size());
            for (const char c : s) {
                if (c == ';')
                    out += "\\;";
                else
                    out += c;
            }
            return out;
        };
        cmdLine = "\"" + wt + "\" -d \"" + wtEscape(workDir) + "\" " + wtEscape(shellArgs);
    } else if (!mintty.empty()) {
        cmdLine = "\"" + mintty + "\" --dir \"" + workDir + "\" " + shellArgs;
    } else {
        // conhost fallback: a new console hosts the same arguments (wt not
        // installed, mintty not applicable).
        cmdLine = "conhost.exe " + shellArgs;
    }

    const std::wstring envBlock = buildEnvironmentBlock(spec.env);
    const std::wstring cwd = winshun::wide(workDir);
    std::wstring cmd = winshun::wide(cmdLine);
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    STARTUPINFOW si {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi {};
    const BOOL ok = ::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                                     CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
                                     envBlock.empty() ? nullptr : (LPVOID)envBlock.c_str(),
                                     cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    if (!ok)
        return "CreateProcess failed (" + std::to_string(::GetLastError()) + ")";
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return {};
}

} // namespace shellplug
