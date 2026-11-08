# 更新日志

本插件所有值得注意的变化。格式遵循
[Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本遵循
[Semantic Versioning](https://semver.org/lang/zh-CN/)。`plugin.json` 里的
`version` 是唯一事实来源；发版 tag 是 `v<version>`。英文版见
[CHANGELOG.en.md](CHANGELOG.en.md)。

## [Unreleased]

## [0.2.2] - 2026-11-08

### Changed

- 清单对齐收录要求：`description` 改双语对象、补 `homepage`；README 的
  「分发与版本」写明 Release 由 CI 在 tag 提交上构建、条目哈希的来源。

## [0.2.1] - 2026-11-08

### Fixed

- 清单声明 `minWinShun: 0.22.0` 并在 README 写明：本插件是原生 exe 包，插件库
  按「未审核的原生插件」收录，旧版宿主没有这类包的安装风险确认。

## [0.2.0] - 2026-11-08

### Added

- 以管理员执行（cmd 和 PowerShell 7 / 5.1）：命令页运行行多 **Ctrl+Shift+Enter**
  动作、下方多一条独立「以管理员执行」行（Enter 触发），历史行同样带管理员动作。
  UAC 确认对象固定是系统的 `System32\cmd.exe`，命令文本写在脚本文件里、不进
  命令行；同意后打开一个**可见的管理员终端窗口**，输出在窗口里、有真实输入、
  跑完窗口留着，交互式命令直接操作。结果不返回 Win顺：没有结果页和日志，插件
  只发通知报告打开/拒绝/失败；窗口归桌面所有，Win顺 退出不影响它。`terminal=wt`
  时走二段式：提权 cmd 拉起 wt.exe 新窗口后即退出，管理员窗口落在 Windows
  Terminal（外层黑框闪一下）；wt 缺失或设置不是 wt 时由提权 cmd 自开控制台。
  新设置 `adminTerminal`（`auto` 默认跟随 `terminal` / `wt` 永远二段式 /
  `conhost` 永远直开）决定管理员窗口宿主，只管管理员窗口。
  UAC 流程一次一条（窗口可并存）；拒绝通知「UAC 已拒绝，没有执行」，提权失败
  通知「无法提权（错误 N）」，两者都不进历史。新设置 `adminMode`（`ask` 默认 /
  `off` 关闭入口）。Git Bash、MSYS2、WSL 和自定义 shell 无管理员入口。
- 命令页独立行「在终端执行：<命令>」（Enter 直达，所有 shell 都有）：与
  Ctrl+Enter 同一能力——终端窗口里跑命令、窗口留存、结果不进列表。

### Changed

- 同步主仓库最新插件 SDK（`winshun.h`）：`Item::withDetails()` 与
  `details()` 覆写（内容详情与预览，GitHub #202）、`serve(in, out)` 插件
  宿主接入、`appearance()` 新增 uiStyle/nativeLook 外观字段。均为可选
  能力，本插件暂未使用；线上协议没有变化，旧版 Win顺 照常可用。
- 普通后台路径的内存与输出加固（插件常驻不退出，防宿主因内存或超长输出行
  击杀）：内存里只保留最近 50 个已结束的运行；环形缓冲单行超 4 KB 截断并注明
  （日志保留整行）；无换行的超长输出读缓冲封顶 1 MB，超限部分直送日志、列表
  只留头部。

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

### 真机与集成验证里修掉的问题

- Unicode 环境块未按变量名排序导致 `CreateProcessW` 报 87。
- 传 UTF-16 环境块缺 `CREATE_UNICODE_ENVIRONMENT` 标志（后台和终端两处）。
- `spawn()` 在 `start()` 已持有 `m_mutex` 时再次加锁导致协议线程自死锁，
  改为 job 句柄回传登记。
- 结果页操作行排在输出行后面，长输出要翻到底才够得到；移到状态行后面。
- 日志文件只剩换行符：输出行先被移进环形缓冲、后从移空的对象写日志；
  改为先写日志再移动。

[Unreleased]: https://github.com/ra9fael/winshun-shell/compare/v0.2.2...HEAD
[0.2.2]: https://github.com/ra9fael/winshun-shell/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/ra9fael/winshun-shell/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/ra9fael/winshun-shell/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/ra9fael/winshun-shell/releases/tag/v0.1.0
