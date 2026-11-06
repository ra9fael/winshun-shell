# 参与贡献 ra9fael.shell

欢迎提 bug、想法和 PR。英文版见 [CONTRIBUTING.en.md](CONTRIBUTING.en.md)。

## 报告 bug

开 issue 时带上：

- 插件版本（Win顺 → 设置 → 插件 → 已安装）和 Win顺 版本，
- Windows 版本，装了哪些 shell（cmd / pwsh / Git Bash / MSYS2 / WSL），
- 你输入了什么、期望什么、实际发生了什么，
- Win顺 消息记录里插件的消息（如有）。

发帖前删掉隐私内容（带用户名的路径、主机名、IP、环境变量值）。

## 开发环境

- Windows 10/11 x64。
- CMake 3.21+，C++17 编译器：Visual Studio 2022 及以上（MSVC）或
  MinGW-w64 GCC 13+。
- Python 3.10+（只用于 `tools/package.py`，标准库即可）。
- Win顺 本体，用来试插件。

```powershell
cmake -S . -B build -A x64                 # MSVC；MinGW 用 -G "MinGW Makefiles"
cmake --build build --config Release       # 在 plugin.json 旁边生成 shell.exe
ctest --test-dir build -C Release --output-on-failure
```

要在 Win顺 里跑你的构建，把仓库用 junction 链接进 Win顺 的插件目录（在设置页
里添加文件夹是复制，后续构建到不了 Win顺）：

```powershell
New-Item -ItemType Junction -Path "$env:LOCALAPPDATA\WinShun\plugins\ra9fael.shell" -Target (Get-Location)
```

然后在 **设置 → 插件 → 已安装** 里启用。Win顺 拒绝用安装包覆盖 junction，
所以装同 id 的包之前先解开链接。**不要在设置页里卸载链接的插件**：卸载会
递归删除目录且不检查 junction，会把你克隆里的文件删掉。用
`cmd /c rmdir "$env:LOCALAPPDATA\WinShun\plugins\ra9fael.shell"` 只删链接
本身。正在运行的 `shell.exe` 是锁定的；重建前把它改名（比如
`shell.exe.old`），Win顺 会用新 exe 重启插件。同 id 的非链接安装副本要先移除。

`winshun.h` 是 Win顺 SDK 头文件（`WinShun/src/plugins-sdk/winshun.h`）的
逐字节复制，不要在这里改它；要更新就从上游单独提交。

## 测试

- `ctest` 跑 `shell_tests`：只测 `logic.h` 里的纯函数（输入解析、自定义
  shell 解析、脚本生成、WSL 路径转换、输出解码、危险规则、历史、清理），
  不起进程、不依赖系统状态，任何 C++17 编译器都能跑。
- 集成测试用真 shell 驱动 stdio 协议（脚本见设计文档 `shell-plugin-design.md`
  的测试计划一节）。改动 runner、shell 检测或协议胶水后，至少跑一遍
  cmd / pwsh / bash 的执行、取消和剪贴板确认场景，结果补进设计文档第 9 节。

## Pull request

1. Fork，从 `main` 拉分支。
2. 一个 PR 一个主题。遵循现有风格：C++17，不用第三方库，不用运行时 DLL，
   代码和注释一律英文。文档双语（见下文「文档语言」）。plugin.json 里
   用户可见的字符串保持 zh / en 双份。
3. 逻辑改动要加或更新单元测试。集成行为改动要在真 shell 上验证过。
4. 开 PR 前在 Win顺 里至少跑一遍：手动输入执行、剪贴板执行、Ctrl+Enter。
5. 行为、设置或权限变了，要同时更新 `README.md` 和 `README.en.md`，并在
   `CHANGELOG.md` 和 `CHANGELOG.en.md` 的 `## [Unreleased]` 下各加一条。
6. 提交信息用 Angular 约定
   （[Conventional Commits](https://www.conventionalcommits.org/zh-Hans/)）：
   `type(scope): 祈使句摘要`，空一行，然后是正文。type 取
   `feat` / `fix` / `docs` / `style` / `refactor` / `perf` / `test` /
   `build` / `ci` / `chore` / `revert`；scope 用模块名：`logic`、`shell`
   （检测和启动参数）、`runner`、`clipboard`、`history`、`manifest`、
   `readme`、`design`。摘要小写开头、结尾不加句号。
   破坏性改动 type 后加 `!`，如 `feat(logic)!: …`。

不要提交 `shell.exe`、`build/` 或 `dist/`；`.gitignore` 已经排除。

### 权限

在 `plugin.json` 里加权限或扩大权限范围，PR 里要给理由，README 的权限一节
也要写。Win顺 会在权限变化时让用户重新确认，所以保持最小。

## 文档语言

Win顺 以中文为主、也自带英文 UI，用户文档按同样的方式拆分。设计文档
（`shell-plugin-design.md` / `shell-plugin-design.en.md`）第 12.3 节定了规则：

| 文件 | 语言 | 角色 |
|---|---|---|
| `README.md` / `CHANGELOG.md` / `CONTRIBUTING.md` | 简体中文 | 默认 |
| `README.en.md` / `CHANGELOG.en.md` / `CONTRIBUTING.en.md` | English | 对应英文版 |

每份文档标题下面第一行是语言切换，结尾也有一次，两个方向都能跳。
**两份保持同步**：结构、设置、权限、安装步骤的改动必须在同一个 PR 里应用到
两份。代码标识符、设置键、命令语法、JSON 和路径在两份里都保持英文。英文版
用 Win顺 的真实英文 UI 标签（`Settings → Plugins → Installed → Add plugin →
Archive (.zip / .wsplugin)`），中文版用中文标签。

`LICENSE` 只有英文一份。

## 发版（维护者）

`plugin.json` 里的 `version` 是唯一事实来源，版本遵循
[Semantic Versioning](https://semver.org/lang/zh-CN/)。

1. 改 `plugin.json` 的 `version`，以及代码里所有引用它的地方。
2. 把 `CHANGELOG.md`（和 `CHANGELOG.en.md`）的 `## [Unreleased]` 改名成
   `## [x.y.z] - YYYY-MM-DD`，上面再放一个新的空 `## [Unreleased]`，更新
   两个文件末尾的链接。
3. 本地检查：
   ```powershell
   cmake --build build --config Release
   ctest --test-dir build -C Release --output-on-failure
   python tools/package.py --tag vx.y.z
   ```
   然后把 `dist/ra9fael.shell-x.y.z.wsplugin` 以压缩包方式装进 Win顺试一遍。
4. 提交（`chore(release): vx.y.z`），打 tag 并推送：
   ```powershell
   git tag -a vx.y.z -m "vx.y.z"
   git push origin main vx.y.z
   ```
5. 在 GitHub Release 上传 `.wsplugin` 和它的 `.sha256`。不要替换已发布
   Release 的资产；要改就发一个新的补丁版本。

## 许可证

参与贡献即表示你同意你的贡献按本仓库的 MIT 许可证授权。
