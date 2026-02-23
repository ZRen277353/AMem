"""
AMem IPC HTTP 客户端
通过 HTTP 调用 AMem GUI 内嵌的 IPC Server (localhost:28100)
"""

import json
import time
import urllib.request
import urllib.error
from typing import Any, Optional


# 耗时操作使用更长超时
_SLOW_METHODS = frozenset({
    "scan_value", "scan_next", "scan_fuzzy", "scan_hex",
    "list_processes", "list_modules",
})


class IpcClient:
    """通过 HTTP JSON 协议与 AMem GUI 通信"""

    def __init__(self, host: str = "127.0.0.1", port: int = 28100):
        self.base_url = f"http://{host}:{port}"

    def call(self, method: str, params: Optional[dict] = None,
             *, retries: int = 2, timeout: Optional[float] = None) -> dict:
        """调用 IPC 方法，返回响应 dict。

        Args:
            method: IPC 方法名
            params: 参数字典
            retries: 最大重试次数（默认 2）
            timeout: 超时秒数（None 则自动选择）
        """
        if timeout is None:
            timeout = 60.0 if method in _SLOW_METHODS else 30.0

        payload = json.dumps({
            "method": method,
            "params": params or {}
        }).encode("utf-8")

        last_error = None
        for attempt in range(1 + retries):
            if attempt > 0:
                time.sleep(0.5)

            req = urllib.request.Request(
                self.base_url,
                data=payload,
                headers={"Content-Type": "application/json"},
                method="POST",
            )

            try:
                with urllib.request.urlopen(req, timeout=timeout) as resp:
                    return json.loads(resp.read().decode("utf-8"))
            except ConnectionRefusedError:
                return {"success": False, "error": "AMem GUI 未启动或 IPC 端口未监听，请先启动 AMem GUI"}
            except urllib.error.URLError as e:
                reason = getattr(e, "reason", e)
                if isinstance(reason, ConnectionRefusedError):
                    return {"success": False, "error": "AMem GUI 未启动或 IPC 端口未监听，请先启动 AMem GUI"}
                if isinstance(reason, TimeoutError):
                    last_error = f"操作超时 ({timeout}s)，方法: {method}"
                    continue  # 超时可重试
                last_error = f"连接 AMem GUI 失败: {reason}"
                continue
            except TimeoutError:
                last_error = f"操作超时 ({timeout}s)，方法: {method}"
                continue
            except json.JSONDecodeError as e:
                return {"success": False, "error": f"GUI 返回了无效的 JSON 响应: {e}"}

        return {"success": False, "error": last_error or "未知错误"}

    def call_or_raise(self, method: str, params: Optional[dict] = None, **kwargs) -> Any:
        """调用并自动检查 success，失败时抛异常"""
        resp = self.call(method, params, **kwargs)
        if not resp.get("success"):
            error_msg = resp.get("error", "未知错误")
            # 如果有 output 字段（如 Lua 执行部分输出），附加到错误信息
            output = resp.get("output", "")
            if output:
                error_msg += f"\n--- 输出 ---\n{output}"
            raise RuntimeError(error_msg)
        return resp.get("result")
