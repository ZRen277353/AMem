"""值编码、hex dump、扫描 flag 组合等辅助函数。"""

from __future__ import annotations

import struct

from .constants import DATA_TYPE_FMT, DATA_TYPE_MAP, DATA_TYPE_SIZE, SCAN_TYPE_MAP


def require_positive(value: int, name: str) -> int:
    """要求整数参数为正数。"""
    if value <= 0:
        raise ValueError(f"{name} 必须大于 0")
    return value


def require_non_negative(value: int, name: str) -> int:
    """要求整数参数非负。"""
    if value < 0:
        raise ValueError(f"{name} 不能为负数")
    return value


def clamp_limit(value: int, limit: int, name: str, *, allow_zero: bool = False) -> int:
    """校验整数下界并限制最大值。"""
    if allow_zero:
        require_non_negative(value, name)
    else:
        require_positive(value, name)
    return min(value, limit)


def parse_int(value: str | int) -> int:
    """接受十进制或 0x/0X 前缀十六进制字符串。"""
    if isinstance(value, int):
        return value
    s = str(value).strip()
    if not s:
        raise ValueError("整数参数不能为空")
    return int(s, 0)


def parse_address(value: str | int, name: str = "address") -> int:
    """解析并校验地址/偏移类参数。"""
    parsed = parse_int(value)
    return require_non_negative(parsed, name)


def normalize_data_type(data_type: str) -> str:
    """校验并规范化数据类型。"""
    key = str(data_type).strip().lower()
    if key not in DATA_TYPE_FMT:
        valid = ", ".join(sorted(DATA_TYPE_FMT))
        raise ValueError(f"未知数据类型: {data_type}，合法值: {valid}")
    return key


def normalize_scan_type(scan_type: str) -> str:
    """校验并规范化扫描类型。"""
    key = str(scan_type).strip().lower()
    if key not in SCAN_TYPE_MAP:
        valid = ", ".join(sorted(SCAN_TYPE_MAP))
        raise ValueError(f"未知扫描类型: {scan_type}，合法值: {valid}")
    return key


def clean_hex_string(hex_string: str) -> str:
    """清理并校验十六进制字节串。"""
    hex_clean = "".join(str(hex_string).split())
    if not hex_clean:
        raise ValueError("hex 字符串不能为空")
    if len(hex_clean) % 2 != 0:
        raise ValueError("hex 字符串长度必须为偶数")
    try:
        bytes.fromhex(hex_clean)
    except ValueError as exc:
        raise ValueError(f"无效 hex 字符串: {hex_string}") from exc
    return hex_clean


def encode_value_hex(value: str, data_type: str) -> str:
    """按数据类型将值编码为小端 hex 字符串。"""
    data_type = normalize_data_type(data_type)
    fmt = DATA_TYPE_FMT[data_type]
    v = float(value) if data_type in ("float", "double") else parse_int(value)
    try:
        return struct.pack(fmt, v).hex()
    except struct.error as exc:
        raise ValueError(f"{data_type} 值超出可编码范围: {value}") from exc


def decode_value(hex_data: str, data_type: str):
    """从 hex 字符串还原为原始值。"""
    data_type = normalize_data_type(data_type)
    sz = DATA_TYPE_SIZE[data_type]
    fmt = DATA_TYPE_FMT[data_type]
    data = bytes.fromhex(hex_data)
    if len(data) < sz:
        return None
    return struct.unpack(fmt, data[:sz])[0]


def make_scan_flags(scan_type: str, data_type: str) -> int:
    """组合 SCAN1_* 和 TYPE_* 位 flag。"""
    scan_type = normalize_scan_type(scan_type)
    data_type = normalize_data_type(data_type)
    return SCAN_TYPE_MAP[scan_type] | DATA_TYPE_MAP[data_type]


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
