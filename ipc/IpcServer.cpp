#include "IpcServer.h"
#include "../socket/client_singleton.h"
#include "../gui/AppContext.h"
#include "../gui/Gui.h"

#ifdef HAVE_LUAJIT
#include "../lua/LuaEngine.h"
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <sstream>
#include <algorithm>
#include <iomanip>

#pragma comment(lib, "ws2_32.lib")

// ── 单例 ─────────────────────────────────────────────────────────
IpcServer& IpcServer::GetInstance() {
    static IpcServer instance;
    return instance;
}

// ── 启动/停止 ────────────────────────────────────────────────────
bool IpcServer::Start(uint16_t port) {
    if (running_.load()) return true;

    port_ = port;
    RegisterBuiltinMethods();

    // 确保 Winsock 已初始化（可能在 WindowsSocketClient 之前启动）
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    listenSocket_ = (uintptr_t)::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket_ == (uintptr_t)INVALID_SOCKET) {
        Gui::log("[IPC] 创建 socket 失败");
        return false;
    }

    // 允许端口复用
    int opt = 1;
    ::setsockopt((SOCKET)listenSocket_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 仅本地

    if (::bind((SOCKET)listenSocket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        Gui::log("[IPC] 绑定端口 %d 失败", (int)port_);
        ::closesocket((SOCKET)listenSocket_);
        listenSocket_ = (uintptr_t)INVALID_SOCKET;
        return false;
    }

    if (::listen((SOCKET)listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
        Gui::log("[IPC] listen 失败");
        ::closesocket((SOCKET)listenSocket_);
        listenSocket_ = (uintptr_t)INVALID_SOCKET;
        return false;
    }

    running_.store(true);
    serverThread_ = std::thread(&IpcServer::ServerThread, this);
    Gui::log("[IPC] 服务已启动，监听端口 %d", (int)port_);
    return true;
}
void IpcServer::Stop() {
    if (!running_.load()) return;
    running_.store(false);
    Gui::log("[IPC] 服务正在停止...");

    // 关闭监听 socket 以唤醒 accept()
    if (listenSocket_ != (uintptr_t)INVALID_SOCKET) {
        ::closesocket((SOCKET)listenSocket_);
        listenSocket_ = (uintptr_t)INVALID_SOCKET;
    }

    if (serverThread_.joinable())
        serverThread_.join();
}

void IpcServer::ServerThread() {
    while (running_.load()) {
        SOCKET client = ::accept((SOCKET)listenSocket_, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;

        // 每个请求在独立线程处理（短连接）
        std::thread([this, client]() {
            HandleClient((uintptr_t)client);
        }).detach();
    }
}

// ── HTTP 处理 ────────────────────────────────────────────────────
void IpcServer::HandleClient(uintptr_t clientSocket) {
    SOCKET sock = (SOCKET)clientSocket;

    // 设置超时
    DWORD timeout = 5000;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    // 读取完整 HTTP 请求（最大 1MB）
    std::string raw;
    raw.reserve(4096);
    char buf[4096];
    int contentLength = -1;
    size_t headerEnd = std::string::npos;

    while (true) {
        int n = ::recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, n);

        // 查找 header 结束
        if (headerEnd == std::string::npos) {
            headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                // 解析 Content-Length
                auto clPos = raw.find("Content-Length:");
                if (clPos == std::string::npos)
                    clPos = raw.find("content-length:");
                if (clPos != std::string::npos) {
                    contentLength = std::atoi(raw.c_str() + clPos + 15);
                } else {
                    contentLength = 0;
                }
            }
        }

        if (headerEnd != std::string::npos) {
            size_t bodyStart = headerEnd + 4;
            if ((int)(raw.size() - bodyStart) >= contentLength) break;
        }

        if (raw.size() > 1024 * 1024) break; // 防止过大
    }

    // 提取 body
    std::string body;
    if (headerEnd != std::string::npos)
        body = raw.substr(headerEnd + 4);

    // 处理 CORS preflight
    if (raw.substr(0, 7) == "OPTIONS") {
        std::string resp = "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Content-Length: 0\r\n\r\n";
        ::send(sock, resp.c_str(), (int)resp.size(), 0);
        ::closesocket(sock);
        return;
    }

    // 解析 JSON 并分发
    json response;
    int statusCode = 200;
    try {
        json request = json::parse(body);
        response = DispatchRequest(request);
    } catch (const json::parse_error& e) {
        statusCode = 400;
        response = {{"success", false}, {"error", std::string("JSON 解析错误: ") + e.what()}};
        Gui::log("[IPC] JSON 解析错误: %s", e.what());
    } catch (const std::exception& e) {
        statusCode = 500;
        response = {{"success", false}, {"error", e.what()}};
        Gui::log("[IPC] 请求处理异常: %s", e.what());
    }

    std::string httpResp = BuildHttpResponse(statusCode, response.dump());
    ::send(sock, httpResp.c_str(), (int)httpResp.size(), 0);
    ::closesocket(sock);
}

std::string IpcServer::BuildHttpResponse(int statusCode, const std::string& body) {
    const char* reason = "OK";
    switch (statusCode) {
    case 200: reason = "OK"; break;
    case 204: reason = "No Content"; break;
    case 400: reason = "Bad Request"; break;
    case 404: reason = "Not Found"; break;
    case 500: reason = "Internal Server Error"; break;
    default:  reason = "Unknown"; break;
    }

    std::ostringstream oss;
    oss << "HTTP/1.1 " << statusCode << " " << reason << "\r\n"
        << "Content-Type: application/json; charset=utf-8\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    return oss.str();
}

json IpcServer::DispatchRequest(const json& request) {
    if (!request.contains("method") || !request["method"].is_string()) {
        return {{"success", false}, {"error", "缺少 method 字段"}};
    }

    std::string method = request["method"].get<std::string>();
    json params = request.value("params", json::object());

    Gui::log("[IPC] 收到请求: %s", method.c_str());

    auto it = handlers_.find(method);
    if (it == handlers_.end()) {
        Gui::log("[IPC] 未知方法: %s", method.c_str());
        return {{"success", false}, {"error", "未知方法: " + method}};
    }

    try {
        json result = it->second(params);
        bool ok = result.value("success", false);
        if (!ok) {
            std::string err = result.value("error", "");
            Gui::log("[IPC] %s 失败: %s", method.c_str(), err.c_str());
        }
        return result;
    } catch (const std::exception& e) {
        Gui::log("[IPC] %s 异常: %s", method.c_str(), e.what());
        return {{"success", false}, {"error", std::string("执行错误: ") + e.what()}};
    }
}

void IpcServer::RegisterMethod(const std::string& method, Handler handler) {
    handlers_[method] = std::move(handler);
}

// ── 辅助：bytes <-> hex string ───────────────────────────────────
static std::string BytesToHex(const std::vector<unsigned char>& data) {
    std::ostringstream oss;
    for (auto b : data) oss << std::hex << std::setfill('0') << std::setw(2) << (int)b;
    return oss.str();
}

static std::vector<unsigned char> HexToBytes(const std::string& hex) {
    std::vector<unsigned char> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back((unsigned char)std::stoi(hex.substr(i, 2), nullptr, 16));
    }
    return out;
}

static uint64_t ParseAddress(const json& params, const std::string& key) {
    auto& v = params.at(key);
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        return std::stoull(s, nullptr, (s.size() > 2 && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10);
    }
    return v.get<uint64_t>();
}

// ── 注册所有内置路由 ─────────────────────────────────────────────
void IpcServer::RegisterBuiltinMethods() {

    // ── get_status ───────────────────────────────────────────────
    RegisterMethod("get_status", [](const json&) -> json {
        auto& ctx = AppContext::Get();
        bool connected = IsMultiPortConnected();
        return {
            {"success", true},
            {"result", {
                {"connected", connected},
                {"pid", ctx.selectedPid.load()},
                {"process_name", ctx.getSelectedName()},
                {"handle", ctx.processHandle.load()}
            }}
        };
    });

    // ── get_version ──────────────────────────────────────────────
    RegisterMethod("get_version", [](const json&) -> json {
        ServerVersionInfo info;
        if (!FetchServerVersion(info))
            return {{"success", false}, {"error", "获取版本失败"}};
        return {{"success", true}, {"result", {
            {"version", info.version},
            {"version_string", info.versionString}
        }}};
    });

    // ── get_architecture ─────────────────────────────────────────
    RegisterMethod("get_architecture", [](const json&) -> json {
        int type = 0;
        if (!GetMemType(type))
            return {{"success", false}, {"error", "获取架构失败"}};
        const char* names[] = {"Null", "IO", "Syscall", "Kernel", "SysHook"};
        std::string name = (type >= 0 && type <= 4) ? names[type] : "Unknown";
        return {{"success", true}, {"result", {{"type", type}, {"name", name}}}};
    });

    // ── init_driver ──────────────────────────────────────────────
    RegisterMethod("init_driver", [](const json& p) -> json {
        std::string card = p.value("card", "");
        std::string resStr;
        if (!InitDriver(card, resStr))
            return {{"success", false}, {"error", "初始化驱动失败: " + resStr}};
        return {{"success", true}, {"result", {{"message", resStr}}}};
    });

    // ── list_processes ───────────────────────────────────────────
    RegisterMethod("list_processes", [](const json&) -> json {
        std::vector<ProcessInfoItem> list;
        if (!FetchProcessList(list))
            return {{"success", false}, {"error", "获取进程列表失败"}};
        json arr = json::array();
        for (auto& p : list)
            arr.push_back({{"pid", p.pid}, {"name", p.name}});
        return {{"success", true}, {"result", arr}};
    });
    // ── open_process ──────────────────────────────────────────────
    RegisterMethod("open_process", [](const json& p) -> json {
        int pid = p.at("pid").get<int>();
        AppContext::Get().selectProcess(pid, "");
        int handle = AppContext::Get().processHandle.load(std::memory_order_relaxed);
        if (handle == 0)
            return {{"success", false}, {"error", "打开进程失败"}};
        return {{"success", true}, {"result", {{"handle", handle}}}};
    });

    // ── list_modules ─────────────────────────────────────────────
    RegisterMethod("list_modules", [](const json& p) -> json {
        std::vector<ModuleInfoItem> list;
        if (!FetchModuleList(list))
            return {{"success", false}, {"error", "获取模块列表失败"}};

        // 可选：名称过滤（大小写不敏感子串匹配）
        std::string filter = p.value("filter", "");
        std::vector<ModuleInfoItem*> filtered;
        if (!filter.empty()) {
            std::string lowerFilter = filter;
            std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(),
                [](unsigned char c) { return (char)std::tolower(c); });
            for (auto& m : list) {
                std::string lowerName = m.name;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                    [](unsigned char c) { return (char)std::tolower(c); });
                if (lowerName.find(lowerFilter) != std::string::npos)
                    filtered.push_back(&m);
            }
        } else {
            for (auto& m : list) filtered.push_back(&m);
        }

        int total = (int)filtered.size();
        int offset = p.value("offset", 0);
        int count = p.value("count", 200);
        if (count > 1000) count = 1000;
        if (offset < 0) offset = 0;
        if (offset > total) offset = total;
        int end = (std::min)(offset + count, total);

        json arr = json::array();
        for (int i = offset; i < end; i++) {
            auto* m = filtered[i];
            std::ostringstream baseStr;
            baseStr << "0x" << std::hex << m->base;
            arr.push_back({
                {"base", baseStr.str()}, {"size", m->size},
                {"type", m->type}, {"flag", m->flag}, {"name", m->name}
            });
        }
        return {{"success", true}, {"result", {{"total", total}, {"offset", offset}, {"modules", arr}}}};
    });

    // ── read_memory ──────────────────────────────────────────────
    RegisterMethod("read_memory", [](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        uint32_t size = p.value("size", 256u);
        if (size > 65536) size = 65536;
        std::vector<unsigned char> data;
        if (!ReadProcessMemoryBytes(addr, size, data))
            return {{"success", false}, {"error", "读取内存失败"}};
        return {{"success", true}, {"result", {
            {"hex", BytesToHex(data)}, {"size", (int)data.size()}
        }}};
    });

    // ── write_memory ─────────────────────────────────────────────
    RegisterMethod("write_memory", [](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        std::string hexStr = p.at("hex").get<std::string>();
        auto data = HexToBytes(hexStr);
        uint32_t size = (uint32_t)data.size();
        if (!WriteProcessMemoryBytes(addr, size, data))
            return {{"success", false}, {"error", "写入内存失败"}};
        return {{"success", true}, {"result", {{"written", (int)size}}}};
    });

    // ── read_batch ───────────────────────────────────────────────
    RegisterMethod("read_batch", [](const json& p) -> json {
        auto& addrsArr = p.at("addresses");
        std::vector<std::pair<uint64_t, int32_t>> addrs;
        for (auto& item : addrsArr) {
            uint64_t a = ParseAddress(item, "address");
            int32_t s = item.value("size", 4);
            addrs.push_back({a, s});
        }
        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> out;
        if (!ReadBratchAddr(addrs, out))
            return {{"success", false}, {"error", "批量读取失败"}};
        json arr = json::array();
        for (auto& [a, d] : out) {
            std::ostringstream addrStr;
            addrStr << "0x" << std::hex << a;
            std::vector<unsigned char> uc(d.begin(), d.end());
            arr.push_back({{"address", addrStr.str()}, {"hex", BytesToHex(uc)}});
        }
        return {{"success", true}, {"result", arr}};
    });
    // ── scan_set_range ────────────────────────────────────────────
    RegisterMethod("scan_set_range", [](const json& p) -> json {
        int type = p.value("type", -1); // -1 = MEM_ALL
        if (!ScanSetRange(type))
            return {{"success", false}, {"error", "设置扫描范围失败"}};
        return {{"success", true}, {"result", nullptr}};
    });

    // ── scan_value ───────────────────────────────────────────────
    RegisterMethod("scan_value", [](const json& p) -> json {
        uint32_t flags = p.at("flags").get<uint32_t>();
        std::string hexVal = p.at("value_hex").get<std::string>();
        auto valBytes = HexToBytes(hexVal);
        uint64_t start = 0, end = UINT64_MAX;
        if (p.contains("start")) start = ParseAddress(p, "start");
        if (p.contains("end")) end = ParseAddress(p, "end");
        int count = ScanValueWithProgress(flags, valBytes, nullptr, nullptr, start, end);
        if (count < 0)
            return {{"success", false}, {"error", "扫描失败"}};
        return {{"success", true}, {"result", {{"count", count}}}};
    });

    // ── scan_next ────────────────────────────────────────────────
    RegisterMethod("scan_next", [](const json& p) -> json {
        uint32_t flags = p.at("flags").get<uint32_t>();
        std::string hexVal = p.at("value_hex").get<std::string>();
        auto valBytes = HexToBytes(hexVal);
        int flag = p.value("scan_flag", static_cast<int>(flags));
        uint64_t start = 0, end = UINT64_MAX;
        if (p.contains("start")) start = ParseAddress(p, "start");
        if (p.contains("end")) end = ParseAddress(p, "end");
        int count = ScanNextValueWithProgress(valBytes, flag, nullptr, nullptr, start, end);
        if (count < 0)
            return {{"success", false}, {"error", "再次扫描失败"}};
        return {{"success", true}, {"result", {{"count", count}}}};
    });

    // ── scan_fuzzy ───────────────────────────────────────────────
    RegisterMethod("scan_fuzzy", [](const json& p) -> json {
        uint32_t flags = p.at("flags").get<uint32_t>();
        uint64_t start = 0, end = UINT64_MAX;
        if (p.contains("start")) start = ParseAddress(p, "start");
        if (p.contains("end")) end = ParseAddress(p, "end");
        int count = ScanFuzzyValueWithProgress(flags, nullptr, nullptr, start, end);
        if (count < 0)
            return {{"success", false}, {"error", "模糊扫描失败"}};
        return {{"success", true}, {"result", {{"count", count}}}};
    });

    // ── scan_hex ─────────────────────────────────────────────────
    RegisterMethod("scan_hex", [](const json& p) -> json {
        std::string hexPattern = p.at("pattern_hex").get<std::string>();
        auto patternBytes = HexToBytes(hexPattern);
        uint64_t start = 0, end = UINT64_MAX;
        if (p.contains("start")) start = ParseAddress(p, "start");
        if (p.contains("end")) end = ParseAddress(p, "end");
        int count = ScanHEXValueWithProgress(start, end, patternBytes, nullptr, nullptr);
        if (count < 0)
            return {{"success", false}, {"error", "HEX 扫描失败"}};
        return {{"success", true}, {"result", {{"count", count}}}};
    });

    // ── get_scan_count ───────────────────────────────────────────
    RegisterMethod("get_scan_count", [](const json&) -> json {
        int count = GetScanResultCount();
        if (count < 0)
            return {{"success", false}, {"error", "获取扫描结果数量失败"}};
        return {{"success", true}, {"result", {{"count", count}}}};
    });

    // ── get_scan_results ─────────────────────────────────────────
    RegisterMethod("get_scan_results", [](const json& p) -> json {
        int total = GetScanResultCount();
        if (total < 0)
            return {{"success", false}, {"error", "获取扫描结果数量失败"}};
        int offset = p.value("offset", 0);
        int count = p.value("count", 20);
        if (count > 1000) count = 1000;
        if (offset < 0) offset = 0;
        if (offset >= total) {
            return {{"success", true}, {"result", {{"total", total}, {"offset", offset}, {"items", json::array()}}}};
        }
        std::vector<std::pair<uint64_t, uint64_t>> results;
        if (!GetScanResult(offset, count, results))
            return {{"success", false}, {"error", "获取扫描结果失败"}};
        json arr = json::array();
        for (auto& [addr, val] : results) {
            std::ostringstream addrStr;
            addrStr << "0x" << std::hex << addr;
            arr.push_back({{"address", addrStr.str()}, {"value", val}});
        }
        return {{"success", true}, {"result", {{"total", total}, {"offset", offset}, {"items", arr}}}};
    });

    // ── clear_scan ───────────────────────────────────────────────
    RegisterMethod("clear_scan", [](const json&) -> json {
        if (!ClearScanResult())
            return {{"success", false}, {"error", "清空扫描结果失败"}};
        return {{"success", true}, {"result", nullptr}};
    });
    // ── set_breakpoint ────────────────────────────────────────────
    RegisterMethod("set_breakpoint", [](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        uint32_t bpType = p.value("bp_type", 1u);
        uint32_t bpSize = p.value("bp_size", 4u);
        if (!SetKernelBreakpoint(addr, bpType, bpSize))
            return {{"success", false}, {"error", "设置断点失败"}};
        return {{"success", true}, {"result", nullptr}};
    });

    // ── remove_breakpoint ────────────────────────────────────────
    RegisterMethod("remove_breakpoint", [](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        if (!RemoveKernelBreakpoint(addr))
            return {{"success", false}, {"error", "移除断点失败"}};
        return {{"success", true}, {"result", nullptr}};
    });

    // ── suspend_breakpoint ───────────────────────────────────────
    RegisterMethod("suspend_breakpoint", [](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        if (!SuspendKernelBreakpoint(addr))
            return {{"success", false}, {"error", "暂停断点失败"}};
        return {{"success", true}, {"result", nullptr}};
    });

    // ── resume_breakpoint ────────────────────────────────────────
    RegisterMethod("resume_breakpoint", [](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        if (!ResumeKernelBreakpoint(addr))
            return {{"success", false}, {"error", "恢复断点失败"}};
        return {{"success", true}, {"result", nullptr}};
    });

    // ── read_bp_info ─────────────────────────────────────────────
    RegisterMethod("read_bp_info", [](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        std::vector<HW_HIT_INFO> infos;
        if (!ReadKernelBreakpointInfo(addr, infos))
            return {{"success", false}, {"error", "读取断点信息失败"}};
        json arr = json::array();
        for (auto& h : infos) {
            json regs = json::array();
            for (int i = 0; i < 31; i++)
                regs.push_back(h.regs_info.regs[i]);
            std::ostringstream hitStr, pcStr, spStr;
            hitStr << "0x" << std::hex << h.hit_addr;
            pcStr << "0x" << std::hex << h.regs_info.pc;
            spStr << "0x" << std::hex << h.regs_info.sp;
            arr.push_back({
                {"hit_addr", hitStr.str()},
                {"hit_time", h.hit_time},
                {"pc", pcStr.str()},
                {"sp", spStr.str()},
                {"pstate", h.regs_info.pstate},
                {"regs", regs}
            });
        }
        return {{"success", true}, {"result", arr}};
    });
    // ── execute_lua ───────────────────────────────────────────────
#ifdef HAVE_LUAJIT
    RegisterMethod("execute_lua", [](const json& p) -> json {
        std::string code = p.at("code").get<std::string>();
        auto& engine = LuaEngine::GetInstance();
        if (!engine.IsInitialized()) {
            if (!engine.Initialize())
                return {{"success", false}, {"error", "Lua 引擎初始化失败: " + engine.GetLastError()}};
        }
        std::string output;
        bool ok = engine.ExecuteStringCapture(code, "ipc", output);
        if (!ok)
            return {{"success", false}, {"error", engine.GetLastError()}, {"output", output}};
        return {{"success", true}, {"result", {{"output", output}}}};
    });
#endif

    // ── get_module_base ──────────────────────────────────────────
    RegisterMethod("get_module_base", [](const json& p) -> json {
        std::string name = p.at("name").get<std::string>();
        uint64_t base = 0;
        if (!GetModuleBaseByName(name, base))
            return {{"success", false}, {"error", "获取模块基址失败"}};
        std::ostringstream oss;
        oss << "0x" << std::hex << base;
        return {{"success", true}, {"result", {{"base", oss.str()}}}};
    });

    // ── resolve_offset_chain ─────────────────────────────────────
    RegisterMethod("resolve_offset_chain", [](const json& p) -> json {
        std::string moduleName = p.at("module").get<std::string>();
        uint64_t baseOffset = ParseAddress(p, "base_offset");
        std::vector<uint64_t> offsets;
        if (p.contains("offsets")) {
            for (auto& o : p["offsets"])
                offsets.push_back(o.get<uint64_t>());
        }
        bool derefFinal = p.value("deref_final", true);
        uint64_t result = 0;
        if (!ResolveModuleOffsetChain(result, moduleName, baseOffset, offsets, derefFinal))
            return {{"success", false}, {"error", "解析偏移链失败"}};
        std::ostringstream oss;
        oss << "0x" << std::hex << result;
        return {{"success", true}, {"result", {{"address", oss.str()}}}};
    });

    // ── symbol_init ────────────────────────────────────────────────
    RegisterMethod("symbol_init", [](const json& p) -> json {
        uint64_t moduleBase = ParseAddress(p, "module_base");
        int totalCount = 0;
        if (!SymbolInit(moduleBase, totalCount))
            return {{"success", false}, {"error", "初始化符号表失败"}};
        return {{"success", true}, {"result", {{"total_count", totalCount}}}};
    });

    // ── symbol_list ────────────────────────────────────────────────
    RegisterMethod("symbol_list", [](const json& p) -> json {
        int offset = p.value("offset", 0);
        int count = p.value("count", 100);
        if (offset < 0) offset = 0;
        if (count < 0) count = 0;
        if (count > 1000) count = 1000;

        int totalCount = 0;
        std::vector<std::pair<uint64_t, std::string>> symbols;
        if (!SymbolGetList(offset, count, symbols, &totalCount))
            return {{"success", false}, {"error", "获取符号列表失败，请先初始化符号表"}};

        json arr = json::array();
        for (const auto& [address, name] : symbols) {
            std::ostringstream addrStr;
            addrStr << "0x" << std::hex << address;
            arr.push_back({
                {"address", addrStr.str()},
                {"name", name}
            });
        }
        return {{"success", true}, {"result", {
            {"total", totalCount},
            {"offset", offset},
            {"symbols", arr}
        }}};
    });

    // ── symbol_find ────────────────────────────────────────────────
    RegisterMethod("symbol_find", [](const json& p) -> json {
        uint64_t moduleBase = ParseAddress(p, "module_base");
        std::string name = p.at("name").get<std::string>();
        uint64_t address = 0;
        if (!SymbolFind(moduleBase, name, address) || address == 0)
            return {{"success", false}, {"error", "查找符号失败"}};

        std::ostringstream oss;
        oss << "0x" << std::hex << address;
        return {{"success", true}, {"result", {{"address", oss.str()}}}};
    });

} // RegisterBuiltinMethods
