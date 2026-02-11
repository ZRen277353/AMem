#pragma once

#include <cstdint>
#include <string>

// 跨窗口事件定义

struct ProcessSelectedEvent {
    int pid;
    std::string name;
};

struct NavigateToAddressEvent {
    uint64_t address;
};
