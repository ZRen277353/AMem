"""Lua 脚本执行。"""

from __future__ import annotations

from mcp.server.fastmcp import FastMCP

from ..ipc_client import IpcClient


def register(mcp: FastMCP, ipc: IpcClient) -> None:

    @mcp.tool()
    def execute_lua(code: str) -> str:
        """在 AMem GUI 内执行 Lua 脚本。

        可使用 mem/process/scan/bp 等全部 Lua API。
        这是最强大的工具 — 可以编写任意复杂的自动化逻辑。
        Lua 引擎会在首次调用时自动初始化，无需手动打开 Lua 窗口。

        Args:
            code: Lua 脚本代码
        """
        r = ipc.call_or_raise("execute_lua", {"code": code})
        output = r.get("output", "")
        return output if output else "(执行成功，无输出)"
