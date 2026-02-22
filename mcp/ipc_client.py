"""
AMem IPC HTTP 客户端
通过 HTTP 调用 AMem GUI 内嵌的 IPC Server (localhost:28100)
"""

import json
import urllib.request
import urllib.error
from typing import Any, Optional


class IpcClient:
    """通过 HTTP JSON 协议与 AMem GUI 通信"""

    def __init__(self, host: str = "127.0.0.1", port: int = 28100):
        self.base_url = f"http://{host}:{port}"

    def call(self, method: str, params: Optional[dict] = None) -> dict:
        """调用 IPC 方法，返回响应 dict"""
        payload = json.dumps({
            "method": method,
            "params": params or {}
        }).encode("utf-8")

        req = urllib.request.Request(
            self.base_url,
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST",
        )

        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.URLError as e:
            return {"success": False, "error": f"连接 AMem GUI 失败: {e}"}
        except json.JSONDecodeError as e:
            return {"success": False, "error": f"JSON 解析错误: {e}"}

    def call_or_raise(self, method: str, params: Optional[dict] = None) -> Any:
        """调用并自动检查 success，失败时抛异常"""
        resp = self.call(method, params)
        if not resp.get("success"):
            raise RuntimeError(resp.get("error", "未知错误"))
        return resp.get("result")
