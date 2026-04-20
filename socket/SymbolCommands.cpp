#include "client_singleton.h"
#include "SocketCommand.h"

bool SymbolInit(uint64_t moduleBase, int &outTotalCount, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_SYMBOL_INIT;
        if (!client->Send(&command, sizeof(command)))
            return false;
        CeSymbolInitInput input{};
        input.hProcess = static_cast<uint32_t>(handle);
        input.moduleBase = moduleBase;
        if (!client->Send(&input, sizeof(input)))
            return false;
        CeSymbolInitOutput output{};
        if (!client->Receive(&output, sizeof(output)))
            return false;
        outTotalCount = output.totalCount;
        return output.result == 0;
    });
}

bool SymbolGetList(int offset, int count,
                   std::vector<std::pair<uint64_t, std::string>> &outSymbols,
                   int *outTotalCount, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int) -> bool {
        unsigned char command = CMD_SYMBOL_GETLIST;
        if (!client->Send(&command, sizeof(command)))
            return false;
        CeGetSymbolListInput input{};
        input.offset = offset;
        input.count = count;
        if (!client->Send(&input, sizeof(input)))
            return false;
        CeGetSymbolListOutput output{};
        if (!client->Receive(&output, sizeof(output)))
            return false;
        if (outTotalCount)
            *outTotalCount = output.totalCount;
        outSymbols.clear();
        outSymbols.reserve(output.actualCount);
        for (int i = 0; i < output.actualCount; ++i) {
            CeSymbolEntry entry{};
            if (!client->Receive(&entry, sizeof(entry)))
                return false;
            std::string name;
            if (entry.nameSize > 0) {
                name.resize(entry.nameSize);
                if (!client->Receive(name.data(), entry.nameSize))
                    return false;
            }
            outSymbols.emplace_back(entry.address, std::move(name));
        }
        return true;
    });
}

bool SymbolFind(uint64_t moduleBase, const std::string &name,
                uint64_t &outAddress, PortType port) {
    outAddress = 0;
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_SYMBOL_FIND;
        if (!client->Send(&command, sizeof(command)))
            return false;
        CeFindSymbolInput input{};
        input.hProcess = static_cast<uint32_t>(handle);
        input.moduleBase = moduleBase;
        input.nameSize = static_cast<int>(name.size());
        if (!client->Send(&input, sizeof(input)))
            return false;
        if (input.nameSize > 0 && !client->Send(name.data(), name.size()))
            return false;
        CeFindSymbolOutput output{};
        if (!client->Receive(&output, sizeof(output)))
            return false;
        outAddress = output.address;
        return output.result == 0;
    });
}
