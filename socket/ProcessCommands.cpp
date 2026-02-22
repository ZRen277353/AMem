#include "client_singleton.h"
#include "SocketCommand.h"
#include "../gui/AppContext.h"
#include <iostream>
#include <ctime>

bool GetMemType(int &outType, PortType type) {
    return SocketCommand::executeNoHandle(type, [&](WindowsSocketClient* client) -> bool {
        unsigned char command = CMD_GETMEMTYPE;
        if (!client->Send(&command, sizeof(command)))
            return false;
        unsigned char t = 0;
        if (!client->Receive(&t, sizeof(t)))
            return false;
        outType = t;
        return true;
    });
}

bool InitDriver(std::string &Card, std::string &resStr, PortType type) {
    int ret = 0;
    int resStrlen = 0;
    std::vector<char> resStrVec;

    bool success = SocketCommand::executeNoHandle(type, [&](WindowsSocketClient* client) -> bool {
        unsigned char command = CMD_INITRWDRIVER;
        if (!client->Send(&command, sizeof(command)))
            return false;
        int Cardlen = Card.size();
        if (!client->Send(&Cardlen, sizeof(Cardlen)))
            return false;
        if (!client->Send(Card.data(), Card.size()))
            return false;
        if (!client->Receive(&ret, sizeof(ret)))
            return false;
        if (!client->Receive(&resStrlen, sizeof(resStrlen)))
            return false;
        if (resStrlen == 0)
            return false;
        resStrVec.resize(resStrlen);
        if (!client->Receive(resStrVec.data(), resStrlen))
            return false;
        return true;
    });

    if (!success)
        return false;

    if (ret > 0) {
        try {
            std::string timestampStr(resStrVec.data(), resStrVec.size());
            uint64_t timestamp_ms = std::stoull(timestampStr);
            time_t timestamp = static_cast<time_t>(timestamp_ms / 1000);
            if (timestamp > 0) {
                std::tm *timeinfo = std::localtime(&timestamp);
                if (timeinfo != nullptr) {
                    char dateTime[20];
                    std::strftime(dateTime, sizeof(dateTime), "%Y-%m-%d %H:%M:%S", timeinfo);
                    resStr = dateTime;
                } else {
                    resStr = "时间格式化失败";
                }
            } else {
                resStr = "无效的时间戳";
            }
        } catch (const std::exception &) {
            resStr = "时间戳解析失败";
        }
    } else {
        resStr.assign(resStrVec.data(), resStrVec.size());
    }
    return true;
}

bool FetchServerVersion(ServerVersionInfo &outInfo, PortType type) {
    return SocketCommand::executeNoHandle(type, [&](WindowsSocketClient* client) -> bool {
        unsigned char command = CMD_GETVERSION;
        if (!client->Send(&command, sizeof(command)))
            return false;
        CeVersion version{};
        if (!client->Receive(&version, sizeof(version)))
            return false;
        outInfo.version = version.version;
        outInfo.versionString.clear();
        if (version.stringsize > 0) {
            std::vector<char> versionString(version.stringsize);
            if (client->Receive(versionString.data(), versionString.size())) {
                outInfo.versionString.assign(versionString.data(), versionString.size());
            }
        }
        return true;
    });
}

bool FetchProcessList(std::vector<ProcessInfoItem> &outList, PortType type) {
    return SocketCommand::executeNoHandle(type, [&](WindowsSocketClient* client) -> bool {
        unsigned char command = CMD_GETPROCESSLIST;
        if (!client->Send(&command, sizeof(command)))
            return false;
        int len = 0;
        if (!client->Receive(&len, 4))
            return false;
        outList.clear();
        while (len--) {
            struct { int pid; int size; } proc{};
            if (!client->Receive(&proc, sizeof(proc)))
                break;
            std::vector<char> name(proc.size);
            if (!client->Receive(name.data(), proc.size))
                break;
            ProcessInfoItem item{};
            item.pid = proc.pid;
            item.name.assign(name.data(), name.size());
            outList.push_back(std::move(item));
        }
        return true;
    });
}

bool FetchModuleList(std::vector<ModuleInfoItem> &outList, PortType type) {
    return SocketCommand::execute(type, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_GETMODULELIST;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        int len = 0;
        if (!client->Receive(&len, 4))
            return false;
        CeModuleListEntry entry{};
        outList.clear();
        while (len > 0) {
            std::memset(&entry, 0, sizeof(entry));
            if (!client->Receive(&entry, sizeof(entry)))
                break;
            std::vector<char> name(entry.modulenamesize);
            if (!client->Receive(name.data(), entry.modulenamesize))
                break;
            ModuleInfoItem mi{};
            mi.base = entry.modulebase;
            mi.size = entry.modulesize;
            mi.type = entry.result;
            mi.flag = entry.flag;
            mi.name = "";
            if (entry.modulenamesize > 0) {
                mi.name.assign(name.data(), entry.modulenamesize);
            }
            outList.push_back(std::move(mi));
            len--;
        }
        return true;
    });
}

bool GetModuleBaseByName(const std::string &moduleName, uint64_t &outBase, PortType port) {
    auto* client = GetSocketMgr().GetClient(port);
    if (!client || !client->IsConnected())
        return false;
    std::vector<ModuleInfoItem> mods;
    if (!FetchModuleList(mods, port))
        return false;
    for (const auto &m : mods) {
        if (_stricmp(m.name.c_str(), moduleName.c_str()) == 0) {
            outBase = m.base;
            return true;
        }
    }
    return false;
}

static bool read_u64(uint64_t address, uint64_t &value) {
    std::vector<unsigned char> buf;
    if (!ReadProcessMemoryBytes(address, 8, buf))
        return false;
    if (buf.size() < 8)
        return false;
    value = (uint64_t)buf[0] | ((uint64_t)buf[1] << 8) |
            ((uint64_t)buf[2] << 16) | ((uint64_t)buf[3] << 24) |
            ((uint64_t)buf[4] << 32) | ((uint64_t)buf[5] << 40) |
            ((uint64_t)buf[6] << 48) | ((uint64_t)buf[7] << 56);
    return true;
}

bool ResolveModuleOffsetChain(uint64_t &outAddress, const std::string &moduleName,
                              uint64_t baseOffset, const std::vector<uint64_t> &offsets,
                              bool derefFinal, PortType port) {
    uint64_t base = 0;
    if (!GetModuleBaseByName(moduleName, base, port))
        return false;
    uint64_t addr = base + baseOffset;
    if (offsets.empty()) {
        outAddress = addr;
        return true;
    }
    for (size_t i = 0; i < offsets.size(); ++i) {
        uint64_t ptr = 0;
        if (!read_u64(addr, ptr))
            return false;
        addr = ptr + offsets[i];
    }
    if (derefFinal) {
        uint64_t finalPtr = 0;
        if (!read_u64(addr, finalPtr))
            return false;
        addr = finalPtr;
    }
    outAddress = addr;
    return true;
}
