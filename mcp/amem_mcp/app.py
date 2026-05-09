"""AMem MCP Server 装配 + CLI 入口。"""

from __future__ import annotations

import argparse
import os
import sys

from mcp.server.fastmcp import FastMCP

from .ipc_client import IpcClient
from .tools import register_all


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 28100


def build_server(host: str = DEFAULT_HOST, port: int = DEFAULT_PORT) -> tuple[FastMCP, IpcClient]:
    """装配一个配置完成的 FastMCP 实例。"""
    mcp = FastMCP(
        "AMem",
        instructions=(
            "AMem Android 内存调试 MCP 服务 — 通过 GUI IPC 桥接，"
            "支持内存扫描、读写、断点、Lua 脚本执行"
        ),
    )
    ipc = IpcClient(host, port)
    register_all(mcp, ipc)
    return mcp, ipc


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(
        prog="amem-mcp",
        description="AMem MCP Server (IPC 代理模式) — 桥接 AMem GUI 的内嵌 IPC Server",
    )
    parser.add_argument(
        "--ipc-host", default=os.environ.get("AMEM_IPC_HOST", DEFAULT_HOST),
        help=f"AMem GUI IPC Server 地址 (默认 {DEFAULT_HOST})",
    )
    parser.add_argument(
        "--ipc-port", type=int, default=int(os.environ.get("AMEM_IPC_PORT", DEFAULT_PORT)),
        help=f"AMem GUI IPC Server 端口 (默认 {DEFAULT_PORT})",
    )
    parser.add_argument(
        "--transport", choices=["stdio"], default="stdio",
        help="MCP 传输方式 (目前仅支持 stdio)",
    )
    args = parser.parse_args(argv)

    mcp, ipc = build_server(args.ipc_host, args.ipc_port)

    # 启动前探测 GUI 是否可达（不阻塞启动）
    probe = ipc.call("get_status")
    if probe.get("success"):
        print(f"[amem-mcp] 已连接 AMem GUI IPC Server ({ipc.base_url})", file=sys.stderr)
    else:
        print(
            f"[amem-mcp] 警告: AMem GUI 未启动或 IPC 不可达 ({ipc.base_url}) — "
            f"{probe.get('error', '')}",
            file=sys.stderr,
        )

    mcp.run(transport=args.transport)


if __name__ == "__main__":
    main()
