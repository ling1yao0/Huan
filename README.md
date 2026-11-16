# 🎯 Huan - 极简、纯粹的轻量级多环鼠标工作台

> **Huan** 是一个基于纯 C++ (Qt6) 构建的 Windows 鼠标轮盘与自动化效率工具。
> 没有几百兆的内存占用，没有强制登录，没有订阅制。只有一个极致轻量、响应如飞的效率之环。

<p align="center">
  <img src="https://img.shields.io/badge/Language-C%2B%2B20-blue.svg" alt="C++20">
  <img src="https://img.shields.io/badge/Framework-Qt6-green.svg" alt="Qt6">
  <img src="https://img.shields.io/badge/Platform-Windows-lightgrey.svg" alt="Windows">
  <img src="https://img.shields.io/badge/License-GPLv3-orange.svg" alt="GPLv3">
</p>

## 💡 为什么叫 Huan？（环的哲学）

在重度使用市面上各种效率工具后，我发现它们要么越来越臃肿，要么强制开启“云服务”订阅。于是，我决定自己动手写一个。

**Huan（环）** 贯穿了整个软件的设计哲学：
- **视觉之环**：从 8 槽的基础环，到 16、32 槽的无限拓展，甚至支持超外围的隐藏触发环。
- **场景之环**：全局环、Chrome 环、VSCode 环。不同应用独立配置，互不干扰。
- **工作流之环**：多页动作页 + 多步流水线，让你的高频操作形成一个完美的自动化闭环。

## ✨ 核心特性

- ⚡ **原生极速**：底层基于 Win32 API 低级钩子拦截，毫秒级响应。
- 🎯 **多环轮盘**：支持 8+16+32 扇区嵌套，完美匹配你的肌肉记忆；支持外圈隐藏动作，盲操更顺手。
- 🗂️ **场景隔离**：根据前台窗口进程名，自动匹配专属的轮盘与动作页。新场景自动继承全局配置。
- 🛠️ **自动化流水线**：内置多步流水线引擎、按键宏录制、变量解析（如 `{clipboard}`、`{time}`）。
- 🐍 **外部脚本支持**：无缝调用 Python / PowerShell / Batch 脚本，极客的扩展无边无际。
- 🔒 **完全本地化**：数据保存在本地 SQLite，支持一键导入/导出 JSON 备份。不上传任何隐私数据。

## 🚀 快速开始

### 方式一：直接下载安装包
前往 [Releases] 页面，下载最新版本的安装包

### 方式二：从源码编译 (开发者)
本项目使用 CMake 构建，依赖于 Qt 6.5+ 和 MSVC 2022。

```bash
# 克隆代码
git clone https://github.com/ling1yao0/Huan.git
cd Huan

# 配置 CMake (请确保安装了 Qt 6)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:\Qt\6.x.x\msvc2022_64"

# 编译
cmake --build build --config Release