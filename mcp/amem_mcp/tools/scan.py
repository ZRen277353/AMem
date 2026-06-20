"""内存扫描。"""

from __future__ import annotations

from mcp.server.fastmcp import FastMCP

from ..constants import MEMORY_TYPE_MAP
from ..helpers import (
    clean_hex_string,
    clamp_page,
    encode_value_hex,
    make_scan_flags,
    normalize_memory_type,
    normalize_scan_type,
)
from ..ipc_client import IpcClient


NO_VALUE_SCAN_TYPES = {"increased", "decreased", "changed", "unchanged"}
FIRST_SCAN_TYPES = {"exact", "greater", "less", "between"}
NEXT_SCAN_TYPES = {
    "exact", "greater", "less", "between",
    "increased", "increased_by", "decreased", "decreased_by",
    "changed", "unchanged",
}
FUZZY_SCAN_TYPES = {"unknown", "increased", "decreased", "changed", "unchanged"}


def register(mcp: FastMCP, ipc: IpcClient) -> None:

    @mcp.tool()
    def scan_set_range(memory_type: str = "all") -> str:
        """设置扫描的内存区域类型。

        Args:
            memory_type: all / anonymous / c_alloc / c_heap / c_data / c_bss /
                         java_heap / java / stack / code_app / code_system /
                         video / ashmem / bad
        """
        memory_type = normalize_memory_type(memory_type)
        mt = MEMORY_TYPE_MAP[memory_type]
        ipc.call_or_raise("scan_set_range", {"type": mt})
        return f"扫描范围已设置为: {memory_type}"

    @mcp.tool()
    def scan_value(value: str, data_type: str = "dword",
                   scan_type: str = "exact", value2: str = "") -> str:
        """首次扫描 - 在内存中搜索指定值。

        Args:
            value: 要搜索的值
            data_type: byte/word/dword/qword/float/double
            scan_type: exact/greater/less/between
        """
        scan_type = normalize_scan_type(scan_type, FIRST_SCAN_TYPES)
        flags = make_scan_flags(scan_type, data_type)
        params = {"flags": flags, "value_hex": encode_value_hex(value, data_type)}
        if scan_type == "between":
            if value2 == "":
                raise ValueError("scan_value with scan_type='between' requires value2")
            params["value2_hex"] = encode_value_hex(value2, data_type)
        r = ipc.call_or_raise("scan_value", params)
        return f"首次扫描完成，找到 {r['count']} 个结果"

    @mcp.tool()
    def scan_next(value: str = "", data_type: str = "dword",
                  scan_type: str = "exact", value2: str = "") -> str:
        """再次扫描 - 在上次结果中筛选。

        Args:
            value: 要搜索的值
            data_type: byte/word/dword/qword/float/double
            scan_type: exact/increased/decreased/changed/unchanged/
                       increased_by/decreased_by/greater/less
        """
        normalized_scan_type = normalize_scan_type(scan_type, NEXT_SCAN_TYPES)
        flags = make_scan_flags(normalized_scan_type, data_type)
        params = {"flags": flags, "scan_flag": flags}
        if value == "":
            if normalized_scan_type not in NO_VALUE_SCAN_TYPES:
                raise ValueError(f"scan_next with scan_type='{scan_type}' requires value")
        else:
            params["value_hex"] = encode_value_hex(value, data_type)
            if normalized_scan_type == "between":
                if value2 == "":
                    raise ValueError("scan_next with scan_type='between' requires value2")
                params["value2_hex"] = encode_value_hex(value2, data_type)
        r = ipc.call_or_raise("scan_next", params)
        return f"再次扫描完成，剩余 {r['count']} 个结果"

    @mcp.tool()
    def scan_fuzzy(data_type: str = "dword", scan_type: str = "unknown") -> str:
        """模糊扫描 - 搜索未知初始值或值变化。

        Args:
            data_type: byte/word/dword/qword/float/double
            scan_type: unknown/increased/decreased/changed/unchanged
        """
        scan_type = normalize_scan_type(scan_type, FUZZY_SCAN_TYPES)
        flags = make_scan_flags(scan_type, data_type)
        r = ipc.call_or_raise("scan_fuzzy", {"flags": flags})
        return f"模糊扫描完成，找到 {r['count']} 个结果"

    @mcp.tool()
    def scan_hex(hex_pattern: str) -> str:
        """十六进制模式扫描。

        Args:
            hex_pattern: 十六进制字节串，如 "48 65 6C 6C 6F"
        """
        pattern = clean_hex_string(hex_pattern)
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
        offset, count = clamp_page(offset, count)
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
