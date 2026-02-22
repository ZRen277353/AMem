#!/usr/bin/env python3
"""
AMem MCP Server (方案 C — HTTP 代理模式)
通过 HTTP 调用 AMem GUI 内嵌的 IPC Server，复用 GUI 全部能力。

用法:
  python server.py                          # stdio 模式（默认）
  python server.py --ipc-port 28100         # 指定 IPC 端口
"""

import argparse
import struct
import sys

from mcp.server.fastmcp import FastMCP
from ipc_client import IpcClient

# ── 全局 IPC 客户端 ──────────────────────────────────────────────
ipc = IpcClient()

# ── MCP Server ────────────────────────────────────────────────────
mcp = FastMCP(
    "AMem",
    instructions="AMem Android 内存调试 MCP 服务 — 通过 GUI IPC 桥接，支持内存扫描、读写、断点、Lua 脚本执行",
)

# ── 辅助 ─────────────────────────────────────────────────────────

def _require_gui():
    """检查 GUI IPC 是否可达"""
    resp = ipc.call("get_status")
    if not resp.get("success"):
        raise RuntimeError("无法连接到 AMem GUI，请确保 GUI 已启动")
    return resp["result"]

def _hex_dump(hex_str: str, base_addr: int, width: int = 16) -> str:
    data = bytes.fromhex(hex_str)
    lines = []
    for i in range(0, len(data), width):
        chunk = data[i:i + width]
        hex_part = " ".join(f"{b:02X}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{base_addr + i:012X}  {hex_part:<{width * 3}}  {ascii_part}")
    return "\n".join(lines)

def _encode_value_hex(value: str, data_type: str) -> str:
    """将值编码为 hex 字符串"""
    fmt = {"byte": "<B", "word": "<H", "dword": "<I", "qword": "<Q",
           "float": "<f", "double": "<d"}
    v = float(value) if data_type in ("float", "double") else int(value)
    return struct.pack(fmt.get(data_type, "<I"), v).hex()

# ── 扫描 flag 构造 ───────────────────────────────────────────────
SCAN1_UNKNOW      = 1 << 30
SCAN1_ACCURATE    = 1 << 29
SCAN1_LARGER      = 1 << 28
SCAN1_LESS        = 1 << 27
SCAN1_BETWEEN     = 1 << 26
SCAN1_ADD_UNKNOW  = 1 << 25
SCAN1_ADD_ACCURATE = 1 << 24
SCAN1_SUB_UNKNOW  = 1 << 23
SCAN1_SUB_ACCURATE = 1 << 22
SCAN1_CHANGED     = 1 << 21
SCAN1_UNCHANGED   = 1 << 20

TYPE_BYTE = 1; TYPE_WORD = 2; TYPE_DWORD = 4; TYPE_FLOAT = 16
TYPE_QWORD = 32; TYPE_DOUBLE = 64; TYPE_XOR = 8

MEMORY_TYPE_MAP = {
    "all": -1, "anonymous": 32, "c_alloc": 4, "c_heap": 1, "c_data": 8,
    "c_bss": 16, "java_heap": 2, "java": 65536, "stack": 64,
    "code_app": 16384, "code_system": 32768,
}

def _make_scan_flags(scan_type: str, data_type: str) -> int:
    scan_map = {
        "exact": SCAN1_ACCURATE, "unknown": SCAN1_UNKNOW,
        "greater": SCAN1_LARGER, "less": SCAN1_LESS,
        "between": SCAN1_BETWEEN, "increased": SCAN1_ADD_UNKNOW,
        "increased_by": SCAN1_ADD_ACCURATE, "decreased": SCAN1_SUB_UNKNOW,
        "decreased_by": SCAN1_SUB_ACCURATE, "changed": SCAN1_CHANGED,
        "unchanged": SCAN1_UNCHANGED,
    }
    type_map = {
        "byte": TYPE_BYTE, "word": TYPE_WORD, "dword": TYPE_DWORD,
        "qword": TYPE_QWORD, "float": TYPE_FLOAT, "double": TYPE_DOUBLE,
        "xor": TYPE_XOR,
    }
    return scan_map.get(scan_type, SCAN1_ACCURATE) | type_map.get(data_type, TYPE_DWORD)


# ══════════════════════════════════════════════════════════════════
#  状态与连接
# ══════════════════════════════════════════════════════════════════

@mcp.tool()
def get_status() -> str:
    """获取 AMem GUI 当前状态（连接、进程信息）。"""
    r = ipc.call_or_raise("get_status")
    connected = "已连接" if r["connected"] else "未连接"
    pid = r.get("pid", 0)
    name = r.get("process_name", "")
    return f"服务端: {connected}, PID: {pid}, 进程: {name}"


@mcp.tool()
def get_server_version() -> str:
    """获取 Android 服务端版本信息。"""
    r = ipc.call_or_raise("get_version")
    return f"版本号: {r['version']}, 版本字符串: {r['version_string']}"


@mcp.tool()
def get_architecture() -> str:
    """获取目标设备的内存架构类型。"""
    r = ipc.call_or_raise("get_architecture")
    return f"架构类型: {r['type']} ({r['name']})"


@mcp.tool()
def init_driver(card_name: str) -> str:
    """初始化内核读写驱动。

    Args:
        card_name: 卡密/授权字符串
    """
    r = ipc.call_or_raise("init_driver", {"card": card_name})
    return f"结果: {r['message']}"


# ══════════════════════════════════════════════════════════════════
#  进程管理
# ══════════════════════════════════════════════════════════════════

@mcp.tool()
def list_processes() -> str:
    """列出 Android 设备上所有运行中的进程。"""
    procs = ipc.call_or_raise("list_processes")
    if not procs:
        return "未获取到进程列表"
    lines = [f"共 {len(procs)} 个进程:", ""]
    for p in procs:
        lines.append(f"  PID {p['pid']:>6}  {p['name']}")
    return "\n".join(lines)


@mcp.tool()
def open_process(pid: int) -> str:
    """打开指定 PID 的进程，后续内存操作将针对此进程。

    Args:
        pid: 目标进程 PID
    """
    r = ipc.call_or_raise("open_process", {"pid": pid})
    return f"已打开进程 PID={pid}, handle={r['handle']}"


@mcp.tool()
def list_modules() -> str:
    """列出当前进程加载的所有模块。"""
    mods = ipc.call_or_raise("list_modules")
    if not mods:
        return "未获取到模块列表"
    lines = [f"共 {len(mods)} 个模块:", ""]
    for m in mods:
        lines.append(f"  {m['base']}  size={m['size']:#010x}  {m['name']}")
    return "\n".join(lines)


# ══════════════════════════════════════════════════════════════════
#  内存读写
# ══════════════════════════════════════════════════════════════════

@mcp.tool()
def read_memory(address: str, size: int = 256) -> str:
    """读取进程内存并以 hex dump 格式返回。

    Args:
        address: 内存地址，支持 0x 前缀（如 "0x7f12345000"）
        size: 读取字节数，默认 256，最大 65536
    """
    size = min(size, 65536)
    r = ipc.call_or_raise("read_memory", {"address": address, "size": size})
    addr_int = int(address, 16) if address.startswith("0x") or address.startswith("0X") else int(address)
    return _hex_dump(r["hex"], addr_int)


@mcp.tool()
def read_value(address: str, data_type: str = "dword") -> str:
    """读取指定地址的单个值。

    Args:
        address: 内存地址
        data_type: 数据类型 - byte/word/dword/qword/float/double
    """
    size_map = {"byte": 1, "word": 2, "dword": 4, "qword": 8, "float": 4, "double": 8}
    fmt_map = {"byte": "<B", "word": "<H", "dword": "<I", "qword": "<Q", "float": "<f", "double": "<d"}
    sz = size_map.get(data_type, 4)
    r = ipc.call_or_raise("read_memory", {"address": address, "size": sz})
    data = bytes.fromhex(r["hex"])
    if len(data) < sz:
        return "读取失败"
    val = struct.unpack(fmt_map.get(data_type, "<I"), data[:sz])[0]
    addr_int = int(address, 16) if address.startswith(("0x", "0X")) else int(address)
    if data_type in ("float", "double"):
        return f"[{addr_int:#x}] {data_type} = {val}"
    return f"[{addr_int:#x}] {data_type} = {val} ({val:#x})"


@mcp.tool()
def write_value(address: str, value: str, data_type: str = "dword") -> str:
    """向指定地址写入一个值。

    Args:
        address: 内存地址
        value: 要写入的值
        data_type: 数据类型 - byte/word/dword/qword/float/double
    """
    hex_val = _encode_value_hex(value, data_type)
    r = ipc.call_or_raise("write_memory", {"address": address, "hex": hex_val})
    return f"已写入 {r['written']} 字节到 {address}"


@mcp.tool()
def write_bytes(address: str, hex_string: str) -> str:
    """向指定地址写入原始字节。

    Args:
        address: 内存地址
        hex_string: 十六进制字节串，如 "90 90 90" 或 "909090"
    """
    hex_clean = hex_string.replace(" ", "").replace("\n", "")
    r = ipc.call_or_raise("write_memory", {"address": address, "hex": hex_clean})
    return f"已写入 {r['written']} 字节到 {address}"


# ══════════════════════════════════════════════════════════════════
#  内存扫描
# ══════════════════════════════════════════════════════════════════

@mcp.tool()
def scan_set_range(memory_type: str = "all") -> str:
    """设置扫描的内存区域类型。

    Args:
        memory_type: 内存类型 - all/anonymous/c_alloc/c_heap/c_data/c_bss/java_heap/java/stack/code_app/code_system
    """
    mt = MEMORY_TYPE_MAP.get(memory_type.lower(), -1)
    ipc.call_or_raise("scan_set_range", {"type": mt})
    return f"扫描范围已设置为: {memory_type}"


@mcp.tool()
def scan_value(value: str, data_type: str = "dword", scan_type: str = "exact") -> str:
    """首次扫描 - 在内存中搜索指定值。

    Args:
        value: 要搜索的值
        data_type: 数据类型 - byte/word/dword/qword/float/double
        scan_type: 扫描类型 - exact/unknown/greater/less/between
    """
    flags = _make_scan_flags(scan_type, data_type)
    val_hex = _encode_value_hex(value, data_type)
    r = ipc.call_or_raise("scan_value", {"flags": flags, "value_hex": val_hex})
    return f"首次扫描完成，找到 {r['count']} 个结果"


@mcp.tool()
def scan_next(value: str, data_type: str = "dword", scan_type: str = "exact") -> str:
    """再次扫描 - 在上次结果中筛选。

    Args:
        value: 要搜索的值
        data_type: 数据类型 - byte/word/dword/qword/float/double
        scan_type: 扫描类型 - exact/increased/decreased/changed/unchanged/increased_by/decreased_by/greater/less
    """
    flags = _make_scan_flags(scan_type, data_type)
    val_hex = _encode_value_hex(value, data_type)
    r = ipc.call_or_raise("scan_next", {"flags": flags, "value_hex": val_hex, "scan_flag": flags})
    return f"再次扫描完成，剩余 {r['count']} 个结果"


@mcp.tool()
def scan_fuzzy(data_type: str = "dword", scan_type: str = "unknown") -> str:
    """模糊扫描 - 搜索未知初始值（首次）或值变化（后续）。

    Args:
        data_type: 数据类型
        scan_type: unknown/increased/decreased/changed/unchanged
    """
    flags = _make_scan_flags(scan_type, data_type)
    r = ipc.call_or_raise("scan_fuzzy", {"flags": flags})
    return f"模糊扫描完成，找到 {r['count']} 个结果"


@mcp.tool()
def scan_hex(hex_pattern: str) -> str:
    """十六进制模式扫描。

    Args:
        hex_pattern: 十六进制字节串，如 "48 65 6C 6C 6F"
    """
    pattern = hex_pattern.replace(" ", "")
    r = ipc.call_or_raise("scan_hex", {"pattern_hex": pattern})
    return f"HEX 扫描完成，找到 {r['count']} 个结果"


@mcp.tool()
def get_scan_count() -> str:
    """获取当前扫描结果总数。"""
    r = ipc.call_or_raise("get_scan_count")
    return f"当前扫描结果: {r['count']} 个"


@mcp.tool()
def get_scan_results(offset: int = 0, count: int = 20) -> str:
    """获取扫描结果列表（分页）。

    Args:
        offset: 起始偏移
        count: 获取数量，默认 20，最大 1000
    """
    count = min(count, 1000)
    results = ipc.call_or_raise("get_scan_results", {"offset": offset, "count": count})
    if not results:
        return "无扫描结果"
    lines = [f"扫描结果 (offset={offset}, count={len(results)}):", ""]
    for r in results:
        lines.append(f"  {r['address']}  value={r['value']} ({r['value']:#x})")
    return "\n".join(lines)


@mcp.tool()
def clear_scan() -> str:
    """清除所有扫描结果。"""
    ipc.call_or_raise("clear_scan")
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
    ipc.call_or_raise("set_breakpoint", {"address": address, "bp_type": bp_type, "bp_size": bp_size})
    return f"断点 {address} 设置成功"


@mcp.tool()
def remove_breakpoint(address: str) -> str:
    """移除硬件断点。

    Args:
        address: 断点地址
    """
    ipc.call_or_raise("remove_breakpoint", {"address": address})
    return f"断点 {address} 已移除"


@mcp.tool()
def read_breakpoint_info(address: str) -> str:
    """读取断点命中信息（ARM64 寄存器状态）。

    Args:
        address: 断点地址
    """
    hits = ipc.call_or_raise("read_bp_info", {"address": address})
    if not hits:
        return f"断点 {address} 无命中记录"
    lines = [f"断点 {address} 共 {len(hits)} 次命中:", ""]
    for i, h in enumerate(hits):
        lines.append(f"--- 命中 #{i + 1} ---")
        lines.append(f"  命中地址: {h['hit_addr']}")
        lines.append(f"  PC: {h['pc']}  SP: {h['sp']}")
        regs = h.get("regs", [])
        for j in range(0, min(len(regs), 31), 4):
            parts = [f"X{j+k}={regs[j+k]:#x}" for k in range(4) if j + k < len(regs)]
            lines.append(f"  {' '.join(parts)}")
        lines.append("")
    return "\n".join(lines)


@mcp.tool()
def suspend_breakpoint(address: str) -> str:
    """暂停硬件断点（不删除）。

    Args:
        address: 断点地址
    """
    ipc.call_or_raise("suspend_breakpoint", {"address": address})
    return f"断点 {address} 已暂停"


@mcp.tool()
def resume_breakpoint(address: str) -> str:
    """恢复已暂停的硬件断点。

    Args:
        address: 断点地址
    """
    ipc.call_or_raise("resume_breakpoint", {"address": address})
    return f"断点 {address} 已恢复"


# ══════════════════════════════════════════════════════════════════
#  Lua 脚本执行
# ══════════════════════════════════════════════════════════════════

@mcp.tool()
def execute_lua(code: str) -> str:
    """在 AMem GUI 内执行 Lua 脚本。
    可使用 mem/process/scan/bp 等全部 Lua API。
    这是最强大的工具 — 可以编写任意复杂的自动化逻辑。

    Args:
        code: Lua 脚本代码
    """
    r = ipc.call_or_raise("execute_lua", {"code": code})
    output = r.get("output", "")
    return output if output else "(执行成功，无输出)"


# ══════════════════════════════════════════════════════════════════
#  模块辅助
# ══════════════════════════════════════════════════════════════════

@mcp.tool()
def get_module_base(module_name: str) -> str:
    """获取指定模块的基址。

    Args:
        module_name: 模块名称
    """
    r = ipc.call_or_raise("get_module_base", {"name": module_name})
    return f"模块 {module_name} 基址: {r['base']}"


@mcp.tool()
def resolve_offset_chain(module: str, base_offset: str, offsets: list[int] = None, deref_final: bool = True) -> str:
    """解析模块偏移链（指针链），获取最终地址。

    Args:
        module: 模块名称
        base_offset: 基础偏移（支持 0x 前缀）
        offsets: 偏移链列表，如 [0x10, 0x20, 0x8]
        deref_final: 是否解引用最终地址，默认 True
    """
    r = ipc.call_or_raise("resolve_offset_chain", {
        "module": module,
        "base_offset": base_offset,
        "offsets": offsets or [],
        "deref_final": deref_final,
    })
    return f"最终地址: {r['address']}"


# ══════════════════════════════════════════════════════════════════
#  MCP Resources
# ══════════════════════════════════════════════════════════════════

@mcp.resource("amem://status")
def resource_status() -> str:
    """当前 AMem GUI 状态。"""
    try:
        r = ipc.call_or_raise("get_status")
        connected = "已连接" if r["connected"] else "未连接"
        return f"状态: {connected}\nPID: {r.get('pid', 0)}\n进程: {r.get('process_name', '')}"
    except Exception as e:
        return f"状态: GUI 未连接 ({e})"


# ══════════════════════════════════════════════════════════════════
#  入口
# ══════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description="AMem MCP Server (IPC 代理模式)")
    parser.add_argument("--ipc-host", default="127.0.0.1", help="IPC Server 地址 (默认 127.0.0.1)")
    parser.add_argument("--ipc-port", type=int, default=28100, help="IPC Server 端口 (默认 28100)")
    args = parser.parse_args()

    global ipc
    ipc = IpcClient(args.ipc_host, args.ipc_port)

    # 检查 GUI 是否可达
    resp = ipc.call("get_status")
    if resp.get("success"):
        print("已连接到 AMem GUI IPC Server", file=sys.stderr)
    else:
        print("警告: AMem GUI 未启动或 IPC Server 不可达", file=sys.stderr)

    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
