# Huan

Windows 轮盘（径向菜单）启动器：全局鼠标/键盘触发，槽位动作由 SQLite 配置，支持按键、启动程序、剪贴板转换、外部脚本、变量、多步流水线。

## 构建

环境：Windows 10/11、Visual Studio 2022、Qt 6.12（msvc2022_64）、CMake 3.21+。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.12.0/msvc2022_64"
cmake --build build --config Debug
```

产物：`build/Debug/mousemouse.exe`。首次运行会在 exe 同目录创建 `data.db`。

## 触发方式

托盘图标右键 →「打开设置」→「触发方式」页。

| 触发 | 行为 |
| --- | --- |
| 鼠标触发键 | 右键 / 中键 / 侧键1(X1) / 侧键2(X2)，按住拖动超过阈值（默认 3px）呼出轮盘 |
| 短按 | 没拖动就松开时，把这次点击原样还给系统（右键菜单、中键滚动、侧键前进后退都正常） |
| 长按 | 超过 220ms 仍没移动，补发按下事件并放弃拦截，交还给原程序 |
| 松开执行 | 轮盘出现后移动鼠标选择扇区，松开触发键执行 |
| `Esc` | 取消轮盘；鼠标触发时会吞掉对应的抬起事件 |
| 键盘热键 | 按住热键呼出轮盘，移动鼠标选择，松开热键执行；`Esc` 取消 |
| 高级鼠标触发 | 中键 / X1 / X2 可以直接绑定一个动作，不经过轮盘 |

轮盘出现后 3 秒不动会自动隐藏，时间可配置。

## 动作库与轮盘绑定

设置窗口：

- **轮盘绑定**：选择场景（`global` 或前台进程名，例如 `Code.exe`）。左侧是 8 个槽位，右侧常驻动作库；从右侧拖一个动作到左侧槽位，松开立即保存。未独立绑定的槽位会显示「（继承 global）」。选中槽位点「解绑选中槽位」可删除该场景的独立绑定。
- **动作库**：全局动作列表，新增 / 编辑 / 删除。同一个动作可以被多个场景、多个槽位复用；删除动作会同时解绑所有引用它的槽位。
- **从全局复制**：把 `global` 的绑定物理复制到当前场景，之后可独立修改。

旧版 `actions` 表中的数据会在首次启动时自动迁移到 `actions_lib` + `wheel_bindings`。

## 动作页

托盘菜单「动作页」，或「轮盘绑定」页的「打开动作页」按钮，会以网格悬浮面板显示动作库。顶部支持搜索，点击按钮执行动作；执行前会隐藏面板并把焦点还给最近一个外部窗口。

## 动作类型

| 类型 | 参数 |
| --- | --- |
| 0 按键组合 | `Ctrl+Shift+Esc`、`Alt+F4`、`Win+Shift+S`、`Ctrl+Left`、`MediaPlayPause`、`Ctrl++` |
| 1 运行程序/打开链接 | `C:\Windows\notepad.exe`、`https://www.baidu.com`、`exit`（退出本程序） |
| 2 剪贴板转换 | `upper` / `lower` / `title` / `trim` / `reverse`，转换选中文字后粘贴回去 |
| 3 外部脚本 | 脚本路径或完整命令，见下 |
| 4 多步流水线 | JSON 数组，见下 |
| 5 输入文本 | Unicode 文本，支持变量，不受输入法影响 |

按键写法：

- 修饰键：`Ctrl` / `Shift` / `Alt` / `Win`（也支持 `Control`、`Meta`、`RCtrl`、`RAlt`）。
- 字母数字、`F1`–`F24`、`Home/End/PgUp/PgDn/Ins/Del`、方向键、小键盘（`Num0`–`Num9`、`NumpadEnter`）、多媒体键、浏览器键、常见符号。
- `Ctrl++` 或 `Ctrl+Plus` 表示 Ctrl + 加号。
- `Ctrl+Alt+Del` 是 Windows 安全注意序列，**任何普通程序都无法模拟**，保存时会提示。

### 外部脚本契约

- 支持 `.py` / `.pyw` / `.js` / `.mjs` / `.cjs` / `.ps1` / `.bat` / `.cmd` / `.vbs` / `.exe`，会自动选择解释器；也可以直接写 `python xxx.py`、`node xxx.js`。
- **stdin**：当前选中的文字（UTF-8；没有选中就是空）。
- **stdout**：脚本处理结果（UTF-8）。非空时会写回剪贴板并 Ctrl+V 粘贴，然后恢复原来的剪贴板文本。
- **stderr**：退出码非 0 或崩溃时显示到托盘通知。
- 超时默认 8 秒，超时会用 `taskkill /T /F` 结束整棵进程树；可在设置页调整。
- 环境变量会注入 `PYTHONIOENCODING=utf-8`、`PYTHONUTF8=1`、`PYTHONUNBUFFERED=1`。
- 脚本输出如果不需要粘贴，保持 stdout 为空、退出码 0 即可。

### 多步流水线

参数是 JSON 数组。`type` 可以是数字 0–5，也可以是 `key` / `launch` / `clipboard` / `script` / `pipeline` / `text`：

```json
[
  {"type": 0, "payload": "Ctrl+C"},
  {"wait": 120},
  {"type": 3, "payload": "python \"C:\\scripts\\clean.py\""},
  {"type": 5, "payload": "{clipboard}"}
]
```

`wait`（或 `delay`）表示等待毫秒数。每一步结束（脚本会等剪贴板恢复完成）后才执行下一步。

## 变量

可用在所有动作参数里：

| 变量 | 含义 |
| --- | --- |
| `{clipboard}` | 当前剪贴板文字 |
| `{selected}` / `{selection}` | 当前选中的文字（会模拟 Ctrl+C 抓取） |
| `{scene}` / `{app}` / `{process}` | 当前前台进程名，例如 `Code.exe` |
| `{date}` / `{time}` / `{datetime}` / `{timestamp}` | 日期时间 |
| `{uuid}` | 随机 UUID |
| `{newline}` / `{tab}` | 换行 / 制表符 |
| `{env:NAME}` | 环境变量 |
| `%NAME%` | Windows 风格环境变量（兼容旧写法） |

未知变量会原样保留。

## 场景与数据库

`data.db` 主要表：

- `actions_lib(id, name, type, payload)`：动作库。
- `wheel_bindings(slot, scene, action_id)`：场景槽位到动作库的绑定。
- `settings(key, value)`：触发方式、阈值、超时。
- `mouse_bindings(button, name, type, payload)`：中键 / X1 / X2 直接绑定。
- `actions(slot, scene, name, type, payload)`：旧版表，仅用于首次启动迁移和兼容回退。

某个场景没有独立绑定的槽位会回退到 `global`；旧版空行在启动时会被清理。迁移标记保存在 `settings.schema.actions_v2`，迁移成功后会停用旧表回退，避免“解绑后又从旧表复活”。

场景继承：在「轮盘绑定」页选中非 `global` 场景后，点「从全局复制」，会把 `global` 的绑定**物理复制**到当前场景，之后可独立修改。

## 系统

「系统」页可以勾选「开机自动启动（当前用户，无需管理员权限）」。它写入：

```text
HKCU\Software\Microsoft\Windows\CurrentVersion\Run\ItervanceMouse
```

不会触发 UAC。移动程序位置后，重新勾选保存即可更新启动路径。

## 测试

```powershell
$env:PATH = "C:\Qt\6.12.0\msvc2022_64\bin;$env:PATH"
.\build\Debug\mousemouse_tests.exe          # 组合键解析 + 变量替换
.\build\Debug\mousemouse_script_tests.exe   # 脚本解析 / UTF-8 / 超时 / 退出码 / .bat / .ps1
.\build\Debug\mousemouse_db_tests.exe       # 动作库 / 绑定 / 迁移 / 场景回退
```

手动诊断（默认不构建）：

```powershell
cmake --build build --config Debug --target mousemouse_input_tests
.\build\Debug\mousemouse_input_tests.exe
```

需要测试窗口能拿到前台焦点，否则会自动跳过，避免把按键打进其他程序。

## 已知限制

- 无法模拟 `Ctrl+Alt+Del`；目标窗口以管理员权限运行时，本程序也需要以管理员权限运行，否则 `SendInput` 会返回 `ERROR_ACCESS_DENIED`。
- 剪贴板只处理文本格式；抓取选中文字/转换时会覆盖图片等其他格式（原文本会恢复）。
- 脚本按 UTF-8 处理 stdout/stderr；非 UTF-8 输出可能乱码。
- 多环（8+16+32）、独立槽数、级联菜单、场景编辑器尚未实现。
- 多屏不同 DPI 的坐标换算已做物理→Qt 逻辑坐标映射，但极端缩放组合仍建议实机确认。
