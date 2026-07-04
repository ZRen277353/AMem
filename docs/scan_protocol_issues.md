# 搜索/扫描协议问题记录

记录时间：2026-05-29

本文档先保留当前 AMem 前端与 android_mem_engine 后端在搜索/扫描协议上的问题，不在本批次直接修复实现。后续修复时按本文档拆分小提交，避免一次性改动 socket 行为导致回归难以定位。

## 涉及代码

AMem 前端：

- `socket/ScanCommands.cpp`
- `socket/SocketCommand.h`
- `socket/client_singleton.h`
- `gui/scan/ScanEngine.cpp`
- `gui/scan/ScanPanel.cpp`
- `gui/ai/ToolDefinitions.cpp`

android_mem_engine 后端：

- `ceserver/CEServer.cpp`
- `ceserver/api.cpp`
- `ceserver/api.h`
- `common/ScanProgress.hpp`
- `interface/socket/server.cpp`

## 已确认问题

### 1. `CMD_SETRANGE` 没有响应确认

前端 `ScanSetRange()` 发送：

- `CMD_SETRANGE`
- `HANDLE`
- `int type`

后端 `CMD_SETRANGE` 接收并调用 `CApi::SetMemoryFilter()`，但不返回任何确认值。

影响：

- 前端只能确认数据已发送，无法确认后端是否成功设置扫描范围和进度回调。
- 后续扫描命令依赖这个回调发送进度消息，如果 `SetMemoryFilter()` 未生效，前端会继续等待扫描进度流，存在卡住风险。
- 协议语义不完整，AI 工具、扫描面板和后续 agent 自动化调用都无法得到明确失败原因。

建议：

- 后端在 `CMD_SETRANGE` 完成后返回 `int result`。
- 前端 `ScanSetRange()` 读取该 result，并以 `result != 0` 作为成功条件。
- 如果需要兼容旧后端，可以通过协议版本或短期 feature flag 过渡。

### 2. 扫描命令强依赖进度流终止消息

前端带进度扫描函数会调用 `SocketCommand::receiveProgressLoop()`，循环读取 `ScanProgress`，直到：

- `msgType == 2`：扫描完成
- `msgType == 3`：扫描错误

后端扫描命令在扫描结束后再发送 `int` 结果数量，但终止进度消息依赖扫描库回调链路。

涉及命令：

- `CMD_SCANVALUE`
- `CMD_SCANNEXTVALUE`
- `CMD_SCANFUZZYVALUE`
- `CMD_SCANGROUPVALUE`
- `CMD_SCANHEX`

影响：

- 如果进度回调未注册、扫描库异常路径未触发终止消息，前端会一直阻塞在进度接收循环。
- 用户侧表现为搜索卡住，后续同端口请求也会被 socket 请求锁阻塞。
- agent 自动化搜索时更明显，因为 agent 会等待工具调用完成。

建议：

- 后端保证每个扫描命令在任何可控结束路径都发送终止进度消息。
- 成功路径发送 `msgType == 2`，失败路径发送 `msgType == 3`。
- 前端保留 result count 读取，但必须先可靠结束进度流。

### 3. `receiveProgressLoop()` 未处理未知 `msgType`

当前循环只处理：

- `1`：进度
- `2`：完成
- `3`：错误

其他值不会返回，也不会报错，会继续读取下一条消息。

影响：

- 一旦协议错位，例如后端提前发送 result count，前端会把普通整数误解释成 `ScanProgress.msgType`。
- 未知消息不会终止循环，可能把后续响应越读越乱。

建议：

- 明确校验 `msgType` 只能是 `1/2/3`。
- 遇到未知 `msgType` 立即返回失败，并记录日志。
- 后续可以把错误暴露给 UI 和 AI tool result。

### 4. 无 socket 接收超时，协议错位时容易永久阻塞

`WindowsSocketClient::Receive()` 当前是阻塞读取。扫描协议只要少发一个消息或消息顺序不一致，UI/AI 工具调用线程就可能永久等待。

影响：

- 搜索卡住后，用户只能断开连接或重启程序。
- agent 项目化后，工具调用可能长期不返回，影响任务编排和取消逻辑。

建议：

- 为扫描进度循环增加可配置超时。
- 超时后返回明确错误，释放 socket 请求锁。
- 不建议全局改变所有 `Receive()` 行为，优先在扫描进度路径做局部增强。

### 5. `StopSearchScan(PORT_DEBUG)` 的端口语义不清晰

前端默认停止搜索使用 `PORT_DEBUG`，但后端 `interface/socket/server.cpp` 当前并没有区分 MAIN/DEBUG/ERROR 角色，所有连接都按命令连接处理。

影响：

- 当前可以工作，是因为 DEBUG 实际只是另一条命令连接。
- 语义上容易误导后续维护者，以为后端存在独立 debug 通道。
- 后续如果后端引入连接角色握手，停止搜索命令需要重新确认路由。

建议：

- 短期在文档和代码命名中说明 DEBUG 只是独立命令连接。
- 长期加入连接角色握手，再明确 MAIN/DEBUG/ERROR 的真实职责。

### 6. 后端 `cmd.md` 与实际命令号不一致

后端 `ceserver/cmd.md` 中扫描结果命令号仍是旧值：

- 文档写 `CMD_GETSCANRESULT_COUNT = 251`
- 文档写 `CMD_GETSCANRESULT = 252`

实际 C++ 宏定义为：

- `CMD_GETSCANRESULT_COUNT = 244`
- `CMD_GETSCANRESULT = 245`

影响：

- 后续排查协议时容易误读文档。
- agent 自动化生成协议工具时不能直接信任该文档。

建议：

- 以后端 `ceserver.h` 和前端 `socket/client.hpp` 的宏定义为准。
- 单独提交更新后端协议文档。

## 建议修复顺序

1. 先补齐扫描协议的失败检测：前端 `receiveProgressLoop()` 校验未知 `msgType`，并加入扫描接收超时。
2. 再补齐 `CMD_SETRANGE` ack：前后端同步增加 result 返回值。
3. 后端扫描命令统一发送终止进度消息，确保成功、失败和异常路径都有明确协议结尾。
4. 梳理 `StopSearchScan(PORT_DEBUG)` 的语义，先文档化，后续再考虑连接角色握手。
5. 更新后端 `cmd.md`，让协议文档与代码宏一致。

## 暂缓修复项

- socket/IPC 架构级重构暂缓。
- MAIN/DEBUG/ERROR 三连接角色握手暂缓。
- 扫描协议二进制格式重设计暂缓。
- AI agent 层的工具取消、重试、超时策略等，等底层扫描协议稳定后再继续推进。
