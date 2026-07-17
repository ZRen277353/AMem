当前项目仅只有ui，QQ交流群:1065266874

[视频效果](https://www.bilibili.com/video/BV1KpWbzeEFU/)

[TG频道 内存库文件获取](https://t.me/AndroidMemX)


# Android Cheat Engine (ImGui版)

<div align="center">

**基于 ImGui + DirectX12 的跨平台 Android 内存修改工具**

[功能特性](#功能特性) • [快速开始](#快速开始) • [使用指南](#使用指南) 

</div>


## 📋 项目简介

这是一个功能强大的 Android 内存修改工具，类似于 PC 端的 Cheat Engine。通过 Socket 连接远程 Android 设备，提供了内存扫描、指针链分析、断点调试等专业功能。

### 主要特点

- 🎨 **现代化 UI** - 基于 Dear ImGui，支持中文界面
- 🔗 **指针链分析** - 可视化指针链树，支持复杂的指针路径分析
- 🐛 **内核级调试** - 硬件断点、内存断点支持
- 📡 **远程连接** - 通过 Socket 连接 Android 设备
- 💾 **崩溃保护** - 完整的异常捕获和 dump 生成
- 🤖 **AI 集成 (MCP)** - 内置 IPC Server + MCP 代理，支持 Claude Code 等 AI 客户端直接调用全部能力
- 📜 **Lua 脚本** - LuaJIT 脚本引擎，支持自动化操作


## ✨ 功能特性

### 1. 内存扫描 (ScanWindow)
- ✅ 精确数值扫描（1/2/4/8字节）
- ✅ 模糊扫描（变大/变小/未变化）
- ✅ 增量扫描（首次扫描 → 再次扫描）
- ✅ 扫描范围设置（所有内存/堆/栈/匿名）
- ✅ 实时进度显示
- ✅ 结果过滤和导出

  <img width="964" height="619" alt="搜索" src="https://github.com/user-attachments/assets/e569c833-1444-43f6-9937-87c557466abe" />


### 2. 内存查看器 (MemoryViewerWindow)
- ✅ 十六进制内存查看
- ✅ 内存编辑功能
- ✅ 支持跳转到指定地址
- ✅ 多种数据类型解析
<img width="1032" height="568" alt="内存查看" src="https://github.com/user-attachments/assets/a4e09eea-8745-4f32-9c93-3ee02e067185" />


### 3. 断点调试 (BreakpointWindow)
- ✅ 硬件断点（读/写/执行）
- ✅ 断点命中信息查看
- ✅ 断点暂停/恢复
- ✅ 反汇编显示（需要 Capstone）
<img width="967" height="603" alt="断点" src="https://github.com/user-attachments/assets/7fd1fe99-966b-4094-9cc5-87a6a5900c23" />
<img width="975" height="645" alt="反汇编" src="https://github.com/user-attachments/assets/5fd54c8b-9087-4246-b34e-e44c772b3aa8" />


### 4. 进程管理
- ✅ 进程列表查看
- ✅ 模块列表查看
- ✅ 进程附加/分离
- ✅ 模块基址查询
<img width="1111" height="705" alt="模块列表" src="https://github.com/user-attachments/assets/16e59225-3453-48a0-873b-4e8c48ccc4a2" />


### 5. 异常处理
- ✅ SEH 异常捕获
- ✅ C++ 标准异常捕获
- ✅ 信号处理
- ✅ 自动生成 .dmp 崩溃转储文件

### 6. MCP / AI 集成
- ✅ GUI 内嵌 HTTP IPC Server（`127.0.0.1:28100`）
- ✅ MCP Python 代理，支持 Claude Code / Claude Desktop 等 AI 客户端
- ✅ 通过 MCP 调用全部功能：进程管理、内存读写、扫描、断点
- ✅ `execute_lua` tool — AI 可直接编写并执行 Lua 脚本完成复杂自动化
- ✅ 零协议重复，所有请求复用 GUI 已有的 C++ 实现


## 🚀 快速开始

### 环境要求

#### 必需环境
- **操作系统**: Windows 10/11 (x64)
- **编译器**: Visual Studio 2022 (需支持 C++17)
- **CMake**: 3.16 或更高版本
- **DirectX**: DirectX 12 SDK

#### 可选依赖
- **Capstone**: 反汇编库（用于断点反汇编功能）
  ```bash
  # 使用 vcpkg 安装
  vcpkg install capstone:x64-windows
  ```
- **Keystone**: 汇编库（用于汇编指令转机器码）
  ```bash
  # 使用 vcpkg 安装
  vcpkg install keystone:x64-windows
  ```

### 编译步骤

#### 方法一：使用批处理脚本（推荐）

```bash
# 双击或运行
build.bat
```

#### 方法二：手动构建

```bash
# 1. 配置（使用 Clang + Ninja）
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=TRUE \
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang.exe" \
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang++.exe" \
  --no-warn-unused-cli -S . -B build -G Ninja

# 2. 编译项目
cmake --build build

# 3. 运行程序
.\bin\ImGuiProject.exe
```

#### 方法三：使用 Visual Studio

```bash
# 1. 用 Visual Studio 打开 CMakeLists.txt
# 2. 选择 x64-Release 配置
# 3. 点击"生成" → "生成解决方案"
# 4. 运行项目
```

### 输出文件

编译成功后，可执行文件位于：
```
build/Release/ImGuiProject.exe
```

## 📖 使用指南

### 1. 连接 Android 设备

1. 启动程序
2. 点击 **"服务器连接"** 按钮
3. 输入 Android 设备的 IP 地址和端口
4. 点击 **"连接"**

> ⚠️ **注意**: 需要在 Android 设备上运行相应的服务端程序

### 2. 附加进程

1. 连接成功后，点击 **"选择进程"**
2. 在进程列表中搜索或选择目标进程
3. 双击进程名称完成附加

### 3. 内存扫描

#### 精确数值扫描
1. 打开 **"数值扫描"** 窗口
2. 设置扫描类型（1/2/4/8字节）
3. 输入要搜索的数值
4. 点击 **"首次扫描"**
5. 修改游戏内数值后，输入新值
6. 点击 **"再次扫描"**
7. 重复步骤 5-6 直到找到目标地址

#### 模糊扫描
1. 不输入具体数值
2. 选择扫描条件（变大/变小/未变化）
3. 逐步缩小结果范围

### 4. 设置断点

1. 打开 **"断点管理"** 窗口
2. 输入断点地址
3. 选择断点类型：
   - **执行断点** (Execute)
   - **写入断点** (Write)
   - **读写断点** (Access)
4. 点击 **"设置断点"**
5. 触发断点后，查看命中信息


### 5. MCP (AI 集成) 使用

AMem 内置了 IPC HTTP Server，配合 `mcp/` 目录下的 Python MCP Server，可让 Claude Code 等 AI 客户端直接操控全部调试功能。

#### 架构

```
Claude Code ──stdio──> MCP Server (Python) ──HTTP──> AMem GUI (C++ IPC Server) ──TCP──> Android
```

#### 使用步骤

1. 启动 AMem GUI（IPC Server 自动监听 `127.0.0.1:28100`）
2. 安装 Python 依赖：
   ```bash
   pip install mcp
   ```
3. 在 Claude Code 的 MCP 配置中添加：
   ```json
   {
     "mcpServers": {
       "amem": {
         "command": "python",
         "args": ["mcp/server.py"]
       }
     }
   }
   ```
4. AI 即可调用 `list_processes`、`read_memory`、`scan_value`、`execute_lua` 等全部 tool

#### 可用 MCP Tools

| 分类 | Tools |
|------|-------|
| 状态 | `get_status`, `get_server_version`, `get_architecture`, `init_driver` |
| 进程 | `list_processes`, `open_process`, `list_modules`, `get_module_base` |
| 内存 | `read_memory`, `read_value`, `write_value`, `write_bytes` |
| 扫描 | `scan_set_range`, `scan_value`, `scan_next`, `scan_fuzzy`, `scan_hex`, `get_scan_count`, `get_scan_results`, `clear_scan` |
| 断点 | `set_breakpoint`, `remove_breakpoint`, `read_breakpoint_info`, `suspend_breakpoint`, `resume_breakpoint` |
| Lua | `execute_lua` — 在 GUI 内执行任意 Lua 脚本 |
| 辅助 | `resolve_offset_chain` |

#### 直接测试 IPC

```bash
curl -X POST http://127.0.0.1:28100 -d "{\"method\":\"get_status\"}"
```


## ⚙️ 构建配置

### CMake 选项

```cmake
# 选择渲染后端
option(USE_DX11 "Use DirectX 11 backend" OFF)
option(USE_DX12 "Use DirectX 12 backend" ON)

# Capstone 库路径
set(CAPSTONE_ROOT "C:/Program Files/capstone" CACHE PATH "Capstone installation directory")
```

### 编译宏定义

- `USE_DX12` - 使用 DirectX 12 渲染
- `HAVE_CAPSTONE` - 启用反汇编功能
- `HAVE_KEYSTONE` - 启用汇编功能
- `HAVE_LUAJIT` - 启用 LuaJIT 脚本引擎
- `IMGUI_DISABLE_DEBUG_TOOLS` - 禁用 ImGui 调试工具
- `DX12_ENABLE_DEBUG_LAYER` - 启用 D3D12 调试层（Debug 模式）


## 🔧 故障排除

### 编译问题

**问题**: CMake 找不到 Capstone
```
解决方案：
1. 安装 Capstone: vcpkg install capstone:x64-windows
2. 设置 CAPSTONE_ROOT 环境变量
3. 或者在 CMakeLists.txt 中修改路径
```

**问题**: DirectX 12 SDK 未找到
```
解决方案：
1. 安装 Windows SDK (最新版本)
2. 确保 Visual Studio 已安装 C++ 桌面开发组件
```

### 运行问题

**问题**: 程序启动后中文显示为方框
```
解决方案：
- 确保系统中存在中文字体（微软雅黑、宋体等）
- 检查 main.cpp 中字体加载路径是否正确
```

**问题**: 无法连接 Android 设备
```
解决方案：
1. 确认 Android 设备已运行服务端程序
2. 检查 IP 地址和端口是否正确
3. 检查防火墙设置
4. 确认设备在同一网络
```

**问题**: 程序崩溃
```
解决方案：
- 检查 CrashDumps 目录下的 .dmp 文件
- 使用 Visual Studio 打开 .dmp 文件分析崩溃原因
- 查看异常处理器输出的描述信息
```


## 🤝 贡献指南

欢迎提交 Issue 和 Pull Request！


## 🙏 致谢

- [Dear ImGui](https://github.com/ocornut/imgui) - 优秀的即时模式 GUI 库
- [Capstone](https://www.capstone-engine.org/) - 强大的反汇编引擎
- [Keystone](https://www.keystone-engine.org/) - 轻量级汇编引擎
- Cheat Engine - 灵感来源

<div align="center">

**⭐ 如果这个项目对你有帮助，请给一个 Star！**


</div>
