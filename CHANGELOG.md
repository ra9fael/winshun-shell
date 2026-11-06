# 更新日志

本插件所有值得注意的变化。格式遵循
[Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本遵循
[Semantic Versioning](https://semver.org/lang/zh-CN/)。`plugin.json` 里的
`version` 是唯一事实来源；发版 tag 是 `v<version>`。英文版见
[CHANGELOG.en.md](CHANGELOG.en.md)。

## [Unreleased]

## [0.1.0] - 2026-11-06

首个版本。

### Added

- 6 个关键词，一个 shell 一个：`>`（默认 shell）、`cmd`、`pwsh`、`bash`、
  `msys`、`wsl`。`>` 支持 `@` 列出 shell、`@名字 命令` 用自定义 shell 执行。
- 后台执行：命令写成脚本文件交给 shell（不经过命令行转义），子进程在插件的
  作业里，取消时进程树整体结束；同时最多 4 个并发。
- 结果页：`setQuery` 后带 `rerun` 实时刷新，stdout / stderr 按时间顺序合并、
  分别标图标，去掉 ANSI 颜色码和 `\r` 进度条，只显示最后 `outputLines` 行，
  完整输出写日志。
- 剪贴板执行：加空格后出现「执行剪贴板内容」行，默认两次确认；危险规则
  （下载即执行、删除格式化、改系统、不可见字符）只警告不拦截，不可见字符
  显示成 `<U+202E>`。
- Ctrl+Enter 在终端里执行（wt / conhost / mintty），执行完不关窗口；交互式
  命令和需要管理员权限的命令走这条路，插件不弹 UAC。
- Shift+Enter 把命令填回搜索框改写。
- 历史：JSONL 存在 `WS_DATA_DIR`，最多 200 条、合并重复，`saveHistory` 可关，
  单条删除用 `deleteHistory` 动作；剪贴板执行的默认不进历史。
- 通知：运行超过 `notifyAfterSec` 秒且搜索框收起时发系统通知，正文只写退出码
  和用时，点通知回到结果页。
- 自定义 shell：`customShells` 设置，`名字=程序 参数…`，`{script}` 占位符，
  `;ext=` 指定扩展名。
- 输出解码：逐行先试 UTF-8，失败退回 OEM 代码页，`wsl.exe` 的 UTF-16LE 报错
  也能解。

### 集成验证里修掉的问题

- Unicode 环境块未按变量名排序导致 `CreateProcessW` 报 87。
- 传 UTF-16 环境块缺 `CREATE_UNICODE_ENVIRONMENT` 标志（后台和终端两处）。
- `spawn()` 在 `start()` 已持有 `m_mutex` 时再次加锁导致协议线程自死锁，
  改为 job 句柄回传登记。

[Unreleased]: https://github.com/ra9fael/winshun-shell/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/ra9fael/winshun-shell/releases/tag/v0.1.0
