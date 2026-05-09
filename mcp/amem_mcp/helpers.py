"""值编码、hex dump、扫描 flag 组合等辅助函数。"""

from __future__ import annotations

import struct

from .constants import (
    DATA_TYPE_FMT,
    DATA_TYPE_MAP,
    DATA_TYPE_SIZE,
    SCAN1_ACCURATE,
    SCAN_TYPE_MAP,
    TYPE_DWORD,
)


def parse_int(value: str | int) -> int:
    """接受十进制或 0x/0X 前缀十六进制字符串。"""
    if isinstance(value, int):
        return value
    s = str(value).strip()
    if s.lower().startswith("0x"):
        return int(s, 16)
    return int(s)


def encode_value_hex(value: str, data_type: str) -> str:
    """按数据类型将值编码为小端 hex 字符串。"""
    fmt = DATA_TYPE_FMT.get(data_type, "<I")
    v = float(value) if data_type in ("float", "double") else int(value)
    return struct.pack(fmt, v).hex()


def decode_value(hex_data: str, data_type: str):
    """从 hex 字符串还原为原始值。"""
    sz = DATA_TYPE_SIZE.get(data_type, 4)
    fmt = DATA_TYPE_FMT.get(data_type, "<I")
    data = bytes.fromhex(hex_data)
    if len(data) < sz:
        return None
    return struct.unpack(fmt, data[:sz])[0]


def make_scan_flags(scan_type: str, data_type: str) -> int:
    """组合 SCAN1_* 和 TYPE_* 位 flag。"""
    return (
        SCAN_TYPE_MAP.get(scan_type, SCAN1_ACCURATE)
        | DATA_TYPE_MAP.get(data_type, TYPE_DWORD)
    )


def hex_dump(hex_str: str, base_addr: int, width: int = 16) -> str:
    """将 hex 字符串渲染为类 xxd 的 dump 格式。"""
    data = bytes.fromhex(hex_str)
    lines = []
    for i in range(0, len(data), width):
        chunk = data[i:i + width]
        hex_part = " ".join(f"{b:02X}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{base_addr + i:012X}  {hex_part:<{width * 3}}  {ascii_part}")
    return "\n".join(lines)
