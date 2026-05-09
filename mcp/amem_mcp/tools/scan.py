"""内存扫描。"""

from __future__ import annotations

from mcp.server.fastmcp import FastMCP

from ..constants import MEMORY_TYPE_MAP
from ..helpers import encode_value_hex, make_scan_flags
from ..ipc_client import IpcClient


def register(mcp: FastMCP, ipc: IpcClient) -> None:

    @mcp.tool()
    def scan_set_range(memory_type: str = "all") -> str:
        """设置扫描的内存区域类型。

        Args:
            memory_type: all / anonymous / c_alloc / c_heap / c_data / c_bss /
                         java_heap / java / stack / code_app / code_system /
                         video / ashmem / bad
        """
        mt = MEMORY_TYPE_MAP.get(memory_type.lower(), -1)
        ipc.call_or_raise("scan_set_range", {"type": mt})
        return f"扫描范围已设置为: {memory_type}"

    @mcp.tool()
    def scan_value(value: str, data_type: str = "dword", scan_type: str = "exact") -> str:
        """首次扫描 - 在内存中搜索指定值。

        Args:
            value: 要搜索的值
            data_type: byte/word/dword/qword/float/double
            scan_type: exact/unknown/greater/less/between
        """
        flags = make_scan_flags(scan_type, data_type)
        val_hex = encode_value_hex(value, data_type)
        r = ipc.call_or_raise("scan_value", {"flags": flags, "value_hex": val_hex})
        return f"首次扫描完成，找到 {r['count']} 个结果"

    @mcp.tool()
    def scan_next(value: str, data_type: str = "dword", scan_type: str = "exact") -> str:
        """再次扫描 - 在上次结果中筛选。

        Args:
            value: 要搜索的值
            data_type: byte/word/dword/qword/float/double
            scan_type: exact/increased/decreased/changed/unchanged/
                       increased_by/decreased_by/greater/less
        """
        flags = make_scan_flags(scan_type, data_type)
        val_hex = encode_value_hex(value, data_type)
        r = ipc.call_or_raise("scan_next", {
            "flags": flags, "value_hex": val_hex, "scan_flag": flags,
        })
        return f"再次扫描完成，剩余 {r['count']} 个结果"

    @mcp.tool()
    def scan_fuzzy(data_type: str = "dword", scan_type: str = "unknown") -> str:
        """模糊扫描 - 搜索未知初始值或值变化。

        Args:
            data_type: byte/word/dword/qword/float/double
            scan_type: unknown/increased/decreased/changed/unchanged
        """
        flags = make_scan_flags(scan_type, data_type)
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
        r = ipc.call_or_raise("get_scan_results", {"offset": offset, "count": count})
        total = r.get("total", 0)
        items = r.get("items", [])
        off = r.get("offset", offset)
        if not items:
            return f"无扫描结果（总数: {total}）"
        lines = [f"扫描结果（总数: {total}, offset: {off}, 本页: {len(items)}）:", ""]
        for item in items:
            lines.append(f"  {item['address']}  value={item['value']} ({item['value']:#x})")
        if off + len(items) < total:
            lines.append(f"\n... 还有 {total - off - len(items)} 个结果未显示")
        return "\n".join(lines)

    @mcp.tool()
    def clear_scan() -> str:
        """清除所有扫描结果。"""
        ipc.call_or_raise("clear_scan")
        return "扫描结果已清除"
