# Changelog

All notable changes to this plugin. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). The version in `plugin.json` is the
single source of truth; release tags are `v<version>`. The Chinese version
lives in [CHANGELOG.md](CHANGELOG.md).

## [Unreleased]

## [0.2.2] - 2026-11-08

### Changed

- Manifest aligned with the index requirements: `description` became a
  bilingual object and `homepage` was added; the README's "Distribution and
  versions" now documents that Releases are built by CI at the tag commit and
  where the index entry's hash comes from.

## [0.2.1] - 2026-11-08

### Fixed

- The manifest declares `minWinShun: 0.22.0` and the README says why: this is
  a native exe package, admitted by the index as an unreviewed native plugin;
  older hosts have no install-risk confirmation for such packages.

## [0.2.0] - 2026-11-08

### Added

- Run as administrator (cmd and PowerShell 7 / 5.1): the command page's run
  row gains a **Ctrl+Shift+Enter** action and a standalone "Run as
  administrator" row below it (Enter triggers), and history rows gain the
  same admin action. The UAC consent object is always the system's
  `System32\cmd.exe`; the command text lives in a script file, never on the
  command line. After consent a **visible admin terminal window** opens: the
  output is in the window, stdin is real, the window stays after the command,
  and interactive commands are operated right there. Results do not return to
  WinShun — no result page and no log; the plugin only notifies opened /
  declined / failed. The window belongs to the desktop, so quitting WinShun
  does not affect it. With `terminal=wt` a two-stage start puts the admin
  window inside Windows Terminal (the elevated cmd launches wt.exe and exits;
  the outer black box flashes once); without wt, or for other `terminal`
  values, the elevated cmd hosts the payload in its own console. New setting
  `adminTerminal` (`auto` default follows `terminal` / `wt` always two-stage /
  `conhost` always direct) chooses the admin window's host and governs admin
  windows only.
  One UAC flow
  at a time (windows may coexist); a declined UAC notifies "UAC declined;
  nothing ran" and a failed elevation "Cannot elevate (error N)" — neither
  reaches history. New setting `adminMode` (`ask` default / `off` removes
  every admin entry). Git Bash, MSYS2, WSL and custom shells have no admin
  entry.
- A standalone command-page row "Run in a terminal: <command>" (Enter
  triggers, every shell): the same capability as Ctrl+Enter — the command
  runs in a terminal window that stays, results do not return to the list.

### Changed

- Synced the latest plugin SDK (`winshun.h`) from the main repo:
  `Item::withDetails()` and the `details()` override (content details and
  the preview, GitHub #202), `serve(in, out)` for the plugin host, and the
  uiStyle/nativeLook keys in `appearance()`. All are optional capabilities
  the plugin does not use yet; the wire protocol is unchanged and older
  WinShun versions keep working.
- Memory and output hardening for the normal background path (the plugin is
  resident and never idles out; this keeps the host from killing it over
  memory or an over-long output line): only the newest 50 finished runs stay
  in RAM; a ring-buffer line over 4 KB is truncated and marked (the log keeps
  it whole); a newline-less output caps the read buffer at 1 MB, streaming
  the excess straight to the log while the list keeps only the head.

## [0.1.0] - 2026-11-06

First release.

### Added

- Six keywords, one per shell: `>` (default shell), `cmd`, `pwsh`, `bash`,
  `msys`, `wsl`. `>` supports `@` to list shells and `@name command` to run
  with a custom shell.
- Background execution: every command is written to a script file handed to
  the shell (no command-line escaping to break), children run inside the
  plugin's job, cancelling kills the whole process tree; at most 4 concurrent
  runs.
- Result pages: after `setQuery` the answer carries `rerun` and refreshes
  live; stdout / stderr are interleaved in time order with separate icons,
  ANSI colour codes and `\r` progress bars are stripped, only the last
  `outputLines` lines are shown, and the full output goes to a log file.
- Clipboard execution: after the keyword plus a space a "run clipboard
  content" row appears, confirmed twice by default; danger rules
  (download-and-run, deletion and formatting, system changes, invisible
  characters) only warn, never block, and invisible characters render as
  `<U+202E>`.
- Ctrl+Enter runs in a terminal (wt / conhost / mintty) and keeps the window
  open; interactive commands and commands needing admin rights go this way,
  with no UAC prompt from the plugin.
- Shift+Enter puts the command back into the search box for editing.
- History: JSONL in `WS_DATA_DIR`, 200 entries max with duplicates merged,
  `saveHistory` turns it off, `deleteHistory` removes a single entry;
  clipboard runs are not recorded by default.
- Notifications: a system notification after `notifyAfterSec` seconds with the
  search box collapsed, the body naming only the exit code and duration,
  clicking it returns to the result page.
- Custom shells: the `customShells` setting, `name=program args…`, the
  `{script}` placeholder, `;ext=` for the extension.
- Output decoding: each line is tried as UTF-8 first, falls back to the OEM
  code page, and `wsl.exe`'s UTF-16LE errors decode as well.

### Fixed during on-machine and integration testing

- The Unicode environment block was not sorted by variable name, which made
  `CreateProcessW` fail with 87.
- The `CREATE_UNICODE_ENVIRONMENT` flag was missing when passing a UTF-16
  environment block (both the background and the terminal paths).
- `spawn()` locked `m_mutex` while `start()` already held it, self-deadlocking
  the protocol thread; the job handle is now handed back and registered by
  the caller.
- Result-page actions sat behind the output lines, unreachable at the bottom
  of long output; they now come right after the status row.
- Log files held nothing but newlines: output lines were moved into the ring
  buffer before the log was written from the moved-from object; the log is
  written first now.

[Unreleased]: https://github.com/ra9fael/winshun-shell/compare/v0.2.2...HEAD
[0.2.2]: https://github.com/ra9fael/winshun-shell/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/ra9fael/winshun-shell/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/ra9fael/winshun-shell/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/ra9fael/winshun-shell/releases/tag/v0.1.0
