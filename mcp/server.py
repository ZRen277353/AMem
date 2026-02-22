#!/usr/bin/env python3
"""
AMem MCP Server
将 AMem 的 Android 内存调试能力暴露为 MCP tools，
供 Claude Code / Claude Desktop 等 AI 客户端调用。

用法:
  python server.py                          # stdio 模式（默认）
  python server.py --host 192.168.1.2       # 预连接到 Android 服务端
  python server.py --host 192.168.1.2 --port 8888
"""

import argparse
import struct
import sys

from mcp.server.fastmcp import FastMCP

from amem_client import (
    AMemClient, make_scan_flags, encode_value,
    MEM_ALL, MEM_ANONYMOUS, MEM_C_ALLOC, MEM_C_HEAP, MEM_C_DATA,
    MEM_C_BSS, MEM_JAVA_HEAP, MEM_JAVA, MEM_STACK, MEM_CODE_APP,
    MEM_CODE_SYS,
)

# ── 全局客户端实例 ────────────────────────────────────────────────
client = AMemClient()

# ── MCP Server ────────────────────────────────────────────────────
mcp = FastMCP(
    "AMem",
    instructions="AMem Android 内存调试 MCP 服务 - 远程内存扫描、读写、断点调试",
)

# ── 辅助函数 ──────────────────────────────────────────────────────

def _require_connected():
    if not client.connected:
        raise RuntimeError("未连接到 Android 服务端，请先调用 connect_server")

def _require_process():
    _require_connected()
    if client._handle == 0:
        raise RuntimeError("未打开进程，请先调用 open_process")

def _parse_addr(addr: str) -> int:
    """支持 0x 前缀和纯十进制"""
    return int(addr, 16) if addr.startswith("0x") or addr.startswith("0X") else int(addr)

def _hex_dump(data: bytes, base_addr: int, width: int = 16) -> str:
    lines = []
    for i in range(0, len(data), width):
        chunk = data[i:i + width]
        hex_part = " ".join(f"{b:02X}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{base_addr + i:012X}  {hex_part:<{width * 3}}  {ascii_part}")
    return "\n".join(lines)


# ══════════════════════════════════════════════════════════════════
#  连接管理
# ══════════════════════════════════════════════════════════════════

@mcp.tool()
def connect_server(host: str, port: int = 28000) -> str:
    """连接到 Android 端的 AMem 服务。必须在所有操作之前调用。

    Args:
        host: Android 设备 IP 地址
        port: 服务端口号，默认 28000
    """
    if client.connected:
        client.disconnect()
    if not client.connect(host, port):
        return f"连接失败: {host}:{port}"
    return f"已连接到 {host}:{port}"


@mcp.tool()
def disconnect_server() -> str:
    """断开与 Android 服务端的连接。"""
    client.disconnect()
    return "已断开连接"


@mcp.tool()
def get_server_version() -> str:
    """获取 Android 服务端版本信息。"""
    _require_connected()
    v = client.get_version()
    return f"版本号: {v.version}, 版本字符串: {v.version_string}"


@mcp.tool()
def get_architecture() -> str:
    """获取目标设备的内存架构类型。"""
    _require_connected()
    arch = client.get_architecture()
    arch_names = {0: "Null", 1: "IO", 2: "Syscall", 3: "Kernel", 4: "SysHook"}
    return f"架构类型: {arch} ({arch_names.get(arch, 'Unknown')})"


@mcp.tool()
def init_driver(card_name: str) -> str:
    """初始化内核读写驱动。

    Args:
        card_name: 卡密/授权字符串
    """
    _require_connected()
    ret, msg = client.init_driver(card_name)
    return f"结果: {ret}, 信息: {msg}"


# ══════════════════════════════════════════════════════════════════
#  进程管理
# ══════════════════════════════════════════════════════════════════

@mcp.tool()
def list_processes() -> str:
    """列出 Android 设备上所有运行中的进程。"""
    _require_connected()
    procs = client.list_processes()
    if not procs:
        return "未获取到进程列表"
    lines = [f"共 {len(procs)} 个进程:", ""]
    for p in procs:
        lines.append(f"  PID {p.pid:>6}  {p.name}")
    return "\n".join(lines)


@mcp.tool()
def open_process(pid: int) -> str:
    """打开指定 PID 的进程，后续内存操作将针对此进程。

    Args:
        pid: 目标进程 PID
    """
    _require_connected()
    handle = client.open_process(pid)
    if handle == 0:
        return f"打开进程 {pid} 失败"
    return f"已打开进程 PID={pid}, handle={handle}"


@mcp.tool()
def list_modules() -> str:
    """列出当前进程加载的所有模块（需先 open_process）。"""
    _require_process()
    mods = client.list_modules()
    if not mods:
        return "未获取到模块列表"
    lines = [f"共 {len(mods)} 个模块:", ""]
    for m in mods:
        lines.append(f"  {m.base:#014x}  size={m.size:#010x}  {m.name}")
    return "\n".join(lines)


# ══════════════════════════════════════════════════════════════════
#  内存读写
# ══════════════════════════════════════════════════════════════════

@mcp.tool()
def read_memory(address: str, size: int = 256) -> str:
    """读取进程内存并以 hex dump 格式返回。

    Args:
        address: 内存地址，支持 0x 前缀（如 "0x7f12345000"）
        size: 读取字节数，默认 256，最大 4096
    """
    _require_process()
    addr = _parse_addr(address)
    size = min(size, 4096)
    data = client.read_memory(addr, size)
    if not data:
        return f"读取 {addr:#x} 失败（返回空数据）"
    return _hex_dump(data, addr)


@mcp.tool()
def read_value(address: str, data_type: str = "dword") -> str:
    """读取指定地址的单个值。

    Args:
        address: 内存地址
        data_type: 数据类型 - byte/word/dword/qword/float/double
    """
    _require_process()
    addr = _parse_addr(address)
    size_map = {"byte": 1, "word": 2, "dword": 4, "qword": 8, "float": 4, "double": 8}
    fmt_map = {"byte": "<B", "word": "<H", "dword": "<I", "qword": "<Q", "float": "<f", "double": "<d"}
    sz = size_map.get(data_type, 4)
    data = client.read_memory(addr, sz)
    if not data or len(data) < sz:
        return f"读取失败"
    val = struct.unpack(fmt_map.get(data_type, "<I"), data[:sz])[0]
    if data_type in ("float", "double"):
        return f"[{addr:#x}] {data_type} = {val}"
    return f"[{addr:#x}] {data_type} = {val} ({val:#x})"


@mcp.tool()
def write_value(address: str, value: str, data_type: str = "dword") -> str:
    """向指定地址写入一个值。

    Args:
        address: 内存地址
        value: 要写入的值
        data_type: 数据类型 - byte/word/dword/qword/float/double
    """
    _require_process()
    addr = _parse_addr(address)
    data = encode_value(value, data_type)
    written = client.write_memory(addr, data)
    return f"已写入 {written} 字节到 {addr:#x}"


@mcp.tool()
def write_bytes(address: str, hex_string: str) -> str:
    """向指定地址写入原始字节。

    Args:
        address: 内存地址
        hex_string: 十六进制字节串，如 "90 90 90" 或 "909090"
    """
    _require_process()
    addr = _parse_addr(address)
    hex_clean = hex_string.replace(" ", "").replace("\n", "")
    data = bytes.fromhex(hex_clean)
    written = client.write_memory(addr, data)
    return f"已写入 {written} 字节到 {addr:#x}"


# ══════════════════════════════════════════════════════════════════
#  内存扫描
# ══════════════════════════════════════════════════════════════════

MEMORY_TYPE_MAP = {
    "all": MEM_ALL, "anonymous": MEM_ANONYMOUS, "c_alloc": MEM_C_ALLOC,
    "c_heap": MEM_C_HEAP, "c_data": MEM_C_DATA, "c_bss": MEM_C_BSS,
    "java_heap": MEM_JAVA_HEAP, "java": MEM_JAVA, "stack": MEM_STACK,
    "code_app": MEM_CODE_APP, "code_system": MEM_CODE_SYS,
}

@mcp.tool()
def scan_set_range(memory_type: str = "all") -> str:
    """设置扫描的内存区域类型。

    Args:
        memory_type: 内存类型 - all/anonymous/c_alloc/c_heap/c_data/c_bss/java_heap/java/stack/code_app/code_system
    """
    _require_process()
    mt = MEMORY_TYPE_MAP.get(memory_type.lower(), MEM_ALL)
    client.scan_set_range(mt)
    return f"扫描范围已设置为: {memory_type}"


@mcp.tool()
def scan_value(value: str, data_type: str = "dword", scan_type: str = "exact") -> str:
    """首次扫描 - 在内存中搜索指定值。

    Args:
        value: 要搜索的值
        data_type: 数据类型 - byte/word/dword/qword/float/double
        scan_type: 扫描类型 - exact/unknown/greater/less/between
    """
    _require_process()
    flags = make_scan_flags(scan_type, data_type)
    val_bytes = encode_value(value, data_type)
    count = client.scan_value(flags, val_bytes)
    return f"首次扫描完成，找到 {count} 个结果"


@mcp.tool()
def scan_next(value: str, data_type: str = "dword", scan_type: str = "exact") -> str:
    """再次扫描 - 在上次结果中筛选。

    Args:
        value: 要搜索的值
        data_type: 数据类型 - byte/word/dword/qword/float/double
        scan_type: 扫描类型 - exact/increased/decreased/changed/unchanged/increased_by/decreased_by/greater/less
    """
    _require_process()
    flags = make_scan_flags(scan_type, data_type)
    val_bytes = encode_value(value, data_type)
    count = client.scan_next_value(flags, val_bytes)
    return f"再次扫描完成，剩余 {count} 个结果"


@mcp.tool()
def scan_fuzzy(data_type: str = "dword", scan_type: str = "unknown") -> str:
    """模糊扫描 - 搜索未知初始值（首次）或值变化（后续）。

    Args:
        data_type: 数据类型
        scan_type: unknown/increased/decreased/changed/unchanged
    """
    _require_process()
    flags = make_scan_flags(scan_type, data_type)
    count = client.scan_fuzzy(flags)
    return f"模糊扫描完成，找到 {count} 个结果"


@mcp.tool()
def scan_hex(hex_pattern: str) -> str:
    """十六进制模式扫描。

    Args:
        hex_pattern: 十六进制字节串，如 "48 65 6C 6C 6F"
    """
    _require_process()
    pattern = bytes.fromhex(hex_pattern.replace(" ", ""))
    count = client.scan_hex(pattern)
    return f"HEX 扫描完成，找到 {count} 个结果"


@mcp.tool()
def get_scan_count() -> str:
    """获取当前扫描结果总数。"""
    _require_process()
    count = client.get_scan_result_count()
    return f"当前扫描结果: {count} 个"


@mcp.tool()
def get_scan_results(offset: int = 0, count: int = 20) -> str:
    """获取扫描结果列表（分页）。

    Args:
        offset: 起始偏移
        count: 获取数量，默认 20，最大 100
    """
    _require_process()
    count = min(count, 100)
    results = client.get_scan_results(offset, count)
    if not results:
        return "无扫描结果"
    lines = [f"扫描结果 (offset={offset}, count={len(results)}):", ""]
    for r in results:
        lines.append(f"  {r.address:#014x}  value={r.value} ({r.value:#x})")
    return "\n".join(lines)


@mcp.tool()
def clear_scan() -> str:
    """清除所有扫描结果。"""
    _require_process()
    client.clear_scan_results()
    return "扫描结果已清除"


# ══════════════════════════════════════════════════════════════════
#  硬件断点
# ══════════════════════════════════════════════════════════════════

@mcp.tool()
def set_breakpoint(address: str, bp_type: int = 1, bp_size: int = 4) -> str:
    """设置硬件断点。

    Args:
        address: 断点地址
        bp_type: 断点类型 (1=执行, 2=写入, 3=读写)
        bp_size: 监控大小 (1/2/4/8 字节)
    """
    _require_process()
    addr = _parse_addr(address)
    ok = client.set_breakpoint(addr, bp_type, bp_size)
    return f"断点 {addr:#x} {'设置成功' if ok else '设置失败'}"


@mcp.tool()
def remove_breakpoint(address: str) -> str:
    """移除硬件断点。

    Args:
        address: 断点地址
    """
    _require_process()
    addr = _parse_addr(address)
    ok = client.remove_breakpoint(addr)
    return f"断点 {addr:#x} {'已移除' if ok else '移除失败'}"


@mcp.tool()
def read_breakpoint_info(address: str) -> str:
    """读取断点命中信息（ARM64 寄存器状态）。

    Args:
        address: 断点地址
    """
    _require_process()
    addr = _parse_addr(address)
    hits = client.read_breakpoint_info(addr)
    if not hits:
        return f"断点 {addr:#x} 无命中记录"
    lines = [f"断点 {addr:#x} 共 {len(hits)} 次命中:", ""]
    for i, h in enumerate(hits):
        lines.append(f"--- 命中 #{i + 1} ---")
        lines.append(f"  命中地址: {h.hit_addr:#x}")
        lines.append(f"  PC: {h.pc:#x}  SP: {h.sp:#x}")
        # 显示关键寄存器
        for j in range(0, min(len(h.regs), 31), 4):
            parts = [f"X{j+k}={h.regs[j+k]:#x}" for k in range(4) if j + k < len(h.regs)]
            lines.append(f"  {' '.join(parts)}")
        lines.append("")
    return "\n".join(lines)


@mcp.tool()
def suspend_breakpoint(address: str) -> str:
    """暂停硬件断点（不删除）。

    Args:
        address: 断点地址
    """
    _require_process()
    addr = _parse_addr(address)
    ok = client.suspend_breakpoint(addr)
    return f"断点 {addr:#x} {'已暂停' if ok else '暂停失败'}"


@mcp.tool()
def resume_breakpoint(address: str) -> str:
    """恢复已暂停的硬件断点。

    Args:
        address: 断点地址
    """
    _require_process()
    addr = _parse_addr(address)
    ok = client.resume_breakpoint(addr)
    return f"断点 {addr:#x} {'已恢复' if ok else '恢复失败'}"


# ══════════════════════════════════════════════════════════════════
#  MCP Resources
# ══════════════════════════════════════════════════════════════════

@mcp.resource("amem://status")
def resource_status() -> str:
    """当前连接和进程状态。"""
    if not client.connected:
        return "状态: 未连接"
    info = f"状态: 已连接\nPID: {client._pid}\nHandle: {client._handle}"
    return info


# ══════════════════════════════════════════════════════════════════
#  入口
# ══════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description="AMem MCP Server")
    parser.add_argument("--host", help="启动时自动连接的 Android 服务端 IP")
    parser.add_argument("--port", type=int, default=28000, help="服务端口 (默认 28000)")
    args = parser.parse_args()

    if args.host:
        if client.connect(args.host, args.port):
            print(f"已连接到 {args.host}:{args.port}", file=sys.stderr)
        else:
            print(f"警告: 无法连接到 {args.host}:{args.port}", file=sys.stderr)

    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
