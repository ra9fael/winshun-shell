# Changelog

All notable changes to this plugin. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). The version in `plugin.json` is the
single source of truth; release tags are `v<version>`. The Chinese version
lives in [CHANGELOG.md](CHANGELOG.md).

## [Unreleased]

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

### Fixed during integration testing

- The Unicode environment block was not sorted by variable name, which made
  `CreateProcessW` fail with 87.
- The `CREATE_UNICODE_ENVIRONMENT` flag was missing when passing a UTF-16
  environment block (both the background and the terminal paths).
- `spawn()` locked `m_mutex` while `start()` already held it, self-deadlocking
  the protocol thread; the job handle is now handed back and registered by
  the caller.
- `emit()` moved the output lines before writing the log, leaving the log
  files with bare newlines; the log is now written before the move.
- The result-page action rows moved to just after the status row, before the
  output lines, so long output does not bury them.

[Unreleased]: https://github.com/ra9fael/winshun-shell/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/ra9fael/winshun-shell/releases/tag/v0.1.0
