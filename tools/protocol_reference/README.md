# Android 协议排障工具

本目录不属于 AMem 产品运行时，也不参与内置 Agent、GUI 或 IPC 构建。
它保留一个仅依赖 Python 标准库的旧 wire-protocol 探针，供开发者手工排障。

## `amem_client.py`

按 `socket/client.hpp` 与部分 `socket/*Commands.cpp` 命令布局直接连接 Android
服务端，可跳过 GUI 做协议调试和调研。C++ socket command 层才是当前协议事实来源；
新增或修改命令时不能只更新本脚本。

典型用途：
- 协议调试、wire-level 抓包对拍
- 在没有 GUI 的环境下跑自动化脚本
- 向新语言移植时作为辅助样例

脚本没有产品级鉴权、审批、target revision、连接 generation 或完整输入上限，
不得作为 Agent/IPC adapter，也不得用于不受控的目标写入。
